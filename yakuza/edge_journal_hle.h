#pragma once

#include <cstdint>

namespace yz::edge_journal {

struct Io {
    void* context = nullptr;
    std::uint32_t (*read32)(void* context, std::uint32_t address) = nullptr;
    void (*write32)(void* context, std::uint32_t address, std::uint32_t value) = nullptr;
    bool (*copy_bytes)(void* context, std::uint32_t destination,
                       std::uint32_t source, std::uint32_t size) = nullptr;
    /* Clear the jump-to-self at stopper_ea. The host supplies the release
     * VALUE: on hardware the SPU consumer writes a gcm jump-forward word
     * (top byte 0x20, target = the stopper's io offset + 4), not zero. */
    void (*release_stopper)(void* context, std::uint32_t stopper_ea) = nullptr;
};

enum class Status {
    applied,
    no_matching_release,
    unsupported_tag,
    changed_during_validation,
    invalid_layout,
    copy_failed,
};

struct Result {
    Status status = Status::invalid_layout;
    std::uint32_t next_cursor = 0;
    std::uint32_t problem_entry = 0;
    std::uint32_t problem_tag = 0;
    std::uint32_t problem_words[8]{};
    unsigned applied_entries = 0;
};

/* Apply complete 0x20-byte journal entries from cursor through the tag-0x7f
 * release for target_stopper_ea. This is deliberately a two-phase operation:
 * the complete span is validated and re-checked before the first guest write.
 * An unknown tag therefore leaves both the command buffer and journal intact. */
Result apply_through_release(const Io& io,
                             std::uint32_t base,
                             std::uint32_t end,
                             std::uint32_t cursor,
                             std::uint32_t head,
                             std::uint32_t target_stopper_ea);

const char* status_name(Status status);

}  // namespace yz::edge_journal
