/*
 * The title's shared presentation clock normally derives a 30 Hz timeline
 * from an audio voice position.  A backend that reports success without
 * producing a valid position leaves the guest conversion with an enormous
 * (~2^62) float.  AUTH then treats every scene as already complete.
 *
 * Keep the original routine for valid clocks.  If it produces an impossible
 * frame position, continue from its previous value using a host monotonic
 * clock.  This preserves the title's expected 30 Hz units and, unlike a scene
 * specific clamp, also protects menus and other AUTH playback users.
 */
#include "ppu_recomp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

struct fallback_clock {
    uint32_t object = 0;
    float value = 0.0f;
    std::chrono::steady_clock::time_point sampled{};
    bool active = false;
};

fallback_clock g_fallback[4];

float read_float(uint32_t address)
{
    const uint32_t bits = vm_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_float(uint32_t address, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    vm_write32(address, bits);
}

fallback_clock& slot_for(uint32_t object)
{
    for (auto& slot : g_fallback) {
        if (slot.object == object)
            return slot;
    }
    for (auto& slot : g_fallback) {
        if (!slot.active) {
            slot.object = object;
            return slot;
        }
    }
    g_fallback[0] = {};
    g_fallback[0].object = object;
    return g_fallback[0];
}

bool valid_position(float value)
{
    /* Even a multi-day 30 Hz timeline is far below this. */
    return std::isfinite(value) && value >= -1.0f && value < 100000000.0f;
}

} // namespace

void func_00AFC178(ppu_context* ctx)
{
    const uint32_t object = (uint32_t)ctx->gpr[3];
    const float previous = read_float(object + 0x10u);

    func_00AFC178_lifted(ctx);

    const float reported = (float)ctx->fpr[1];
    fallback_clock& fallback = slot_for(object);
    if (valid_position(reported)) {
        fallback.value = reported;
        fallback.sampled = std::chrono::steady_clock::now();
        fallback.active = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!fallback.active || !valid_position(fallback.value) ||
        (valid_position(previous) &&
         std::fabs(previous - fallback.value) > 90.0f)) {
        fallback.value = valid_position(previous) && previous >= 0.0f
                       ? previous : 0.0f;
        fallback.sampled = now;
        fallback.active = true;
        fprintf(stderr,
                "[present-clock] rejected invalid position object=%08X "
                "reported=%g previous=%g source=%08X mode=%08X; "
                "using monotonic 30 Hz fallback\n",
                object, (double)reported, (double)previous,
                vm_read32(object + 0x120u), vm_read32(object + 0x04u));
        fflush(stderr);
    } else {
        const double elapsed =
            std::chrono::duration<double>(now - fallback.sampled).count();
        fallback.value += (float)(elapsed * 30.0);
        fallback.sampled = now;
    }

    ctx->fpr[1] = fallback.value;
    write_float(object + 0x10u, fallback.value);
}
