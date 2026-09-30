/*
 * ps3recomp - PPU memory access for recompiled code
 *
 * All PS3 memory is big-endian.  These functions translate a 32-bit PS3
 * virtual address to a host pointer via vm_base, then perform a load or
 * store with the appropriate byte swap.
 */

#ifndef PPU_MEMORY_H
#define PPU_MEMORY_H

#include "../../include/ps3emu/endian.h"
#include "ppu_guest_write.h"

#include <stdint.h>
#include <string.h>

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

/* PPU->SPU lock-line coherence for the inline stores below. A store to a
 * 128-byte line an SPU holds under GETLLAR must serialize through the SPU
 * lock-line and raise SPU_EVENT_LR, otherwise the SPU's PUTLLC clobbers the
 * store or a parked idle-service never wakes. spu_coh_is_reserved() is a
 * range check plus an O(1) bitmap probe, so ordinary writes skip the lock.
 * Bulk forms check only the first line. */
#ifdef PS3RECOMP_NO_SPU_COH
/* Standalone unit-test builds that compile an HLE module without the SPU
 * runtime (e.g. libs/system/tests) define this to get no-op coherence. */
static inline int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
static inline void spu_lockline_lock(void) {}
static inline void spu_lockline_unlock(void) {}
static inline void spu_coh_notify_write(uint32_t ea) { (void)ea; }
#else
int  spu_coh_is_reserved(uint32_t addr);
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t ea);
#endif
static inline void vm_store_coh(uint32_t addr, const void* src, size_t n)
{
    if (spu_coh_is_reserved(addr)) {
        spu_lockline_lock();
        memcpy(vm_ptr8(addr), src, n);
        spu_coh_notify_write(addr);
        spu_lockline_unlock();
    } else {
        memcpy(vm_ptr8(addr), src, n);
    }
}

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
    vm_store_coh(addr, &val, 1);
    vm_native_spurs_notify_write(addr, 1);
}

static inline void vm_write16(uint32_t addr, uint16_t val)
{
    uint16_t raw = ps3_bswap16(val);
    vm_store_coh(addr, &raw, sizeof(raw));
    vm_native_spurs_notify_write(addr, 2);
}

static inline void vm_write32(uint32_t addr, uint32_t val)
{
    uint32_t raw = ps3_bswap32(val);
    vm_store_coh(addr, &raw, sizeof(raw));
    vm_native_spurs_notify_write(addr, 4);
}

static inline void vm_write64(uint32_t addr, uint64_t val)
{
    uint64_t raw = ps3_bswap64(val);
    vm_store_coh(addr, &raw, sizeof(raw));
    vm_native_spurs_notify_write(addr, 8);
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
static inline void vm_memcpy_to(uint32_t guest_dst, const void* host_src, size_t len)
{
    vm_store_coh(guest_dst, host_src, len);
    if (len <= UINT32_MAX)
        vm_native_spurs_notify_write(guest_dst, (uint32_t)len);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_MEMORY_H */
