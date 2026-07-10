/*
 * ps3recomp - PPU memory access for recompiled code
 *
 * All PS3 memory is big-endian.  These functions translate a 32-bit PS3
 * virtual address to a host pointer via vm_base, then perform a load or
 * store with the appropriate byte swap.
 *
 * Atomic operations (lwarx/stwcx) are emulated with C11 atomics.
 */

#ifndef PPU_MEMORY_H
#define PPU_MEMORY_H

#include "../../include/ps3emu/endian.h"
#include "ppu_context.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#if defined(_MSC_VER)
#  include <intrin.h>          /* _ReturnAddress -- the YZ_SLOTSTORE diag */
#else
#  define _ReturnAddress() __builtin_return_address(0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Base pointer -- set by the VM manager at startup.
 *
 * Host pointer to the beginning of the PS3 32-bit address space mapping.
 * All guest address translation is:  host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Address translation
 * -----------------------------------------------------------------------*/
static inline void* vm_translate(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint8_t* vm_ptr8(uint32_t addr)  { return (uint8_t*)vm_translate(addr); }
static inline uint16_t* vm_ptr16(uint32_t addr) { return (uint16_t*)vm_translate(addr); }
static inline uint32_t* vm_ptr32(uint32_t addr) { return (uint32_t*)vm_translate(addr); }
static inline uint64_t* vm_ptr64(uint32_t addr) { return (uint64_t*)vm_translate(addr); }

/* ---------------------------------------------------------------------------
 * Loads -- read from big-endian guest memory, return host-endian value.
 * -----------------------------------------------------------------------*/
static inline uint8_t vm_read8(uint32_t addr)
{
    return *vm_ptr8(addr);
}

static inline uint16_t vm_read16(uint32_t addr)
{
    uint16_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap16(raw);
}

static inline uint32_t vm_read32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap32(raw);
}

static inline uint64_t vm_read64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap64(raw);
}

static inline float vm_read_f32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap32(raw);
    float result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

static inline double vm_read_f64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap64(raw);
    double result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

/* ---------------------------------------------------------------------------
 * Stores -- write host-endian value into big-endian guest memory.
 * -----------------------------------------------------------------------*/
static inline void vm_write8(uint32_t addr, uint8_t val)
{
    /* s26 stager hunt (final unwatched widths — see vm_write32) */
    if (addr >= 0x42452880u && addr < 0x424529E0u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(addr, val, 8, _ReturnAddress());
        }
    }
    *vm_ptr8(addr) = val;
}

static inline void vm_write16(uint32_t addr, uint16_t val)
{
    if (addr >= 0x42452880u && addr < 0x424529E0u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(addr, val, 16, _ReturnAddress());
        }
    }
    uint16_t raw = ps3_bswap16(val);
    memcpy(vm_ptr8(addr), &raw, sizeof(raw));
}

static inline void vm_write32(uint32_t addr, uint32_t val)
{
    /* DIAG (env YZ_WATCH_DLIST): watch PPU writes to the io 0x1104D00 display
     * list (EA 0x41504D00). Tells whether the PPU producer (t1) ever writes the
     * REAL contents over the 0xA2000500 placeholder -> it's the producer and the
     * deadlock is a fixable consumer circular wait; vs never -> producer missing
     * (gs_task). One branch, predicted not-taken; harmless when env unset. */
    if (addr >= 0x41504C00u && addr < 0x41504E00u) {   /* io 0x1104D00 display list */
        extern int g_yz_watch_dlist;
        if (g_yz_watch_dlist) {
            extern unsigned long g_yz_dlist_w;
            if (g_yz_dlist_w < 120) { g_yz_dlist_w++;
                fprintf(stderr, "[dlist-w] EA=0x%08X = 0x%08X\n", addr, val);
                fflush(stderr);
            }
        }
    }
    /* DIAG (env YZ_SLOTSTORE, s26 — DONT_RECHASE #57 mode B): name the PPU
     * writer of the wid4 work-record slots (the round value a signaled task
     * consumes half-staged). The page-guard watch was defeated by the vm's
     * aliased mappings; every LIFTED store flows through here instead. One
     * predicted-not-taken branch when off. */
    if (addr >= 0x42452880u && addr < 0x424529E0u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(addr, val, 32, _ReturnAddress());
        }
    }
    /* s26 ride15 pivot (STATUS ⚡): PPU writes to the decode label 0x10200FE0
     * (the per-round RE-ARM zeroes) — order vs the SPU publishes ([fe0]) is
     * the wedge discriminator (acquire parks on an already-published value).
     * Same env gate as the slot watch. */
    if (addr == 0x10200FE0u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(addr, val, 33, _ReturnAddress());  /* width 33 = label tag */
        }
    }
    uint32_t raw = ps3_bswap32(val);
    memcpy(vm_ptr8(addr), &raw, sizeof(raw));
}

static inline void vm_write64(uint32_t addr, uint64_t val)
{
    if (addr >= 0x42452880u && addr < 0x424529E0u) {   /* see vm_write32 */
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(addr, val, 64, _ReturnAddress());
        }
    }
    uint64_t raw = ps3_bswap64(val);
    memcpy(vm_ptr8(addr), &raw, sizeof(raw));
}

static inline void vm_write_f32(uint32_t addr, float val)
{
    uint32_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write32(addr, tmp);
}

static inline void vm_write_f64(uint32_t addr, double val)
{
    uint64_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write64(addr, tmp);
}

/* ---------------------------------------------------------------------------
 * Block copies (for string / bulk data movement)
 * -----------------------------------------------------------------------*/
static inline void vm_memcpy_from(void* host_dst, uint32_t guest_src, size_t len)
{
    memcpy(host_dst, vm_ptr8(guest_src), len);
}

static inline void vm_memcpy_to(uint32_t guest_dst, const void* host_src, size_t len)
{
    /* s26 stager hunt (STATUS ⚡ ~03:25): the wid4 work-record slots are
     * written by NEITHER lifted PPU stores (YZ_SLOTSTORE zero) NOR SPU DMA
     * ([w4stage] zero) ⇒ this bulk path is the remaining candidate. Same env
     * gate; width tag 99 = memcpy_to. */
    if (guest_dst < 0x424529E0u && guest_dst + len > 0x42452880u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            unsigned long long head = 0;
            memcpy(&head, host_src, len < 8 ? len : 8);
            yz_slotstore_log(guest_dst, head, 99, _ReturnAddress());
        }
    }
    memcpy(vm_ptr8(guest_dst), host_src, len);
}

static inline void vm_memset(uint32_t guest_dst, int val, size_t len)
{
    if (guest_dst < 0x424529E0u && guest_dst + len > 0x42452880u) {
        extern int g_yz_slotstore;
        if (g_yz_slotstore) {
            extern void yz_slotstore_log(uint32_t addr, unsigned long long val,
                                         int width, void* ra);
            yz_slotstore_log(guest_dst, (unsigned long long)(unsigned)val, 98, _ReturnAddress());
        }
    }
    memset(vm_ptr8(guest_dst), val, len);
}

/* ---------------------------------------------------------------------------
 * Atomic operations -- lwarx / stwcx emulation
 *
 * PowerPC reservation-based atomics:
 *   lwarx  rD, rA, rB   -- Load word and reserve (sets reservation)
 *   stwcx. rS, rA, rB   -- Store word conditional (clears reservation)
 *                           Sets CR0 EQ bit on success.
 *
 * We emulate this using C11 compare-and-swap on the host.
 * -----------------------------------------------------------------------*/

static inline uint32_t ppu_lwarx(ppu_context* ctx, uint32_t addr)
{
    /* Read the current value from memory (big-endian, so swap). */
    uint32_t raw;
    _Atomic(uint32_t)* atom = (_Atomic(uint32_t)*)vm_ptr32(addr);
    raw = atomic_load_explicit(atom, memory_order_acquire);

    ctx->reserve_addr  = addr;
    ctx->reserve_value = raw;
    ctx->reserve_valid = 1;

    return ps3_bswap32(raw);
}

static inline int ppu_stwcx(ppu_context* ctx, uint32_t addr, uint32_t val)
{
    if (!ctx->reserve_valid || ctx->reserve_addr != addr) {
        /* No reservation or address mismatch -- fail */
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));
        ctx->reserve_valid = 0;
        return 0;
    }

    uint32_t expected = (uint32_t)ctx->reserve_value;
    uint32_t desired  = ps3_bswap32(val);
    _Atomic(uint32_t)* atom = (_Atomic(uint32_t)*)vm_ptr32(addr);

    int ok = atomic_compare_exchange_strong_explicit(
        atom, &expected, desired,
        memory_order_acq_rel, memory_order_acquire);

    ctx->reserve_valid = 0;

    if (ok)
        ppu_cr_set(ctx, 0, PPU_CR_EQ | (PPU_CR_SO * ppu_xer_get_so(ctx)));
    else
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));

    return ok;
}

/* 64-bit variants: ldarx / stdcx. */
static inline uint64_t ppu_ldarx(ppu_context* ctx, uint32_t addr)
{
    _Atomic(uint64_t)* atom = (_Atomic(uint64_t)*)vm_ptr64(addr);
    uint64_t raw = atomic_load_explicit(atom, memory_order_acquire);

    ctx->reserve_addr  = addr;
    ctx->reserve_value = raw;
    ctx->reserve_valid = 1;

    return ps3_bswap64(raw);
}

static inline int ppu_stdcx(ppu_context* ctx, uint32_t addr, uint64_t val)
{
    if (!ctx->reserve_valid || ctx->reserve_addr != addr) {
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));
        ctx->reserve_valid = 0;
        return 0;
    }

    uint64_t expected = ctx->reserve_value;
    uint64_t desired  = ps3_bswap64(val);
    _Atomic(uint64_t)* atom = (_Atomic(uint64_t)*)vm_ptr64(addr);

    int ok = atomic_compare_exchange_strong_explicit(
        atom, &expected, desired,
        memory_order_acq_rel, memory_order_acquire);

    ctx->reserve_valid = 0;

    if (ok)
        ppu_cr_set(ctx, 0, PPU_CR_EQ | (PPU_CR_SO * ppu_xer_get_so(ctx)));
    else
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));

    return ok;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_MEMORY_H */
