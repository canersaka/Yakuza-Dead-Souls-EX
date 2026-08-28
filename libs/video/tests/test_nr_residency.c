#include "../rsx_nr_residency.h"
#include "ppu_guest_read.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x, m) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", m); return 1; } } while (0)

static uint32_t g_observer_calls;
static uint32_t g_observer_write_begin;
static uint32_t g_observer_write_end;

static void observe_access(void* user, uint32_t ea, uint32_t size,
                           uint32_t source, uint32_t write,
                           uint32_t image, uint32_t task,
                           uint32_t pc, uint32_t command)
{
    (void)user; (void)ea; (void)size; (void)source;
    (void)image; (void)task; (void)pc; (void)command;
    ++g_observer_calls;
    g_observer_write_begin += write == VM_NATIVE_RESIDENCY_WRITE_BEGIN;
    g_observer_write_end += write == VM_NATIVE_RESIDENCY_WRITE_END;
}

int main(void)
{
    rsx_nr_residency_slot slot;
    rsx_nr_residency_init(&slot);
    uint32_t gen = 0;
    CHECK(rsx_nr_residency_begin(&slot, 0x41B72D00u, 0x300000u,
                                 17u, &gen) == 0 && gen == 1u,
          "begin generation");
    CHECK(atomic_load(&slot.writer_fence) == 17u,
          "reader before submission retains covering fence");
    int need = 0;
    CHECK(rsx_nr_residency_access(&slot, 0x40000000u, 128u, 0, &need) == 0 &&
          !need, "unrelated access");
    CHECK(rsx_nr_residency_access(&slot, 0x41B72D00u, 512u, 0, &need) == gen &&
          need, "reader claims materialization");
    CHECK(rsx_nr_residency_access(&slot, 0x41B73100u, 512u, 0, &need) == gen &&
          need, "concurrent reader joins generation");
    CHECK(atomic_load(&slot.state) == RSX_NR_RESIDENCY_MATERIALIZING,
          "one materialization episode before fence completion");
    CHECK(rsx_nr_residency_mark_coherent(&slot, gen) == 0,
          "publish coherent bytes");
    CHECK(rsx_nr_residency_access(&slot, 0x41B72D01u, 3u, 0, &need) == gen &&
          !need, "unaligned reader after fence completion");
    CHECK(rsx_nr_residency_access(&slot, 0x41B73D00u, 4096u, 1, &need) == gen &&
          !need, "writer after publication");
    CHECK(rsx_nr_residency_mark_dirty(&slot, gen, 0x41B73D00u, 4096u) == 0,
          "mark exact dirty page");
    CHECK(atomic_load(&slot.dirty_pages[0]) == 2u,
          "dirty bitmap exact page");
    CHECK(rsx_nr_residency_finish(&slot, gen) == 0,
          "restore closes generation");
    CHECK(rsx_nr_residency_access(&slot, 0x41B72D00u, 4u, 0, &need) == 0,
          "closed generation rejects access");

    CHECK(rsx_nr_residency_begin(&slot, 0x1000u, 0x2000u, 19u, &gen) == 0,
          "second generation");
    const uint32_t reset_gen = gen;
    rsx_nr_residency_reset(&slot);
    CHECK(atomic_load(&slot.state) == RSX_NR_RESIDENCY_IDLE &&
          atomic_load(&slot.reset_generation) == 1u,
          "reset cancels pending generation");
    CHECK(rsx_nr_residency_mark_coherent(&slot, reset_gen) != 0,
          "cancelled generation cannot publish");

    CHECK(rsx_nr_residency_begin(&slot, 0x2003u, 0x3001u, 23u, &gen) == 0 &&
          gen > reset_gen, "generation replacement after reset");
    CHECK(rsx_nr_residency_access(&slot, 0x2002u, 1u, 0, &need) == 0 &&
          !need, "adjacent alias rejected exactly");
    CHECK(rsx_nr_residency_access(&slot, 0x1FFFu, 8u, 0, &need) == gen && need,
          "unaligned cross-page overlap claims materialization");
    CHECK(rsx_nr_residency_mark_coherent(&slot, gen) == 0,
          "cross-page generation becomes coherent");
    CHECK(rsx_nr_residency_access(&slot, 0x2FFFu, 0x2006u, 1, &need) == gen &&
          !need, "overlapping writer begins from coherent bytes");
    CHECK(rsx_nr_residency_mark_dirty(&slot, gen, 0x2FFFu, 0x2006u) == 0,
          "partial cross-page writer publishes dirty span");
    CHECK((atomic_load(&slot.dirty_pages[0]) & 0xFu) == 0xFu,
          "partial unaligned write marks every touched page");
    CHECK(rsx_nr_residency_finish(&slot, gen) == 0,
          "movie/present handoff closes dirty generation");

    CHECK(rsx_nr_residency_begin(&slot, 0xFFFFFF00u, 0x200u, 1u, &gen) != 0,
          "wrapping range rejected");

    /* Sparse hook: disabled is one flag test, watched accesses deliver the
     * pre/post writer phases exactly once, and clear/reset rejects them. */
    vm_native_residency_clear_watches();
    vm_native_residency_set_observer(NULL, NULL);
    vm_native_residency_notify(
        0x3003u, 8u, 1u, VM_NATIVE_RESIDENCY_READ, 4u, 2u, 0x5D34u, 0x40u);
    CHECK(g_observer_calls == 0u, "disabled path is observationally inert");
    vm_native_residency_set_observer(observe_access, NULL);
    vm_native_residency_watch_range(0x3FFFu, 4u);
    vm_native_residency_notify(
        0x9000u, 4u, 0u, VM_NATIVE_RESIDENCY_READ, 0u, 0u, 0u, 0u);
    CHECK(g_observer_calls == 0u, "unrelated sparse page rejects immediately");
    vm_native_residency_notify(
        0x3FFFu, 4u, 1u, VM_NATIVE_RESIDENCY_READ, 4u, 2u, 0x5D34u, 0x40u);
    vm_native_residency_notify(
        0x4001u, 2u, 1u, VM_NATIVE_RESIDENCY_WRITE_BEGIN,
        4u, 2u, 0x48E0u, 0x20u);
    vm_native_residency_notify(
        0x4001u, 2u, 1u, VM_NATIVE_RESIDENCY_WRITE_END,
        4u, 2u, 0x48E0u, 0x20u);
    CHECK(g_observer_calls == 3u && g_observer_write_begin == 1u &&
          g_observer_write_end == 1u,
          "SPU read and paired writer phases retain semantic identity");
    vm_native_residency_clear_watches();
    vm_native_residency_notify(
        0x4001u, 2u, 1u, VM_NATIVE_RESIDENCY_READ, 4u, 2u, 0u, 0u);
    CHECK(g_observer_calls == 3u, "reset clears sparse watched pages");
    vm_native_residency_set_observer(NULL, NULL);

    puts("rsx_nr_residency: PASS");
    return 0;
}
