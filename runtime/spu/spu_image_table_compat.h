/* Optional title image metadata.
 *
 * The generic runtime must compile without a generated title tree.  A title
 * that provides recomp_prx/spu_image_table.h gets the full table; otherwise
 * the legacy LLE-only task-image helpers become inert.
 */
#ifndef SPU_IMAGE_TABLE_COMPAT_H
#define SPU_IMAGE_TABLE_COMPAT_H

#if defined(PS3RECOMP_SPU_IMAGE_TABLE_HEADER)
#  include PS3RECOMP_SPU_IMAGE_TABLE_HEADER
#  define PS3RECOMP_HAVE_SPU_IMAGE_TABLE 1
#elif defined(__has_include)
#  if __has_include("../../yakuza/generated/spu_image_table.h")
#    include "../../yakuza/generated/spu_image_table.h"
#    define PS3RECOMP_HAVE_SPU_IMAGE_TABLE 1
#  elif __has_include("../../recomp_prx/spu_image_table.h")
#    include "../../recomp_prx/spu_image_table.h"
#    define PS3RECOMP_HAVE_SPU_IMAGE_TABLE 1
#  endif
#endif

#ifndef PS3RECOMP_HAVE_SPU_IMAGE_TABLE
typedef struct spu_image_desc {
    uint32_t elf_ea;
    int image_id;
    uint32_t entry;
    const char* name;
    uint32_t bss_va[4];
    uint32_t bss_len[4];
    int bss_n;
} spu_image_desc;
#define SPU_IMAGE_COUNT 0
static inline const spu_image_desc* spu_image_for_elf(uint32_t elf_ea)
{
    (void)elf_ea;
    return (const spu_image_desc*)0;
}
static inline void spu_image_zero_bss(uint8_t* ls, const spu_image_desc* d)
{
    (void)ls; (void)d;
}
#endif
#endif
