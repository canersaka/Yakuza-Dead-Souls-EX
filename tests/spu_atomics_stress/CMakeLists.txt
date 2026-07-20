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
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(spu_atomics_stress LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
    "${RUNTIME_SPU_DIR}/spu_fltrec.c"
    "${RUNTIME_SPU_DIR}/spu_lockstep.c"
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

add_test(NAME spu_atomics_stress COMMAND spu_atomics_stress)
set_tests_properties(spu_atomics_stress PROPERTIES LABELS "stress;runtime;spu" TIMEOUT 120)
