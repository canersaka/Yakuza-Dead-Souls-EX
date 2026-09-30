# Regenerating the lifted sources

`recomp/`, `recomp_prx/` and `yakuza/generated/` are generator output and are not tracked.
They are rebuilt from your own decrypted copy of the game, and nothing is ever typed into
them: a translation fix belongs in a lifter, and game-specific data belongs in the inputs
named below.

## Game files

Run everything from the repository root. You need two decrypted executables from the
game disc, in `game/`:

| File | From the disc |
|---|---|
| `game/EBOOT.elf` | `PS3_GAME/USRDIR/EBOOT.BIN` |
| `game/ogrez_shader_ps3.ppu.elf` | `PS3_GAME/USRDIR/data/module/ps3/ogrez_shader_ps3.ppu.sprx` |

Both are encrypted on the disc; decrypt them with a tool of your choice (RPCS3's
"Decrypt PS3 Binaries" utility works). The runner also needs the disc contents at
`gamedata/dev_bdvd/` (so `gamedata/dev_bdvd/PS3_GAME/PARAM.SFO` exists).

## PPU

```powershell
py -3 tools\find_functions.py game\EBOOT.elf -o functions.json
py -3 tools\ppu_lifter.py game\EBOOT.elf --output recomp\ --functions functions.json --override-funcs yakuza\movie_overrides.txt
```

`yakuza/movie_overrides.txt` lists the guest functions implemented by hand; the lifter
emits their translations as `func_X_lifted` and leaves `func_X` to the hand-written code.

## Shader module

```powershell
py -3 tools\lift_prx.py game\ogrez_shader_ps3.ppu.elf --base 0x02200000 --output recomp_prx\
py -3 tools\ppu_lifter.py recomp_prx\ogrez_shader_ps3.ppu_image.bin --raw --base 0x02200000 --functions recomp_prx\ogrez_shader_ps3.ppu_functions.json --output recomp\prx --header-name pxd_shader_recomp.h --source-name pxd_shader_recomp --table-name pxd_shader_function_table --extern-funcs recomp\ppu_recomp.h
```

The runner loads `recomp_prx/ogrez_shader_ps3.ppu_image.bin` at startup, so keep that
folder next to the executable's working directory.

## SPU

```powershell
py -3 tools\gen_spu_images.py
```

This lifts every SPU program listed in `yakuza/spu_images.json` into
`yakuza/generated/spu/`: one region-lifted source per placement, plus `spu_images.c/.h`
with the task image, job binary and job placement tables and the function that registers
every placement. All inputs come from `game/EBOOT.elf`.

The manifest has two parts:

- `binaries`: an `elf` entry is the EBOOT address of an embedded SPU ELF (a SPURS task
  image). An `ea`/`size` entry is a raw SPURS job binary; its `identity_sizes` are the
  lengths job descriptors publish for it (default: `size`), and `image_id` is the id its
  launches adopt.
- `images`: the placements, in registration order. The runtime serves the first
  registered match, so `gs_task` (image 0, which matches any lookup) stays last. A job
  binary is lifted once per local-store base it runs at, and placements that overlap in
  local store need distinct image ids. `skip_calls` maps a call site to a C condition
  under which the call is skipped; `includes` lists the headers that condition needs.

Before lifting, the generator checks the manifest against the EBOOT: every embedded SPU
ELF must be listed, the SPU payload section must be accounted for apart from alignment
padding, and placements that share an image id must not overlap.

## Imports

`import_bridges_gen.cpp` and `func_table_gen.cpp` are generated into the build directory
by CMake through `yakuza/gen_imports.py` and `yakuza/gen_func_table.py`.

## Reviewing a lifter change

Keep a copy of `recomp/` and `yakuza/generated/` before regenerating and diff the new
output against it. Every difference should be explained by the change.

