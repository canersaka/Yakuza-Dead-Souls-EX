#ifndef PS3RECOMP_SPU_JOB_DISPATCH_H
#define PS3RECOMP_SPU_JOB_DISPATCH_H

#include <stdint.h>

/*
 * Resolve job binaries whose launch descriptor is more authoritative than
 * the per-SPU residency cache.  This must not depend on a scene/diagnostic
 * flag: SPURS can launch the descriptor before higher-level scene tracking
 * observes the corresponding asset open.
 */
static inline int spu_job_descriptor_image(
    int current_image, uint32_t entry_pc, uint32_t binary_ea)
{
    const int job_family =
        current_image == 13 || current_image == 14 ||
        current_image == 15 || current_image == 17 ||
        current_image == 18 || current_image == 19 ||
        current_image == 20 || current_image == 21 ||
        current_image == 22 || current_image == 23 ||
        current_image == 24 || current_image == 25 ||
        current_image == 26 || current_image == 27 ||
        current_image == 28 || current_image == 29 ||
        current_image == 30 || current_image == 31 ||
        current_image == 32 || current_image == 33 ||
        current_image == 34 || current_image == 35 ||
        current_image == 36 || current_image == 37 ||
        current_image == 38 || current_image == 39 ||
        current_image == 40 || current_image == 41 ||
        current_image == 42 || current_image == 43 ||
        current_image == 44 || current_image == 45 ||
        current_image == 46;

    if (!job_family)
        return -1;

    switch (binary_ea) {
        case 0x01254500u:
            if (entry_pc == 0x04C00u || entry_pc == 0x0E400u ||
                entry_pc == 0x24400u)
                return 14;
            if (entry_pc == 0x0C400u)
                return 39;
            if (entry_pc == 0x2A400u)
                return 41;
            break;
        case 0x01275A00u:
            if (entry_pc == 0x04C00u || entry_pc == 0x06C00u ||
                entry_pc == 0x0E400u || entry_pc == 0x15800u ||
                entry_pc == 0x1EC00u || entry_pc == 0x3BC00u ||
                entry_pc == 0x3DC00u || entry_pc == 0x39C00u)
                return 15;
            if (entry_pc == 0x15400u)
                return 24;
            if (entry_pc == 0x37C00u)
                return 35;
            if (entry_pc == 0x3B000u)
                return 37;
            if (entry_pc == 0x3D000u)
                return 43;
            break;
        case 0x0125DA80u:
            if (entry_pc == 0x04C00u)
                return 38;
            if (entry_pc == 0x0E400u)
                return 17;
            if (entry_pc == 0x15800u)
                return 40;
            if (entry_pc == 0x17800u)
                return 17;
            if (entry_pc == 0x1A800u)
                return 20;
            if (entry_pc == 0x1AC00u)
                return 27;
            if (entry_pc == 0x1D800u)
                return 22;
            if (entry_pc == 0x1B800u)
                return 23;
            if (entry_pc == 0x1E800u)
                return 28;
            break;
        case 0x01265180u:
            if (entry_pc == 0x05000u)
                return 26;
            if (entry_pc == 0x0C400u)
                return 42;
            if (entry_pc == 0x0E400u || entry_pc == 0x20C00u)
                return 18;
            if (entry_pc == 0x04C00u || entry_pc == 0x23000u)
                return 21;
            if (entry_pc == 0x15800u)
                return 25;
            if (entry_pc == 0x2B400u)
                return 29;
            if (entry_pc == 0x2B800u)
                return 45;
            if (entry_pc == 0x27400u)
                return 32;
            if (entry_pc == 0x2A800u)
                return 33;
            if (entry_pc == 0x2D400u)
                return 31;
            if (entry_pc == 0x2E800u)
                return 30;
            if (entry_pc == 0x2E400u)
                return 34;
            if (entry_pc == 0x2C400u)
                return 36;
            break;
        case 0x01252680u:
            if (entry_pc == 0x04C00u || entry_pc == 0x0E400u ||
                entry_pc == 0x2D800u || entry_pc == 0x37C00u ||
                entry_pc == 0x3B000u)
                return 19;
            if (entry_pc == 0x3BC00u)
                return 44;
            if (entry_pc == 0x3CC00u)
                return 46;
            break;
        default:
            break;
    }

    return -1;
}

static inline int spu_job_descriptor_slot(int image)
{
    switch (image) {
        case 14: return 0;
        case 15: return 1;
        case 24: return 1;
        case 17: return 2;
        case 20: return 2;
        case 22: return 2;
        case 23: return 2;
        case 27: return 2;
        case 28: return 2;
        case 18: return 3;
        case 21: return 3;
        case 25: return 3;
        case 26: return 3;
        case 29: return 3;
        case 30: return 3;
        case 31: return 3;
        case 32: return 3;
        case 33: return 3;
        case 34: return 3;
        case 35: return 1;
        case 36: return 3;
        case 37: return 1;
        case 38: return 2;
        case 39: return 0;
        case 40: return 2;
        case 41: return 0;
        case 43: return 1;
        case 44: return 4;
        case 45: return 3;
        case 46: return 4;
        case 19: return 4;
        default: return -1;
    }
}

static inline uint32_t spu_job_descriptor_span(int image)
{
    switch (image) {
        case 14: return 0x9540u;
        case 15: return 0x14C0u;
        case 24: return 0x14C0u;
        case 17: return 0x7640u;
        case 20: return 0x7640u;
        case 22: return 0x7640u;
        case 23: return 0x7640u;
        case 27: return 0x7640u;
        case 28: return 0x7640u;
        case 18: return 0x10610u;
        case 21: return 0x10610u;
        case 25: return 0x10610u;
        case 26: return 0x10610u;
        case 29: return 0x10610u;
        case 30: return 0x10610u;
        case 31: return 0x10610u;
        case 32: return 0x10610u;
        case 33: return 0x10610u;
        case 34: return 0x10610u;
        case 35: return 0x14C0u;
        case 36: return 0x10610u;
        case 37: return 0x14C0u;
        case 38: return 0x7640u;
        case 39: return 0x9540u;
        case 40: return 0x7640u;
        case 41: return 0x9540u;
        case 42: return 0x10610u;
        case 43: return 0x14C0u;
        case 44: return 0x1E80u;
        case 45: return 0x10610u;
        case 46: return 0x1E80u;
        case 19: return 0x1E80u;
        default: return 0;
    }
}

/*
 * Map a per-SPU job-binary residency slot back to the lift registered for
 * that exact relocation.  Internal job calls do not carry the launch
 * descriptor, so their image selection must preserve the entry relocation
 * recorded in job_bin_base rather than falling back to the family's default
 * image.
 */
static inline int spu_job_resident_image(int slot, uint32_t base)
{
    switch (slot) {
        case 0:
            if (base == 0x0C400u) return 39;
            if (base == 0x2A400u) return 41;
            return 14;
        case 1:
            if (base == 0x37C00u) return 35;
            if (base == 0x3B000u) return 37;
            if (base == 0x3D000u) return 43;
            return base == 0x15400u ? 24 : 15;
        case 2:
            if (base == 0x04C00u) return 38;
            if (base == 0x15800u) return 40;
            if (base == 0x1A800u) return 20;
            if (base == 0x1AC00u) return 27;
            if (base == 0x1D800u) return 22;
            if (base == 0x1B800u) return 23;
            if (base == 0x1E800u) return 28;
            return 17;
        case 3:
            if (base == 0x04C00u || base == 0x23000u) return 21;
            if (base == 0x0C400u) return 42;
            if (base == 0x15800u) return 25;
            if (base == 0x05000u) return 26;
            if (base == 0x2B400u) return 29;
            if (base == 0x2B800u) return 45;
            if (base == 0x27400u) return 32;
            if (base == 0x2A800u) return 33;
            if (base == 0x2D400u) return 31;
            if (base == 0x2E400u) return 34;
            if (base == 0x2C400u) return 36;
            if (base == 0x2E800u) return 30;
            return 18;
        case 4:
            if (base == 0x3BC00u) return 44;
            if (base == 0x3CC00u) return 46;
            return 19;
        default:
            return -1;
    }
}

#endif
