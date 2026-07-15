#include "edge_journal_hle.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

std::array<std::uint8_t, 0x20000> memory{};

std::uint32_t read32(void*, std::uint32_t address)
{
    std::uint32_t value = 0;
    std::memcpy(&value, memory.data() + address, sizeof(value));
    return value;
}

void write32(void*, std::uint32_t address, std::uint32_t value)
{
    std::memcpy(memory.data() + address, &value, sizeof(value));
}

bool copy_bytes(void*, std::uint32_t destination, std::uint32_t source,
                std::uint32_t size)
{
    if (source + size > memory.size() || destination + size > memory.size())
        return false;
    std::memmove(memory.data() + destination, memory.data() + source, size);
    return true;
}

/* The game host writes a gcm jump-forward release word; this fake models the
 * same "value chosen by the host, in released order" contract with a marker
 * and records the order releases were requested in. */
constexpr std::uint32_t released_marker = 0x20000004;
std::array<std::uint32_t, 8> release_order{};
unsigned release_count = 0;

void release_stopper(void*, std::uint32_t stopper_ea)
{
    if (release_count < release_order.size())
        release_order[release_count] = stopper_ea;
    ++release_count;
    write32(nullptr, stopper_ea, released_marker);
}

void entry(std::uint32_t address, std::uint32_t tag, std::uint32_t a,
           std::uint32_t b = 0, std::uint32_t c = 0)
{
    write32(nullptr, address, tag);
    write32(nullptr, address + 4, a);
    write32(nullptr, address + 8, b);
    write32(nullptr, address + 12, c);
}

bool expect(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

}  // namespace

int main()
{
    constexpr std::uint32_t base = 0x10000;
    constexpr std::uint32_t end = 0x10200;
    constexpr std::uint32_t source = 0x10800;
    constexpr std::uint32_t destination = 0x10900;
    constexpr std::uint32_t old_stopper = 0x10a00;
    constexpr std::uint32_t target_stopper = 0x10a20;

    const yz::edge_journal::Io io{nullptr, read32, write32, copy_bytes,
                                  release_stopper};
    bool ok = true;

    /* Ordered happy path: memcpy, a [0x11 header][0x0A payload] unit
     * (retire-only, no memory operation), an older release, then the target
     * release. */
    memory.fill(0);
    release_count = 0;
    constexpr std::uint32_t payload_ea = 0x10b00;
    for (unsigned i = 0; i < 16; ++i) memory[source + i] = std::uint8_t(i + 1);
    write32(nullptr, old_stopper, 0x20abcdef);
    write32(nullptr, target_stopper, 0x20abcdef);
    write32(nullptr, payload_ea, 0xDEADBEEF);   /* must survive untouched */
    entry(base, 0x10, source, 16, destination);
    entry(base + 0x20, 0x11, 0);
    entry(base + 0x40, 0x0a, payload_ea);
    entry(base + 0x60, 0x7f, old_stopper);
    entry(base + 0x80, 0x7f, target_stopper);

    auto result = yz::edge_journal::apply_through_release(
        io, base, end, base, base + 0xa0, target_stopper);
    ok &= expect(result.status == yz::edge_journal::Status::applied,
                 "ordered span should apply");
    ok &= expect(result.applied_entries == 5, "five live entries should retire");
    ok &= expect(result.next_cursor == base + 0xa0, "cursor should follow target release");
    ok &= expect(read32(nullptr, payload_ea) == 0xDEADBEEF,
                 "a [0x11][0x0A] unit must not touch its referenced EA");
    ok &= expect(std::memcmp(memory.data() + source, memory.data() + destination, 16) == 0,
                 "memcpy payload should be installed before release");
    ok &= expect(read32(nullptr, old_stopper) == released_marker &&
                 read32(nullptr, target_stopper) == released_marker,
                 "both ordered releases should go through the host release callback");
    ok &= expect(release_count == 2 && release_order[0] == old_stopper &&
                 release_order[1] == target_stopper,
                 "releases must run in journal order, older stopper first");
    ok &= expect(read32(nullptr, base) == 0 && read32(nullptr, base + 0x20) == 0 &&
                 read32(nullptr, base + 0x40) == 0,
                 "tags should retire after their operations");

    /* Safety property: an unknown operation prevents every mutation. */
    memory.fill(0);
    release_count = 0;
    for (unsigned i = 0; i < 16; ++i) memory[source + i] = std::uint8_t(0x80 + i);
    write32(nullptr, target_stopper, 0x20abcdef);
    entry(base, 0x10, source, 16, destination);
    entry(base + 0x20, 0x08, 0x11111111, 0x22222222, 0x33333333);
    entry(base + 0x40, 0x7f, target_stopper);

    result = yz::edge_journal::apply_through_release(
        io, base, end, base, base + 0x60, target_stopper);
    ok &= expect(result.status == yz::edge_journal::Status::unsupported_tag,
                 "unknown tag should block the transaction");
    ok &= expect(result.problem_entry == base + 0x20 && result.problem_tag == 0x08,
                 "unknown entry should be reported exactly");
    ok &= expect(read32(nullptr, base) == 0x10 && read32(nullptr, target_stopper) == 0x20abcdef,
                 "unknown tag must leave prior entries and stopper untouched");
    ok &= expect(release_count == 0,
                 "unknown tag must prevent every release callback");
    std::array<std::uint8_t, 16> zero{};
    ok &= expect(std::memcmp(memory.data() + destination, zero.data(), zero.size()) == 0,
                 "unknown tag must prevent prefix memcpy");

    if (ok) std::puts("OK: EDGE journal HLE preserves patch-before-release ordering");
    return ok ? 0 : 1;
}
