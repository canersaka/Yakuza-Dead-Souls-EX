# Yakuza: Dead Souls EX

A work-in-progress native PC port of **Yakuza: Dead Souls** built by statically recompiling the original PlayStation 3 executable.

The project translates the game’s PowerPC PPU code, Cell SPU programs, and NVIDIA RSX workloads into native PC code. It supplies a replacement runtime for the PS3 kernel, system libraries, SPURS, filesystem, audio, input, and graphics interfaces expected by the game.

> This is an experimental preservation and engineering project. It is not currently a finished or fully playable port.

## Current Status

Development began on June 9, 2026, the fifteenth anniversary of the original North American release of Yakuza: Dead Souls

As of July 2026, the port:

- Compiles the game’s lifted PPU and SPU programs into a native Windows executable.
- Executes SPURS components lifted from user-supplied, legally obtained PS3 firmware, including the kernel, system service, taskset policies, and job workloads.
- Boots through the game’s CRT, constructors, memory setup, threading, filesystem, SPURS, audio, and graphics initialization.
- Opens a native 1280×720 Windows window.
- Runs the game’s RSX command stream, frame loop, display flips, and GPU synchronization paths.
- Has progressed through early loading and demonstrated a real screen transition.
- Can translate real NV40 vertex and fragment shaders into HLSL.
- Includes PPU, SPU, synchronization, atomics, and Edge-journal tests

The port is **not yet playable**. The current investigation concerns a later SPU/Edge task-state divergence during scene progression. Live rendering also still needs additional integration and correctness work before the translated gameplay scene is presented reliably during a normal boot.

## Major Engineering Accomplishments

### Recovered the Game’s PPU Code

The original recompilation contained more than **73,000 instructions** that were either not translated or were silently decoded as the wrong operation.

The PPU toolchain was rebuilt and corrected against publicly available IBM, Power.org, and Cell Broadband Engine architecture manuals. Work included:

- Correcting scrambled VMX/AltiVec opcode tables.
- Implementing missing scalar, floating-point, atomic, vector, load/store, rotate, compare, and condition-register operations.
- Correcting big-endian VMX lane handling.
- Implementing PowerPC reservation semantics and memory fences.
- Recovering functions and switch cases located in gaps in the original function map.
- Discovering jump tables in raw firmware PRX modules.
- Preserving TOC state and control flow across direct calls, indirect calls, split functions, and trampoline chains.
- Correcting floating-point NaN, fused-operation, and rounding behavior.

The Yakuza executable is represented by approximately **45,425 OPD-verified function entries** and **57,167 lifted functions**, including recovered gap continuations and jump-table targets.

Current verification includes:

- 1,627 static PPU conformance checks.
- 15,573 generated PPU fuzz checks in the recorded full sweep.
- Full instruction parsing audits across the lifted inputs.
- Zero unresolved-call warnings after current relifts.

### Built a Complete SPU Recompilation Path

The SPU toolchain was expanded from partial scaffolding into a complete static-recompilation pipeline.

Major work includes:

- Decoder and lifter coverage for all **199 SPU ISA operations**.
- Correct RRR, RI7, RI8, RI10, RI16, and RI18 instruction forms.
- Correct SPU branch, link-register, channel, halt, synchronization, and floating-point behavior.
- Big-endian quadword operations and preferred-word register semantics.
- SPU local-store execution with channel and mailbox behavior.
- MFC DMA commands, DMA lists, tag completion, GETLLAR, PUTLLC, and PUTLLUC.
- Cross-PPU/SPU reservation coherence.
- Architected blocking channel reads with wake-up plumbing.
- SPU extended-range floating-point behavior, including denormal flushing and saturation.
- Context preservation across task switches and local-store overlays.

The SPU audit has parsed **567,787 instructions across 20 images with zero decode disagreements**. The current executable SPU conformance suite contains 329 passing cases, with additional fuzz/reference modules covering the major instruction families.

### Executes Sony’s Real SPURS Code

Rather than replacing SPURS with a large collection of approximating stubs, the port can lift and execute Sony’s actual firmware modules and SPU workloads.

That work includes:

- Lifting `libsre` and its PPU exports.
- Running the real SPURS kernel and system-service SPU images.
- Running taskset policies, game SPU tasks, CRI audio workloads, and jobchain modules.
- Supporting multiple SPU images whose code occupies overlapping local-store addresses.
- Image-aware function dispatch and residency tracking.
- Restoring task code and read-only segments after overlay rotation.
- Preserving image identity across nested calls, task adoption, suspension, and resume.
- Correcting job binaries that load at multiple descriptor-selected local-store bases.
- Implementing SPURS workload signals, event queues, task scheduling state, and job completion paths.

This moved the project beyond “SPURS API compatibility” into execution of the console’s real scheduling and workload machinery.

### Translates NV40 Shaders to HLSL

The RSX shader work is a major completed subsystem.

The project contains clean-room decompilers for both sides of the NVIDIA NV40 shader pipeline:

- **NV40 vertex programs → HLSL vertex shaders**
- **NV40 fragment programs → HLSL pixel shaders**

The translators handle:

- NV40 vector and scalar ALU instructions.
- Source swizzles, negation, absolute values, and destination masks.
- Vertex constant banks and viewport transforms.
- Vertex output routing for position, colors, fog, and texture coordinates.
- Inline fragment-program constants.
- Fragment output selection between the NV40 fp16 and fp32 register files.
- Texture sampling and all 16 fragment texture units.
- Shader-control state and per-program hashing.
- RSX-specific result scaling and reciprocal-square-root behavior.
- Shader caching and D3D12 pipeline-state creation.

The offline capture-replay engine walks real Yakuza RSX shader bytecode, translates it, compiles the generated HLSL, and caches a D3D12 PSO for each vertex/fragment shader pair.

That replay pipeline currently executes **1,641 of 1,642 draws** from a captured gameplay frame and produces a composite containing the real interior scene and readable tutorial UI, comparable to the RPCS3 reference replay.

Important rendering defects already identified and fixed include:

- Wrong fragment output-register selection.
- Missing primitive-restart handling.
- Incorrect triangle-strip winding.
- Missing per-channel color masks.
- Shared depth buffers across unrelated zeta targets.
- Incorrect logical render-target dimensions.
- Render-target-as-texture handling.
- D24S8 depth texture decoding.
- BC texture remap behavior.
- G8B8 and R5G6B5 texture decoding.
- CMP32 vertex attributes.
- NV40 result-scale modifiers.
- RSX-specific RSQ and DIVSQ behavior.
- Texture and vertex-buffer cache exhaustion.

The fixes were validated through capture replay, deterministic surface hashes, shader-output inspection, and image comparison.

See [RSX Fragment Programs](docs/RSX_FRAGMENT_PROGRAM.md) and [RSX Graphics](docs/RSX_GRAPHICS.md).

### Built a Live NV4097-to-D3D12 Draw Engine

The proven replay renderer has also been adapted into a live graphics path:

```text
Yakuza RSX FIFO
    → NV4097 method dispatch
    → RSX register/state model
    → NV40 VP/FP translation
    → HLSL compilation and PSO cache
    → D3D12 surfaces, textures, and draws
    → native Windows presentation
```

The live path consumes the game’s own method stream and includes:

- Per-shader D3D12 pipeline caching.
- Guest vertex and index-buffer decoding.
- Primitive restart and strip/fan expansion.
- Render-target and depth-target tracking.
- Render-target-as-texture sampling.
- Texture decoding, remapping, and caching.
- Per-draw vertex constants.
- Blend, depth, stencil, culling, viewport, scissor, and color-mask state.
- GPU reference fences and frame presentation.
- Draw-count and frame-rate telemetry in the window title.

The newer live engine is separate from the older generic `rsx_d3d12_backend.c`, which still contains a placeholder shader path. Final integration into a normal, bridge-free game boot remains unfinished, but the shader translation and gameplay-rendering machinery itself is real and validated.

### Rebuilt the RSX and libgcm Execution Path

The project progressed from a hand-written approximation of `cellGcm` to running lifted Sony `libgcm` code and implementing the lower-level interfaces it expects.

Completed work includes:

- LLE-lifted `libgcm_sys`.
- `sys_rsx` context and device behavior.
- RSX IO-memory mapping.
- Big-endian and host-endian control-register handling.
- FIFO command parsing and control flow.
- CALL, RETURN, JUMP, stopper, label, reference, and report operations.
- Flip queuing, retirement, vblank pacing, and display-label lifecycle.
- Per-method NV4097 state tracking.
- A native Win32 presentation window.
- Capture replay using WARP or hardware D3D12.

The remaining graphics difficulty is largely the timing and interaction between the game’s producer, Sony’s lifted driver code, the SPU journal consumer, and the host RSX consumer—not the absence of a shader translator.

### Built a Native Runtime Around the Recompiled Game

The runtime now covers the parts of the PS3 environment needed to take Yakuza from process entry to its current scene frontier:

- Guest virtual memory and big-endian memory access.
- PPU thread creation, joining, priorities, TLS, and stack handling.
- Mutexes, condition variables, semaphores, event queues, event flags, and timers.
- Filesystem translation and real host file IO.
- Module and NID dispatch.
- Guest callback and function-descriptor dispatch.
- Input through XInput.
- Audio output and CRI-related media infrastructure.
- FFmpeg-backed host movie decoding and presentation.
- Save-data, sysutil, video-output, and system-library compatibility.
- Structured crash reporting and guest-address symbol resolution.

Many of these fixes are game-independent and suitable for upstream ps3recomp.

### Developed Verification and Differential-Debugging Infrastructure

The project is not advanced solely through one-off boot patches. It includes reusable tools for finding and proving correctness problems:

- PPU and SPU semantic conformance suites.
- Deterministic fuzz generators.
- Instruction parse audits.
- HLE ABI audits.
- Endianness audits.
- Lift-structure audits.
- PPU and SPU trace comparison against RPCS3.
- Wait-graph and synchronization stress tools.
- RSX capture replay and image comparison.
- Shader-corpus validation.
- Per-surface render hashes.
- SPU flight recording and multi-context event ordering.
- Kill switches and A/B gates for behavior-changing fixes.

The normal test-enabled MSVC build now registers five suites through CTest, covering both lifters, LV2 synchronization, SPU atomics, and the Yakuza Edge-journal implementation.

## What Is Still Missing?

This is not yet a finished port.

The main remaining work includes:

- Resolving the current SPU/Edge item-state divergence during scene progression. It causes a lockup after the initial loading screen.
- Reaching the next scene naturally without diagnostic bridges or forced progress.
- Completing coordination between the game, lifted SPU journal consumer, and RSX FIFO.
- Making the translated live renderer the normal presentation path.
- Completing remaining NV40 shader edge cases and render-state behavior.
- Completing or replacing lifecycle-only HLE modules where the game requires real behavior.
- Performance work, packaging, configuration, and end-user usability.

A feature being present in the runtime does not necessarily mean it is fully compatible with every PS3 title. Some library modules intentionally provide offline, lifecycle-only, or game-specific behavior.

## Why Static Recompilation?

An emulator translates or interprets guest code while the game runs. Static recompilation performs most of that translation ahead of time:

```text
PS3 executable and SPU programs
    → analyze functions, imports, and control flow
    → lift PPU and SPU instructions
    → generate native C/C++
    → compile and link
    → run against the ps3recomp runtime
```

The long-term benefits are native performance, easier debugging, portability, mod support, and preservation independent of the original hardware.

## Repository Layout

```text
tools/          Binary analysis, disassembly, lifting, fuzzing, and audit tools
runtime/        PPU/SPU execution, memory, LV2 syscalls, and synchronization
libs/           PS3 system-library and RSX implementations
include/        Public ps3recomp headers
tests/          Runtime stress and Yakuza-specific unit tests
yakuza/         Yakuza: Dead Souls runner and game integration
templates/      Starting point for other recompilation projects
docs/           Architecture, graphics, runtime, and porting documentation
```

Generated game and firmware lifts live in `recomp/` and `recomp_prx/`. They are intentionally not distributed and must be produced from files supplied by the user.

## Building the Runtime

Requirements:

- CMake 3.20 or newer
- Ninja
- Python 3.10 or newer
- A C17 and C++20 compiler
- MSVC is the primary toolchain for the Yakuza port

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

## Running the Test Suites

```powershell
cmake -S . -B build-tests -G Ninja -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The test-enabled MSVC build currently registers:

- PPU lifter conformance
- SPU lifter conformance
- LV2 synchronization stress
- SPU reservation and atomics stress
- Yakuza Edge-journal unit tests

## Building the Yakuza Runner

The runner requires generated PPU output and a legally obtained dump of the game executable. No game executable, firmware, keys, or copyrighted game data is included.

After producing the required generated sources:

```powershell
cmake -S yakuza -B yakuza/build -G Ninja
cmake --build yakuza/build --target yakuza_recomp
```

Run it from the repository root:

```powershell
.\yakuza\build\yakuza_recomp.exe game\EBOOT.elf
```

The port is currently developed and tested primarily on Windows x64.

## Documentation

- [Getting Started](docs/GETTING_STARTED.md)
- [Building](docs/BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Runtime Reference](docs/RUNTIME.md)
- [SPU Lifter](docs/SPU_LIFTER.md)
- [RSX Graphics](docs/RSX_GRAPHICS.md)
- [NV40 Fragment Programs](docs/RSX_FRAGMENT_PROGRAM.md)
- [Game Porting Guide](docs/GAME_PORTING_GUIDE.md)
- [Module Status](docs/MODULE_STATUS.md)
- [Contributing](CONTRIBUTING.md)

## Relationship to ps3recomp

This repository is the home of the Yakuza: Dead Souls port. It carries:

- The Yakuza-specific runner and integration code.
- Port-specific diagnostics and compatibility work.
- General fixes to the ps3recomp lifters and runtime.
- Reusable SPU, RSX, shader-translation, and verification infrastructure.

Game-independent fixes are prepared for submission to the upstream [ps3recomp project](https://github.com/sp00nznet/ps3recomp).

## Legal

You must provide your own legally obtained copy of the game and any required system files.

No Sony PlayStation 3 development SDK, leaked SDK documentation, or SDK libraries are included in or required by this project. Implementation work is based on publicly available hardware documentation, independently authored code, clean-room analysis, and behavioral comparison with open-source emulators. Users must provide their own legally obtained game and firmware files.

This repository does not distribute Sony firmware, encryption keys, game executables, game assets, or generated code derived from those copyrighted inputs.

The project is intended for preservation, research, interoperability, and personal use.

## Credits

The original ps3recomp toolkit and runtime were created by [sp00nznet](https://github.com/sp00nznet).

RPCS3 is used as a behavioral reference and testing oracle. Its source is not copied into this MIT-licensed project.

Architectural behavior is also verified against publicly available PowerPC, Cell Broadband Engine, SPU, AltiVec, NVIDIA NV40, Mesa/nouveau, and RSX documentation.

## License

Project-authored source code is available under the [MIT License](LICENSE).
