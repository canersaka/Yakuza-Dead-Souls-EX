cmake_minimum_required(VERSION 3.20)
project(YakuzaRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Yakuza: Dead Souls PS3->PC static recomp runner"
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Everything here uses the static CRT (/MT) so the link doesn't mix LIBCMT
# with MSVCRT*.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(YZ_SPURS_BACKEND "LLE" CACHE STRING "SPURS backend: LLE (default) or NATIVE")
set_property(CACHE YZ_SPURS_BACKEND PROPERTY STRINGS LLE NATIVE)
string(TOUPPER "${YZ_SPURS_BACKEND}" YZ_SPURS_BACKEND)
if(NOT YZ_SPURS_BACKEND STREQUAL "LLE" AND
   NOT YZ_SPURS_BACKEND STREQUAL "NATIVE")
    message(FATAL_ERROR "YZ_SPURS_BACKEND must be LLE or NATIVE")
endif()
string(TOLOWER "${YZ_SPURS_BACKEND}" _yz_spurs_backend_arg)

# A separate worktree can use the original tree's decrypted, untracked game
# and firmware inputs while compiling the current branch's tracked lifts.
set(YZ_GAME_INPUT_DIR "${PS3RECOMP_DIR}" CACHE PATH
    "Tree containing game/, recomp_prx/, and other decrypted Yakuza inputs")
set(YZ_RECOMP_DIR "${PS3RECOMP_DIR}/recomp")
set(YZ_RECOMP_PRX_DIR "${PS3RECOMP_DIR}/recomp/prx")
set(YZ_FIRMWARE_DIR "${YZ_GAME_INPUT_DIR}/recomp_prx")

option(YZ_PERF_FAST
    "Build the clean benchmark lane: compile normal-path diagnostics and hot-path environment queries out."
    OFF)
option(YZ_PERF_PROFILE
    "Build the symbolized aggregate-profile lane with the same diagnostic cleanup as the fast lane."
    OFF)
option(YZ_PPU_SAMPLE
    "Add the low-noise, fixed-window main-PPU instruction sampler."
    OFF)
if(YZ_PERF_FAST AND YZ_PERF_PROFILE)
    message(FATAL_ERROR "YZ_PERF_FAST and YZ_PERF_PROFILE are distinct lanes; enable only one.")
endif()
if(YZ_PERF_FAST OR YZ_PERF_PROFILE)
    list(APPEND YZ_PERF_DEFINITIONS YZ_PERF_CLEAN=1)
endif()
if(YZ_PERF_PROFILE)
    list(APPEND YZ_PERF_DEFINITIONS YZ_PERF_PROFILE=1)
    if(MSVC)
        # Optimized, symbolized sampling lane. Keep functions distinct so
        # profiler samples resolve to the actual lifted/runtime function.
        add_compile_options(/Zi)
        add_link_options(/DEBUG:FULL /OPT:REF /OPT:NOICF /INCREMENTAL:NO)
    endif()
endif()
# Build the runtime library in this tree (with the CRT setting above).
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
if(YZ_PERF_DEFINITIONS)
    target_compile_definitions(ps3recomp_runtime PRIVATE ${YZ_PERF_DEFINITIONS})
endif()
if(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    target_compile_definitions(ps3recomp_runtime PRIVATE
        $<$<CONFIG:Release>:NDEBUG>)
endif()
target_include_directories(ps3recomp_runtime PRIVATE
    "${PS3RECOMP_DIR}/yakuza/generated")
target_compile_definitions(ps3recomp_runtime PRIVATE
    PS3RECOMP_SPU_IMAGE_TABLE_HEADER="spu_image_table.h")

# The lifted PPU code is split into ppu_recomp_NNN.cpp chunks (one giant file
# exceeds the MSVC source-line limit). The chunks go into their own object
# library with a small job pool: each chunk compile takes a lot of memory,
# and letting ninja run one per core exhausts RAM and freezes the machine.
# Raise RECOMP_JOBS only if you have plenty of free memory.
set(RECOMP_JOBS 4 CACHE STRING "Concurrent lifted-chunk compiles")
set_property(GLOBAL PROPERTY JOB_POOLS recomp_pool=${RECOMP_JOBS})

file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS
    "${YZ_RECOMP_DIR}/ppu_recomp_*.cpp")
if(NOT RECOMP_CHUNKS)
    message(FATAL_ERROR "No recomp/ppu_recomp_*.cpp chunks found -- run the lifter first.")
endif()

# The game-owned shader PRX is common to both lanes. Firmware PRX lifts are
# confined to the LLE oracle lane; the native lane resolves those imports to
# independent host implementations and never compiles their lifted images.
file(GLOB PRX_CHUNKS CONFIGURE_DEPENDS
    "${YZ_RECOMP_PRX_DIR}/pxd_shader_recomp_*.cpp")
if(YZ_SPURS_BACKEND STREQUAL "LLE")
    file(GLOB _YZ_FIRMWARE_CHUNKS CONFIGURE_DEPENDS
        "${YZ_RECOMP_PRX_DIR}/libgcm_sys_recomp_*.cpp"
        "${YZ_RECOMP_PRX_DIR}/libsre_recomp_*.cpp")
    list(APPEND PRX_CHUNKS ${_YZ_FIRMWARE_CHUNKS})
endif()

add_library(ppu_recomp_objs OBJECT ${RECOMP_CHUNKS} ${PRX_CHUNKS})
set_target_properties(ppu_recomp_objs PROPERTIES JOB_POOL_COMPILE recomp_pool)
if(YZ_SPURS_BACKEND STREQUAL "NATIVE")
    target_compile_definitions(ppu_recomp_objs PRIVATE
        PS3RECOMP_NATIVE_SPURS_PPU_WATCH=1)
endif()
target_include_directories(ppu_recomp_objs PRIVATE
    "${YZ_RECOMP_DIR}"
    "${YZ_RECOMP_PRX_DIR}")
option(YZ_RECOMP_OPT
    "Optimize the lifted recomp chunks (/O1). Costly compile; default OFF keeps the fast-iteration /Od build. s31: /Od measured as a prime suspect in the guest-code slowness that loses the boot-preload race (STATUS ⚡2)."
    OFF)
option(YZ_A010_FUNC_TRACE
    "Instrument the AUTH renderer chunk with MSVC /Gh for YZ_A010_MODELREAD diagnostics."
    OFF)
if(MSVC)
    if(YZ_RECOMP_OPT)
        # /O1 not /O2: MSVC on 400k-line machine-generated TUs — favor-size
        # optimizes far cheaper than /O2 and captures most of the win vs /Od
        # (inlining the vm helpers is the bulk of it).
        target_compile_options(ppu_recomp_objs PRIVATE /O1 /bigobj /W0)
    else()
        # /Od: fast iteration; optimizing costs minutes and gigabytes per chunk.
        target_compile_options(ppu_recomp_objs PRIVATE /Od /bigobj /W0)
    endif()

    # a010 orphanage render-path probe. Instrument only diagnostic builds:
    # even an immediately-returning _penter call on every generated function
    # is measurable in the normal renderer.
    if(YZ_A010_FUNC_TRACE)
        set_source_files_properties(
            "${PS3RECOMP_DIR}/recomp/ppu_recomp_000.cpp"
            PROPERTIES COMPILE_OPTIONS "/Gh")
    endif()
endif()

# s41/s50 SPU REGION LIFT: per-family opt-in FAST (region-lifted) source
# selection. Every family defaults to the per-instruction DIAG lift; turning a
# family's flag ON builds the tracked yakuza/generated/fast/<stem>_fast.c twins generated by
# tools/gen_spu_regions.py from tools/spu_region_manifest.json (s50 mass
# region lift). Both twins of a stem export the same
# register symbol, so every swap is drop-in. YZ_SPU_FAST keeps its original
# s41 meaning (gs_task only, the validated pilot). All flags default OFF; the
# instruction-mode sources remain the correctness twins.
option(YZ_SPU_FAST "Build gs_task from the region-lifted FAST source instead of the per-instruction DIAG source." OFF)
option(YZ_SPU_FAST_CRI "cri_audio from the region-lifted FAST source." OFF)
option(YZ_SPU_FAST_WKL4 "wkl4_task from the region-lifted FAST source." OFF)
option(YZ_SPU_FAST_SPUIMG "spuimg_* task images from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_JOB_A "every Job A placement from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_JOB_B "every Job B placement from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_JOB_C "every Job C placement from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_JOB_D "every Job D placement from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_ORPHANAGE "every orphanage-worker placement from region-lifted FAST sources." OFF)
option(YZ_SPU_FAST_SPURS_EXPERIMENTAL "EXPERIMENTAL: the five libsre SPURS payloads (kernel2/sysservice/policy/job_module/ts_exit) from region-lifted FAST sources -- an LLE oracle/fallback lane for the native-HLE track, not its destination." OFF)
option(YZ_SPU_FAST_SPURS_KERNEL2 "Bisect override: region-lift only spurs_kernel2." OFF)
option(YZ_SPU_FAST_SPURS_SYSSERVICE "Bisect override: region-lift only spurs_sysservice." OFF)
option(YZ_SPU_FAST_SPURS_POLICY "Bisect override: region-lift only policy_module." OFF)
option(YZ_SPU_FAST_SPURS_JOB_MODULE "Bisect override: region-lift only job_module." OFF)
option(YZ_SPU_FAST_SPURS_TS_EXIT "Bisect override: region-lift only ts_exit." OFF)

if(YZ_SPU_FAST_SPURS_EXPERIMENTAL)
    set(YZ_SPU_FAST_SPURS_KERNEL2 ON)
    set(YZ_SPU_FAST_SPURS_SYSSERVICE ON)
    set(YZ_SPU_FAST_SPURS_POLICY ON)
    set(YZ_SPU_FAST_SPURS_JOB_MODULE ON)
    set(YZ_SPU_FAST_SPURS_TS_EXIT ON)
endif()

# _yz_spu_pick(<listvar> <fast_flag> <diag_path> <stem>): append either the
# instruction-lifted DIAG source or its tracked region-lifted twin.
function(_yz_spu_pick outvar fast diag stem)
    if(${fast})
        list(APPEND ${outvar} "${PS3RECOMP_DIR}/yakuza/generated/fast/${stem}_fast.c")
    else()
        list(APPEND ${outvar} "${diag}")
    endif()
    set(${outvar} "${${outvar}}" PARENT_SCOPE)
endfunction()

# Lifted SPU images. spurs_kernel1 is excluded (unused; its register fn would
# collide); each image has a distinct --register-name/--func-prefix. The job
# placement set below is the FULL active inventory (matches the
# a010-performance build; image ids in tools/spu_region_manifest.json).
set(SPU_KERNELS "")
if(YZ_SPURS_BACKEND STREQUAL "LLE")
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPURS_KERNEL2 "${PS3RECOMP_DIR}/yakuza/generated/diag/spurs_kernel2.c" spurs_kernel2)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPURS_SYSSERVICE "${PS3RECOMP_DIR}/yakuza/generated/diag/spurs_sysservice.c" spurs_sysservice)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPURS_POLICY "${PS3RECOMP_DIR}/yakuza/generated/diag/policy_module.c" policy_module)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPURS_JOB_MODULE "${PS3RECOMP_DIR}/yakuza/generated/diag/job_module.c" job_module)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPURS_TS_EXIT "${PS3RECOMP_DIR}/yakuza/generated/diag/ts_exit.c" ts_exit)
endif()
_yz_spu_pick(SPU_KERNELS YZ_SPU_FAST "${PS3RECOMP_DIR}/yakuza/generated/diag/gs_task.c" gs_task)          # game render task, EBOOT img #3 @0x0127A580, image 0
_yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_CRI "${PS3RECOMP_DIR}/yakuza/generated/diag/cri_audio.c" cri_audio)  # SOFDEC/ADX codec task, image 3
_yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_WKL4 "${PS3RECOMP_DIR}/yakuza/generated/diag/wkl4_task.c" wkl4_task) # 4-task worker pool, image 4
foreach(stem spuimg_00 spuimg_01 spuimg_02 spuimg_04 spuimg_06 spuimg_08 spuimg_09)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_SPUIMG "${PS3RECOMP_DIR}/yakuza/generated/diag/${stem}.c" "${stem}")
endforeach()
foreach(stem job_bin_a job_bin_a_e400 job_bin_a_c400 job_bin_a_24400 job_bin_a_2a400)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_A "${PS3RECOMP_DIR}/yakuza/generated/diag/${stem}.c" "${stem}")
endforeach()
foreach(stem job_bin_b job_bin_b_4c00 job_bin_b_6c00 job_bin_b_15400 job_bin_b_15800
        job_bin_b_1ec00 job_bin_b_37c00 job_bin_b_39c00 job_bin_b_3b000 job_bin_b_3bc00
        job_bin_b_3dc00)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_B "${PS3RECOMP_DIR}/yakuza/generated/diag/${stem}.c" "${stem}")
endforeach()
_yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_B "${PS3RECOMP_DIR}/yakuza/generated/job_bin_b_3d000.c" job_bin_b_3d000)
foreach(stem job_bin_c_4c00 job_bin_c_e400 job_bin_c_15800 job_bin_c_17800 job_bin_c_1a800
        job_bin_c_1ac00 job_bin_c_1b800 job_bin_c_1d800 job_bin_c_1e800)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_C "${PS3RECOMP_DIR}/yakuza/generated/diag/${stem}.c" "${stem}")
endforeach()
foreach(stem job_bin_d_4c00 job_bin_d_5000 job_bin_d_e400 job_bin_d_15800
        job_bin_d_15c00 job_bin_d_20c00 job_bin_d_23000 job_bin_d_27400
        job_bin_d_2a800 job_bin_d_2b400 job_bin_d_2b800
        job_bin_d_2c400 job_bin_d_2d400 job_bin_d_2e400 job_bin_d_2e800)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_D "${PS3RECOMP_DIR}/yakuza/generated/diag/${stem}.c" "${stem}")
endforeach()
_yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_JOB_D "${PS3RECOMP_DIR}/yakuza/generated/job_bin_d_c400.c" job_bin_d_c400)
foreach(stem job_bin_orphanage job_bin_orphanage_e400 job_bin_orphanage_2d800
        job_bin_orphanage_37c00 job_bin_orphanage_3b000 job_bin_orphanage_3bc00
        job_bin_orphanage_3c000 job_bin_orphanage_3cc00)
    _yz_spu_pick(SPU_KERNELS YZ_SPU_FAST_ORPHANAGE "${PS3RECOMP_DIR}/yakuza/generated/${stem}.c" "${stem}")
endforeach()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
set(YZ_IMPORT_BRIDGES "${CMAKE_CURRENT_BINARY_DIR}/import_bridges_gen.cpp")
# Import binding is derived from function definitions across the independent
# HLE/runtime sources.  Keep those files in the generator dependency graph so
# adding an implementation cannot leave a stale ENOSYS bridge in an otherwise
# successful incremental build.
file(GLOB_RECURSE _YZ_IMPORT_IMPL_SOURCES CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/libs/*.c"
    "${PS3RECOMP_DIR}/libs/*.cpp"
    "${PS3RECOMP_DIR}/runtime/*.c"
    "${PS3RECOMP_DIR}/runtime/*.cpp")
list(FILTER _YZ_IMPORT_IMPL_SOURCES EXCLUDE REGEX "/tests/")
add_custom_command(
    OUTPUT "${YZ_IMPORT_BRIDGES}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_imports.py"
            --spurs-backend "${_yz_spurs_backend_arg}"
            --elf "${YZ_GAME_INPUT_DIR}/game/EBOOT.elf"
            --firmware-dir "${YZ_FIRMWARE_DIR}"
            --output "${YZ_IMPORT_BRIDGES}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_imports.py"
            "${YZ_GAME_INPUT_DIR}/game/EBOOT.elf"
            ${_YZ_IMPORT_IMPL_SOURCES}
    VERBATIM)

set(_YZ_FUNC_HEADERS "${YZ_RECOMP_DIR}/ppu_recomp.h")
if(YZ_SPURS_BACKEND STREQUAL "LLE")
    list(APPEND _YZ_FUNC_HEADERS
        "${YZ_RECOMP_PRX_DIR}/libsre_recomp.h"
        "${YZ_RECOMP_PRX_DIR}/libgcm_sys_recomp.h")
endif()
list(APPEND _YZ_FUNC_HEADERS
    "${YZ_RECOMP_PRX_DIR}/pxd_shader_recomp.h")
set(YZ_FUNC_TABLE "${CMAKE_CURRENT_BINARY_DIR}/func_table_gen.cpp")
add_custom_command(
    OUTPUT "${YZ_FUNC_TABLE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_func_table.py"
            ${_YZ_FUNC_HEADERS} -o "${YZ_FUNC_TABLE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_func_table.py" ${_YZ_FUNC_HEADERS}
    VERBATIM)

add_executable(yakuza_recomp
    main.cpp
    app.rc
    runtime_config.cpp
    dispatch.cpp
    stage_render_checkpoint.cpp
    mwply_hle.cpp
    presentation_clock_hle.cpp
    auth_clock_probe.cpp
    shims.cpp
    threads.cpp
    "${YZ_FUNC_TABLE}"
    "${YZ_IMPORT_BRIDGES}"
    import_overrides.cpp
    edge_journal_hle.cpp
    ${SPU_KERNELS}
    $<TARGET_OBJECTS:ppu_recomp_objs>
)
if(YZ_PPU_SAMPLE)
    target_compile_definitions(yakuza_recomp PRIVATE YZ_PPU_SAMPLE=1)
    target_compile_definitions(ps3recomp_runtime PRIVATE YZ_PPU_SAMPLE=1)
    if(MSVC)
        target_compile_options(yakuza_recomp PRIVATE /Zi)
        target_link_options(yakuza_recomp PRIVATE
            /DEBUG:FULL /OPT:REF /OPT:NOICF /INCREMENTAL:NO)
    endif()
endif()
if(YZ_PERF_DEFINITIONS)
    target_compile_definitions(yakuza_recomp PRIVATE ${YZ_PERF_DEFINITIONS})
endif()
if(MSVC)
    target_compile_options(yakuza_recomp PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    target_compile_definitions(yakuza_recomp PRIVATE
        $<$<CONFIG:Release>:NDEBUG>)
endif()

target_include_directories(yakuza_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${YZ_RECOMP_DIR}"
    "${YZ_RECOMP_PRX_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/libs/video"   # rsx_commands.h / rsx_null_backend.h
    "${PS3RECOMP_DIR}/libs/codec"   # movie_ffmpeg.h (host movie decode)
)

# dbghelp: crash-handler symbol resolution (SymFromAddr against the PDB)
# user32/gdi32: the null RSX backend opens a Win32 window + GDI present
target_link_libraries(yakuza_recomp PRIVATE ps3recomp_runtime dbghelp user32 gdi32)
if(YZ_SPURS_BACKEND STREQUAL "NATIVE")
    target_compile_definitions(yakuza_recomp PRIVATE YZ_NATIVE_SPURS=1)
endif()

# FFmpeg (host movie decode) creates a load-time DLL dependency on the exe.
# Copy the runtime DLLs next to it from an in-repo RPCS3 install, if present.
# Harmless no-op when FFmpeg wasn't linked (movie support stubbed).
file(GLOB _yz_ff_dlls
    "${YZ_GAME_INPUT_DIR}/rpcs3-v*/avcodec-*.dll"  "${YZ_GAME_INPUT_DIR}/rpcs3-v*/avformat-*.dll"
    "${YZ_GAME_INPUT_DIR}/rpcs3-v*/avutil-*.dll"   "${YZ_GAME_INPUT_DIR}/rpcs3-v*/swscale-*.dll"
    "${YZ_GAME_INPUT_DIR}/rpcs3-v*/swresample-*.dll")
if(_yz_ff_dlls)
    add_custom_command(TARGET yakuza_recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_yz_ff_dlls} $<TARGET_FILE_DIR:yakuza_recomp>
        COMMENT "Copying FFmpeg DLLs next to yakuza_recomp")
endif()

if(MSVC)
    target_compile_options(yakuza_recomp PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /bigobj
        /Zc:__cplusplus
        # C sources only: the SPU PRX TUs include spu_context.h, which carries
        # _Atomic fields (s43 channel-atomicity fix); C++ never sees _Atomic.
        $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
    )
    target_compile_definitions(yakuza_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /MAP: symbol->RVA listing so crash-handler RVAs resolve to functions.
    # /OPT:NOICF: do NOT fold identical functions -- the many empty/nop SPU
    # functions (e.g. gs_task 0x7444/0xB770) would otherwise collapse to shared
    # addresses and the SPU spin profiler would mis-attribute hops to them.
    target_link_options(yakuza_recomp PRIVATE /INCREMENTAL:NO /OPT:NOICF
        /MAP:${CMAKE_BINARY_DIR}/yakuza_recomp.map)
endif()

message(STATUS "Yakuza SPURS backend: ${YZ_SPURS_BACKEND}")
if(YZ_SPURS_BACKEND STREQUAL "NATIVE")
    message(STATUS "  firmware-free: SPURS and GCM firmware PPU/SPU payloads excluded")
endif()
