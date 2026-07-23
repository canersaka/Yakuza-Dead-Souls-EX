/*
 * Compatibility aliases for the prepared generated-input tree.
 *
 * The current user-supplied PPU lift gives these game-owned mwPly entrypoints
 * distinct *_lifted symbols so an optional movie HLE layer can wrap them.
 * This firmware-free SPURS worktree deliberately does not import or modify
 * that out-of-scope movie layer; the public names therefore remain exact
 * pass-throughs to the lifted game code.
 */

#include "ppu_recomp.h"

#define YZ_LIFTED_PASSTHROUGH(ea)             \
    void func_##ea(ppu_context* ctx)           \
    {                                           \
        func_##ea##_lifted(ctx);                \
    }

YZ_LIFTED_PASSTHROUGH(00F4A9AC)
YZ_LIFTED_PASSTHROUGH(00F4D0A8)
YZ_LIFTED_PASSTHROUGH(00F4D134)
YZ_LIFTED_PASSTHROUGH(00F4D210)
YZ_LIFTED_PASSTHROUGH(00F4D878)
YZ_LIFTED_PASSTHROUGH(00F4DA90)
YZ_LIFTED_PASSTHROUGH(00F4E608)
YZ_LIFTED_PASSTHROUGH(00F4E720)
YZ_LIFTED_PASSTHROUGH(00F4ED44)
YZ_LIFTED_PASSTHROUGH(00F4FD3C)
YZ_LIFTED_PASSTHROUGH(00F51658)

#undef YZ_LIFTED_PASSTHROUGH
