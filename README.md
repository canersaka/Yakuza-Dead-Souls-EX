# Yakuza: Dead Souls EX

A work-in-progress native PC port of **Yakuza: Dead Souls**, built by statically recompiling the original PlayStation 3 executable.

The project translates the game's PowerPC PPU code and Cell SPU programs into native code and replaces the PS3 kernel, system libraries, SPURS, graphics, audio, input and filesystem with a native runtime. It needs no PS3 firmware: every system component the game calls is implemented from scratch.

> This is an experimental preservation and engineering project. It is not a finished port.

## Current Status

<p align="center">
  <a href="https://www.youtube.com/watch?v=Y3LBx3jeiLc">
    <img src="https://img.youtube.com/vi/Y3LBx3jeiLc/maxresdefault.jpg" width="600">
  </a>
</p>

Development began on June 9, 2026, the fifteenth anniversary of the original Japanese release of *Ryū ga Gotoku: Of the End*.

The port currently:

- Compiles the game's PPU code, its shader PRX module and its SPU programs into a native Windows executable.
- Boots without any PS3 firmware, using a native SPURS scheduler, a native gcm/RSX driver layer and native lv2 system calls.
- Plays the intro movies with audio, reaches the title screen, starts a new game and plays through the opening into gameplay.
- Renders the game's own RSX command stream through a live Direct3D 12 renderer, with its vertex and fragment programs translated to HLSL.
- Runs lighter scenes, such as the Continue tutorial, at the game's 30 FPS cap.

The main open problem is performance in heavy scenes: the crowded New Game opening runs at roughly 19 to 21 FPS. The game's main PPU thread, the renderer and the lifted SPU work that feeds it each take longer than a 33 ms frame, so no single change reaches 30 FPS.

## What you need

- Windows 10 or 11 (x64) and a GPU with Direct3D 12.
- Visual Studio 2022 or newer with the C++ workload, CMake 3.21 or newer, Ninja and Python 3.10 or newer.
- An FFmpeg development package (headers and import libraries) for the game's movies. The build looks in `third_party/ffmpeg` unless you pass `-DYZ_FFMPEG_DIR=<path>`. Without it the game still builds, but movies are skipped.
- Your own copy of Yakuza: Dead Souls. The port is built and tested against the North American disc, BLUS30826 version 1.00.

## Building

The game's code is not in this repository. You generate it from your own copy of the game, then build. [yakuza/REGENERATE.md](yakuza/REGENERATE.md) lists the files to extract and decrypt and the exact commands. In short:

```powershell
py -3 tools\find_functions.py game\EBOOT.elf -o functions.json
py -3 tools\ppu_lifter.py game\EBOOT.elf --output recomp\ --functions functions.json --override-funcs yakuza\movie_overrides.txt
py -3 tools\lift_prx.py game\ogrez_shader_ps3.ppu.elf --base 0x02200000 --output recomp_prx\
py -3 tools\ppu_lifter.py recomp_prx\ogrez_shader_ps3.ppu_image.bin --raw --base 0x02200000 --functions recomp_prx\ogrez_shader_ps3.ppu_functions.json --output recomp\prx --header-name pxd_shader_recomp.h --source-name pxd_shader_recomp --table-name pxd_shader_function_table --extern-funcs recomp\ppu_recomp.h
py -3 tools\gen_spu_images.py
```

Then, from a Visual Studio x64 developer prompt:

```powershell
cd yakuza
cmake --preset release
cmake --build --preset release
cd ..
```

The first build takes a while: the lifted game code is several million lines of C++, compiled with whole-program optimization.

## Running

Put the disc contents under `gamedata/dev_bdvd/` and start the runner from the repository root:

```powershell
.\yakuza\build_release\yakuza_recomp.exe game\EBOOT.elf
```

Saves go to `gamedata/dev_hdd0/`. A controller works through XInput. On the keyboard, Enter is Start, Backspace is Select, the arrow keys are the D-pad, WASD is the left stick, the number pad is the right stick, X/C/V/Z (or K/L/I/J) are Cross/Circle/Triangle/Square, Q and E are L1 and R1, and 1 and 3 are L2 and R2. Escape closes the game.

A few optional environment variables:

| Variable | Effect |
|---|---|
| `YZ_GPU=high-performance` | Render on the discrete GPU of a laptop with two GPUs. By default Windows chooses, usually the integrated GPU that drives the display, which is faster here because the game is limited by the CPU. |
| `YZ_SPURS_PARALLEL_JOBS=0` | Run SPU jobs on one host thread instead of two. |
| `YZ_PPU_AFFINITY=<mask>` | Pin the game's PPU threads to the host cores in this mask. |
| `YZ_THREAD_PRIO=1` | Map the game's thread priorities onto Windows thread priorities. |
| `YZ_SEPARATE_CRITICAL_CORES=1` | Give the RSX consumer and the main SPU worker their own cores. |
| `RSX_D3D_DEBUG=1` | Enable the Direct3D 12 debug layer (needs the Graphics Tools feature). |

## How it works

`tools/` holds the recompiler: an ELF and PRX loader, PPU and SPU disassemblers, and the lifters that turn each PPU function and each SPU program into C/C++. `runtime/` is the machine the lifted code runs on: guest memory, PPU and SPU contexts, the lv2 system calls and SPU DMA. `libs/` replaces the PS3 system libraries the game imports, including SPURS, the gcm graphics library, audio, input, save data and the filesystem. `yakuza/` is the game itself: the runner, the few game functions replaced by hand (such as movie playback) and the Direct3D 12 renderer's hookup to the game's RSX command stream.

## Credits

The original ps3recomp toolkit and runtime were created by [sp00nznet](https://github.com/sp00nznet/ps3recomp); this port started as a fork of it.

The PowerPC, Cell Broadband Engine, SPU and AltiVec manuals from IBM and Power.org, and the NV40 documentation from the envytools and Mesa projects and psdevwiki, describe the hardware this port implements. RPCS3 was used as a behavioural reference when checking results; none of its code is included.

## Legal

This project contains no game code, game data, firmware, encryption keys or Sony SDK material. You need your own legally obtained copy of the game.

## License

The source code in this repository is available under the [MIT License](LICENSE).
