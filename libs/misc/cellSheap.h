/*
 * ps3recomp - cellSheap HLE
 *
 * Shared heap allocator for PPU/SPU shared memory regions.
 * Manages allocation within a user-provided buffer.
 */

#ifndef PS3RECOMP_CELL_SHEAP_H
#define PS3RECOMP_CELL_SHEAP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80410401-04) were invented and collided byte-for-byte
 * with cellOvis's real facility (0x80410402 == CELL_OVIS_ERROR_INVAL).
 * Our macro names already match the real cellSheap names exactly
 * (cell/sheap/error.h) — only the numeric values were wrong; repointed to
 * the real sheap facility base (minor facility 0x3 -> 0x804103xx).
 */
#define CELL_SHEAP_ERROR_INVAL       0x80410302  /* real SDK value (cell/sheap/error.h:35) */
#define CELL_SHEAP_ERROR_NOMEM       0x80410304  /* real SDK value (cell/sheap/error.h:41) */
#define CELL_SHEAP_ERROR_ALIGN       0x80410310  /* real SDK value (cell/sheap/error.h:76) */
#define CELL_SHEAP_ERROR_NOSYS       0x80410303  /* real SDK value (cell/sheap/error.h:38) */

/* Constants */
#define CELL_SHEAP_MAX   8
#define CELL_SHEAP_BLOCK_MAX  256

/* Types */
typedef u32 CellSheapHandle;

typedef struct CellSheapAttr {
    void* heapBase;
    u32 heapSize;
    u32 align;         /* minimum alignment, power of 2, >= 16 */
    u32 reserved[4];
} CellSheapAttr;

/* Functions */
s32 cellSheapInitialize(const CellSheapAttr* attr, CellSheapHandle* handle);
s32 cellSheapFinalize(CellSheapHandle handle);

void* cellSheapAllocate(CellSheapHandle handle, u32 size);
s32 cellSheapFree(CellSheapHandle handle, void* ptr);

s32 cellSheapQueryMax(CellSheapHandle handle, u32* maxFree);
s32 cellSheapQueryFree(CellSheapHandle handle, u32* freeBytes);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SHEAP_H */
