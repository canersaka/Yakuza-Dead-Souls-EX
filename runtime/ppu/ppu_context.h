/*
 * ps3recomp - PPU (PowerPC Processing Unit) execution context
 *
 * PPU thread context shared by lifted code and the syscall layer, with the
 * condition-register and XER helpers used by the instruction macros. The
 * field order must match the copy emitted by tools/ppu_lifter.py.
 */

#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Condition Register field -- 4 bits: LT, GT, EQ, SO
 * -----------------------------------------------------------------------*/
#define PPU_CR_LT  0x8
#define PPU_CR_GT  0x4
#define PPU_CR_EQ  0x2
#define PPU_CR_SO  0x1

/* XER bit positions */
#define PPU_XER_SO  (1u << 31)
#define PPU_XER_CA  (1u << 29)

/* ---------------------------------------------------------------------------
 * PPU thread context
 * -----------------------------------------------------------------------*/
typedef struct ppu_context {
    /* General-purpose registers (r0-r31) */
    uint64_t gpr[32];

    /* Floating-point registers (f0-f31) */
    double   fpr[32];

    /* Vector registers (vr0-vr31) for VMX/AltiVec
     * Each is 128 bits, aligned to 16 bytes. */
#ifdef _MSC_VER
    __declspec(align(16)) u128 vr[32];
#else
    u128     vr[32] __attribute__((aligned(16)));
#endif

    /* Condition register -- 8 x 4-bit fields packed into 32 bits.
     * CR0 is bits 31:28, CR1 is 27:24, ..., CR7 is 3:0. */
    uint32_t cr;

    /* Link register -- return address */
    uint64_t lr;

    /* Count register -- used by bdnz-style branches */
    uint64_t ctr;

    /* Fixed-point exception register */
    uint32_t xer;

    /* Floating-point status and control register */
    uint32_t fpscr;

    /* Vector status and control register */
    uint32_t vscr;

    /* Current instruction address (program counter) */
    uint64_t cia;

    /* Thread identification */
    uint64_t thread_id;

    /* Reservation state for lwarx/ldarx and stwcx./stdcx. (reserve_addr 0 means
     * no reservation) */
    uint64_t reserve_addr;
    uint64_t reserve_value;
    int      reserve_valid;

} ppu_context;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void ppu_context_init(ppu_context* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    /* r1 (stack pointer) gets set during thread creation.
     * r2 (TOC) gets set from the ELF entry descriptor.
     * r13 (SDA base) gets set from the module loader. */
}

/* ---------------------------------------------------------------------------
 * Condition Register helpers
 * -----------------------------------------------------------------------*/

/* Set a 4-bit CR field (0-7). */
static inline void ppu_cr_set(ppu_context* ctx, int field, uint8_t value)
{
    uint32_t shift = (uint32_t)(28 - field * 4);
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | ((uint32_t)(value & 0xF) << shift);
}

/* Set a CR field from a signed 64-bit comparison result. */
static inline void ppu_cr_set_s64(ppu_context* ctx, int field, int64_t a, int64_t b)
{
    uint8_t val;
    if (a < b)      val = PPU_CR_LT;
    else if (a > b) val = PPU_CR_GT;
    else            val = PPU_CR_EQ;
    if (ctx->xer & PPU_XER_SO) val |= PPU_CR_SO;
    ppu_cr_set(ctx, field, val);
}

/* Set a CR field from an unsigned 64-bit comparison result. */
static inline void ppu_cr_set_u64(ppu_context* ctx, int field, uint64_t a, uint64_t b)
{
    uint8_t val;
    if (a < b)      val = PPU_CR_LT;
    else if (a > b) val = PPU_CR_GT;
    else            val = PPU_CR_EQ;
    if (ctx->xer & PPU_XER_SO) val |= PPU_CR_SO;
    ppu_cr_set(ctx, field, val);
}

/* Set a CR field from a signed 32-bit comparison. */
static inline void ppu_cr_set_s32(ppu_context* ctx, int field, int32_t a, int32_t b)
{
    ppu_cr_set_s64(ctx, field, (int64_t)a, (int64_t)b);
}

static inline void ppu_cr_set_u32(ppu_context* ctx, int field, uint32_t a, uint32_t b)
{
    ppu_cr_set_u64(ctx, field, (uint64_t)a, (uint64_t)b);
}

/* ---------------------------------------------------------------------------
 * XER helpers
 * -----------------------------------------------------------------------*/
static inline int ppu_xer_get_ca(const ppu_context* ctx) { return (ctx->xer & PPU_XER_CA) != 0; }

static inline void ppu_xer_set_ca(ppu_context* ctx, int ca)
{
    if (ca) ctx->xer |= PPU_XER_CA;
    else    ctx->xer &= ~PPU_XER_CA;
}

/* ---------------------------------------------------------------------------
 * Stack pointer management
 *
 * PS3/PPC64 ABI: r1 = stack pointer, stack grows downward,
 * 16-byte aligned, with a back-chain word at r1+0.
 * -----------------------------------------------------------------------*/
static inline void ppu_set_stack(ppu_context* ctx, uint64_t stack_top, uint64_t stack_size)
{
    /* Place SP at the top of the allocated region, aligned down to 16 bytes,
     * leaving room for the minimum stack frame (48 bytes on PPC64). */
    uint64_t sp = (stack_top + stack_size - 48) & ~(uint64_t)0xF;
    ctx->gpr[1] = sp;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_CONTEXT_H */
