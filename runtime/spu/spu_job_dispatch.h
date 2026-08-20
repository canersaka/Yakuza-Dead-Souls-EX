#ifndef PS3RECOMP_SPU_JOB_DISPATCH_H
#define PS3RECOMP_SPU_JOB_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

/*
 * One authoritative catalog for every known EBOOT-resident SPURS job
 * placement.  Older code kept four separate switch/array copies in the DMA,
 * descriptor-handoff and indirect-branch paths; adding a job to only one of
 * them produced an INVALID_BIN or, worse, ran stale code from another family.
 */
typedef struct spu_job_placement {
    uint32_t binary_ea;
    uint32_t entry_pc;
    uint32_t span;
    int image;
    int slot;
} spu_job_placement;

enum { SPU_JOB_FAMILY_COUNT = 6 };

static const spu_job_placement g_spu_job_placements[] = {
    /* Job A */
    {0x01254500u, 0x04C00u, 0x09540u, 14, 0},
    {0x01254500u, 0x0E400u, 0x09540u, 14, 0},
    {0x01254500u, 0x24400u, 0x09540u, 14, 0},
    {0x01254500u, 0x0C400u, 0x09540u, 39, 0},
    {0x01254500u, 0x2A400u, 0x09540u, 41, 0},

    /* Job B */
    {0x01275A00u, 0x04C00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x06C00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x0E400u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x15800u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x1EC00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x3BC00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x3DC00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x39C00u, 0x014C0u, 15, 1},
    {0x01275A00u, 0x15400u, 0x014C0u, 24, 1},
    {0x01275A00u, 0x37C00u, 0x014C0u, 35, 1},
    {0x01275A00u, 0x3B000u, 0x014C0u, 37, 1},
    {0x01275A00u, 0x3D000u, 0x014C0u, 43, 1},

    /* Job C */
    {0x0125DA80u, 0x04C00u, 0x07640u, 38, 2},
    {0x0125DA80u, 0x0E400u, 0x07640u, 17, 2},
    {0x0125DA80u, 0x15800u, 0x07640u, 40, 2},
    {0x0125DA80u, 0x17800u, 0x07640u, 17, 2},
    {0x0125DA80u, 0x1A800u, 0x07640u, 20, 2},
    {0x0125DA80u, 0x1AC00u, 0x07640u, 27, 2},
    {0x0125DA80u, 0x1D800u, 0x07640u, 22, 2},
    {0x0125DA80u, 0x1B800u, 0x07640u, 23, 2},
    {0x0125DA80u, 0x1E800u, 0x07640u, 28, 2},

    /* Job D */
    {0x01265180u, 0x15C00u, 0x10610u, 47, 3},
    {0x01265180u, 0x05000u, 0x10610u, 26, 3},
    {0x01265180u, 0x0C400u, 0x10610u, 42, 3},
    {0x01265180u, 0x0E400u, 0x10610u, 18, 3},
    {0x01265180u, 0x20C00u, 0x10610u, 18, 3},
    {0x01265180u, 0x04C00u, 0x10610u, 21, 3},
    {0x01265180u, 0x23000u, 0x10610u, 21, 3},
    {0x01265180u, 0x15800u, 0x10610u, 25, 3},
    {0x01265180u, 0x2B400u, 0x10610u, 29, 3},
    {0x01265180u, 0x2B800u, 0x10610u, 45, 3},
    {0x01265180u, 0x27400u, 0x10610u, 32, 3},
    {0x01265180u, 0x2A800u, 0x10610u, 33, 3},
    {0x01265180u, 0x2D400u, 0x10610u, 31, 3},
    {0x01265180u, 0x2E800u, 0x10610u, 30, 3},
    {0x01265180u, 0x2E400u, 0x10610u, 34, 3},
    {0x01265180u, 0x2C400u, 0x10610u, 36, 3},

    /* Orphanage worker */
    {0x01252680u, 0x04C00u, 0x01E80u, 19, 4},
    {0x01252680u, 0x0E400u, 0x01E80u, 19, 4},
    {0x01252680u, 0x2D800u, 0x01E80u, 19, 4},
    {0x01252680u, 0x37C00u, 0x01E80u, 19, 4},
    {0x01252680u, 0x3B000u, 0x01E80u, 19, 4},
    {0x01252680u, 0x3BC00u, 0x01E80u, 44, 4},
    {0x01252680u, 0x3C000u, 0x01E80u, 48, 4},
    {0x01252680u, 0x3CC00u, 0x01E80u, 46, 4},

    /* Frontier job discovered at descriptor 0x401B2900. Native SPURS
     * alternates fresh job contexts between these two LS slots. */
    {0x01241400u, 0x0E400u, 0x11280u, 49, 5},
    {0x01241400u, 0x04C00u, 0x11280u, 50, 5},
};

#define SPU_JOB_PLACEMENT_COUNT \
    (sizeof(g_spu_job_placements) / sizeof(g_spu_job_placements[0]))

static inline int spu_job_image_is_known(int image)
{
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i)
        if (g_spu_job_placements[i].image == image)
            return 1;
    return 0;
}

/* Resolve a launch descriptor to the lift for its exact source and LS base. */
static inline int spu_job_descriptor_image(
    int current_image, uint32_t entry_pc, uint32_t binary_ea)
{
    if (current_image != 13 && !spu_job_image_is_known(current_image))
        return -1;

    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i) {
        const spu_job_placement* p = &g_spu_job_placements[i];
        if (p->binary_ea == binary_ea && p->entry_pc == entry_pc)
            return p->image;
    }
    return -1;
}

static inline int spu_job_descriptor_slot(int image)
{
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i)
        if (g_spu_job_placements[i].image == image)
            return g_spu_job_placements[i].slot;
    return -1;
}

static inline uint32_t spu_job_descriptor_span(int image)
{
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i)
        if (g_spu_job_placements[i].image == image)
            return g_spu_job_placements[i].span;
    return 0;
}

static inline int spu_job_binary_slot(uint32_t binary_ea)
{
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i)
        if (g_spu_job_placements[i].binary_ea == binary_ea)
            return g_spu_job_placements[i].slot;
    return -1;
}

static inline uint32_t spu_job_family_span(int slot)
{
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i)
        if (g_spu_job_placements[i].slot == slot)
            return g_spu_job_placements[i].span;
    return 0;
}

/* Internal job calls carry no descriptor, so use the recorded family/base. */
static inline int spu_job_resident_image(int slot, uint32_t base)
{
    static const int default_image[SPU_JOB_FAMILY_COUNT] = {
        14, 15, 17, 18, 19, 49
    };
    for (size_t i = 0; i < SPU_JOB_PLACEMENT_COUNT; ++i) {
        const spu_job_placement* p = &g_spu_job_placements[i];
        if (p->slot == slot && p->entry_pc == base)
            return p->image;
    }
    return slot >= 0 && slot < SPU_JOB_FAMILY_COUNT
        ? default_image[slot] : -1;
}

#endif
