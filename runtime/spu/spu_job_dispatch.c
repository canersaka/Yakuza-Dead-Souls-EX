#include "spu_job_dispatch.h"

static const spu_job_placement* s_placements;
static unsigned s_placement_count;

void spu_job_placements_set(const spu_job_placement* table, unsigned count)
{
    s_placements = table;
    s_placement_count = table ? count : 0;
}

int spu_job_placement_image(uint32_t binary_ea, uint32_t base)
{
    for (unsigned i = 0; i < s_placement_count; ++i) {
        const spu_job_placement* p = &s_placements[i];
        if (p->binary_ea == binary_ea && p->base == base)
            return p->image;
    }
    return -1;
}
