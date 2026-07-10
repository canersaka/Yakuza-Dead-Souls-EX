# ps3recomp - tests/spu_atomics_stress
#
# Deterministic conformance/stress suite for the SPU MFC reservation/atomics
# model (GETLLAR/PUTLLC/PUTLLUC + the lock-free GETLLAR fast path + the PPU<->SPU
# lock-line coherence bridge). Mirrors tests/sync_stress's standalone-project
# pattern: its OWN build dir, never yakuza\build, links the REAL runtime source
# (runtime/spu/spu_channels.c, header-only spu_dma.h/spu_context.h) directly so
# it exercises the actual mfc_for() registry + spu_coh_* coherence primitives,
# not a reimplementation.
#
# Build (chain in ONE command -- Launch-VsDevShell is blocked in this harness;
# shell state does not persist between tool calls):
#
#   cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake -S tests\spu_atomics_stress -B tests\spu_atomics_stress\build -G Ninja && ninja -C tests\spu_atomics_stress\build"
#
# Run:
#   .\tests\spu_atomics_stress\build\spu_atomics_stress.exe [seed]
#
cmake_minimum_required(VERSION 3.20)
project(spu_atomics_stress LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Force the STATIC CRT (/MT), like yakuza/CMakeLists.txt does (its own comment:
# "Everything here uses the static CRT (/MT)..."). Confirmed via standalone
# cl.exe repro (identical flags, only the CRT choice varies): compiling
# spu_channels.c with EITHER dynamic-CRT flag (/MD release OR /MDd debug)
# fails with "error C2375: 'getenv': redefinition; different linkage" against
# runtime/spu/spu_context.h:328's local `extern char* getenv(const char*);`
# (used to gate the YZ_QLINE diagnostic in spu_ls_write128) colliding with the
# DLL-imported UCRT getenv declaration; /MT's statically-linked getenv has no
# dllimport decoration to collide with, so it compiles clean. This means ANY
# translation unit that includes spu_context.h under a dynamic-CRT (/MD or
# /MDd) MSVC build breaks -- the real project is unaffected only because
# yakuza/CMakeLists.txt already forces /MT; a plain `cmake --preset` default
# (dynamic CRT, CMake's own MSVC default) or any other consumer of
# spu_context.h would hit this. Reported separately as its own finding, not
# fixed here (verify-then-implement protocol) -- this line only matches this
# harness's CRT choice to the real project's so the harness can link.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ---------------------------------------------------------------------------
# Location of the ps3recomp runtime sources under test. Overridable so a
# seeded-bug self-test could point PS3RECOMP_TREE_ROOT at a mutated mirror
# without touching the real tree (same override convention as sync_stress).
# ---------------------------------------------------------------------------
get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

if(NOT DEFINED RUNTIME_SPU_DIR)
    set(RUNTIME_SPU_DIR "${PS3RECOMP_TREE_ROOT}/runtime/spu")
endif()

add_executable(spu_atomics_stress
    main.c
    "${RUNTIME_SPU_DIR}/spu_channels.c"
)

target_include_directories(spu_atomics_stress PRIVATE
    "${RUNTIME_SPU_DIR}"
    "${PS3RECOMP_TREE_ROOT}/runtime"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(spu_atomics_stress PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # spu_channels.c's lock-line spinlock needs <stdatomic.h>
    )
    target_compile_definitions(spu_atomics_stress PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(spu_atomics_stress PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(spu_atomics_stress PRIVATE pthread)
endif()
