#include "edge_journal_hle.h"

#include <atomic>
#include <cstddef>

namespace yz::edge_journal {
namespace {

constexpr std::uint32_t kEntrySize = 0x20;
constexpr std::uint32_t kTagMemcpy = 0x10;
constexpr std::uint32_t kTagRelease = 0x7f;
/* [0x11 header][0x0A payload]: one PPU append op, decoded s39 as stage-1
 * bookkeeping with NO memory operation -- triangulated three ways: the 0x11
 * handler reads zero payload bytes and its opcode stamp is dead data, the
 * 0x0A handler only files an EA, and the oracle census shows the consumer's
 * only ring writes are release jump-words (15,951/15,951; no third write
 * class exists). Retire-only. (scratch/s39_tag_decode.md,
 * s39_stage2_trace.md, s39_put_census.md, s39_stage2_probe.md) */
constexpr std::uint32_t kTagHeaderMarker = 0x11;
constexpr std::uint32_t kTagPayloadEa = 0x0a;
constexpr std::uint32_t kMinGuestAddress = 0x00010000;
constexpr std::uint32_t kMaxGuestAddress = 0xe0000000;
constexpr std::uint32_t kMaxArenaSize = 0x01000000;
constexpr std::uint32_t kMaxCopySize = 0x04000000;

bool address_span_valid(std::uint32_t address, std::uint32_t size)
{
    if (address < kMinGuestAddress || address >= kMaxGuestAddress) return false;
    if (size > kMaxCopySize) return false;
    return size <= kMaxGuestAddress - address;
}

std::uint32_t next_entry(std::uint32_t entry, std::uint32_t base,
                         std::uint32_t end)
{
    entry += kEntrySize;
    return entry >= end ? base : entry;
}

void capture_problem(const Io& io, Result& result, std::uint32_t entry,
                     std::uint32_t tag)
{
    result.problem_entry = entry;
    result.problem_tag = tag;
    for (unsigned i = 0; i < 8; ++i)
        result.problem_words[i] = io.read32(io.context, entry + i * 4);
}

std::uint64_t hash_span(const Io& io, std::uint32_t base, std::uint32_t end,
                        std::uint32_t first, std::uint32_t last,
                        std::uint32_t slot_count)
{
    std::uint64_t hash = 1469598103934665603ull;
    std::uint32_t entry = first;
    for (std::uint32_t n = 0; n < slot_count; ++n) {
        for (unsigned i = 0; i < 8; ++i) {
            hash ^= io.read32(io.context, entry + i * 4);
            hash *= 1099511628211ull;
        }
        if (entry == last) return hash;
        entry = next_entry(entry, base, end);
    }
    return 0;
}

}  // namespace

Result apply_through_release(const Io& io,
                             std::uint32_t base,
                             std::uint32_t end,
                             std::uint32_t cursor,
                             std::uint32_t head,
                             std::uint32_t target_stopper_ea)
{
    Result result{};
    if (!io.read32 || !io.write32 || !io.copy_bytes || !io.release_stopper ||
        base < kMinGuestAddress || end <= base || end - base > kMaxArenaSize ||
        ((end - base) % kEntrySize) != 0 ||
        cursor < base || cursor >= end || head < base || head >= end ||
        ((cursor - base) % kEntrySize) != 0 ||
        ((head - base) % kEntrySize) != 0 ||
        !address_span_valid(target_stopper_ea, 4)) {
        result.status = Status::invalid_layout;
        return result;
    }

    const std::uint32_t slot_count = (end - base) / kEntrySize;
    std::uint32_t entry = cursor;
    std::uint32_t release_entry = 0;

    /* Pass 1: prove that every still-live entry before the target release is
     * understood. Do not partially apply a prefix and then discover an
     * unknown operation immediately before the release. */
    for (std::uint32_t n = 0; n < slot_count && entry != head; ++n) {
        const std::uint32_t tag = io.read32(io.context, entry);
        if (tag == kTagMemcpy) {
            const std::uint32_t source = io.read32(io.context, entry + 0x04);
            const std::uint32_t size = io.read32(io.context, entry + 0x08);
            const std::uint32_t destination = io.read32(io.context, entry + 0x0c);
            if (!address_span_valid(source, size) ||
                !address_span_valid(destination, size)) {
                result.status = Status::invalid_layout;
                capture_problem(io, result, entry, tag);
                return result;
            }
        } else if (tag == kTagRelease) {
            const std::uint32_t stopper_ea = io.read32(io.context, entry + 0x04);
            if (!address_span_valid(stopper_ea, 4)) {
                result.status = Status::invalid_layout;
                capture_problem(io, result, entry, tag);
                return result;
            }
            if (stopper_ea == target_stopper_ea) {
                release_entry = entry;
                break;
            }
        } else if (tag == kTagHeaderMarker || tag == kTagPayloadEa) {
            /* No fields to validate: 0x11 carries none, 0x0A's EA is not
             * applied by anyone (see the decode note above). Retire-only. */
        } else if (tag != 0) {
            result.status = Status::unsupported_tag;
            capture_problem(io, result, entry, tag);
            return result;
        }
        entry = next_entry(entry, base, end);
    }

    if (!release_entry) {
        result.status = Status::no_matching_release;
        return result;
    }

    /* Pass 2: take a stable fingerprint, then immediately repeat it. The
     * caller also requires a frozen producer head; this catches an entry that
     * was rewritten without moving that head. */
    const std::uint64_t before = hash_span(io, base, end, cursor, release_entry,
                                           slot_count);
    const std::uint64_t after = hash_span(io, base, end, cursor, release_entry,
                                          slot_count);
    if (!before || before != after) {
        result.status = Status::changed_during_validation;
        return result;
    }

    /* Pass 3: commit in journal order. A tag is retired only after its
     * operation has completed. The release fence models SPU putf ordering:
     * command-buffer writes become visible before the jump-to-self is cleared. */
    entry = cursor;
    for (std::uint32_t n = 0; n < slot_count; ++n) {
        const std::uint32_t tag = io.read32(io.context, entry);
        if (tag == kTagMemcpy) {
            const std::uint32_t source = io.read32(io.context, entry + 0x04);
            const std::uint32_t size = io.read32(io.context, entry + 0x08);
            const std::uint32_t destination = io.read32(io.context, entry + 0x0c);
            if (!io.copy_bytes(io.context, destination, source, size)) {
                result.status = Status::copy_failed;
                capture_problem(io, result, entry, tag);
                return result;
            }
        } else if (tag == kTagRelease) {
            const std::uint32_t stopper_ea = io.read32(io.context, entry + 0x04);
            std::atomic_thread_fence(std::memory_order_release);
            io.release_stopper(io.context, stopper_ea);
        }

        if (tag != 0) {
            io.write32(io.context, entry, 0);
            ++result.applied_entries;
        }

        const std::uint32_t consumed = entry;
        entry = next_entry(entry, base, end);
        if (consumed == release_entry) {
            result.status = Status::applied;
            result.next_cursor = entry;
            return result;
        }
    }

    result.status = Status::invalid_layout;
    return result;
}

const char* status_name(Status status)
{
    switch (status) {
    case Status::applied: return "applied";
    case Status::no_matching_release: return "no-matching-release";
    case Status::unsupported_tag: return "unsupported-tag";
    case Status::changed_during_validation: return "changed-during-validation";
    case Status::invalid_layout: return "invalid-layout";
    case Status::copy_failed: return "copy-failed";
    }
    return "unknown";
}

}  // namespace yz::edge_journal
