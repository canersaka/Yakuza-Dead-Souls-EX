cmake_minimum_required(VERSION 3.21)
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

# YZ_GAME_INPUT_DIR points at the directory containing game/ (the decrypted
# EBOOT.elf) and the other decrypted game inputs.
set(YZ_GAME_INPUT_DIR "${PS3RECOMP_DIR}" CACHE PATH
    "Tree containing game/ and the other decrypted Yakuza inputs")
set(YZ_RECOMP_DIR "${PS3RECOMP_DIR}/recomp")
set(YZ_RECOMP_PRX_DIR "${PS3RECOMP_DIR}/recomp/prx")

option(YZ_SPU_SIMD_SHUFB
    "Use the bit-exact SSSE3 SPU shufb helper; OFF selects its scalar reference twin."
    ON)
option(YZ_SPU_SIMD_ROTQBY
    "Use exact SSSE3 byte rotations; OFF selects the scalar instruction reference."
    ON)
option(YZ_SPU_SIMD_LS128
    "Use bit-exact SSSE3 word-byte conversion for SPU 128-bit local-store accesses."
    ON)
option(YZ_SPU_SIMD_ABSDB
    "Use the bit-exact SSE2 SPU absolute byte-difference helper."
    ON)
option(YZ_SPU_SIMD_XFLOAT
    "Use the bit-exact AVX2 ordinary-lane fast path for SPU xfloat arithmetic."
    ON)
option(YZ_SPU_EXACT_IMAGE_BYTES
    "Resolve registered SPU images with an owned exact-byte reference before the legacy fingerprint fallback."
    ON)
# Build the runtime library in this tree (with the CRT setting above).
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
# Runtime/HLE bulk writers must use the same sparse watched-page route as
# generated PPU stores so native SPURS waiters see every guest write.
target_compile_definitions(ps3recomp_runtime PRIVATE
    PS3RECOMP_NATIVE_SPURS_PPU_WATCH=1)
if(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    target_compile_definitions(ps3recomp_runtime PRIVATE
        $<$<CONFIG:Release>:NDEBUG>)
endif()
target_compile_definitions(ps3recomp_runtime PRIVATE
    YZ_SPU_SIMD_SHUFB=$<BOOL:${YZ_SPU_SIMD_SHUFB}>
    YZ_SPU_SIMD_ROTQBY=$<BOOL:${YZ_SPU_SIMD_ROTQBY}>
    YZ_SPU_SIMD_LS128=$<BOOL:${YZ_SPU_SIMD_LS128}>
    YZ_SPU_SIMD_ABSDB=$<BOOL:${YZ_SPU_SIMD_ABSDB}>
    YZ_SPU_SIMD_XFLOAT=$<BOOL:${YZ_SPU_SIMD_XFLOAT}>
    YZ_SPU_EXACT_IMAGE_BYTES=$<BOOL:${YZ_SPU_EXACT_IMAGE_BYTES}>)

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

# The game-owned shader PRX is lifted alongside the EBOOT. Firmware modules are
# never lifted: their imports resolve to the host implementations.
file(GLOB PRX_CHUNKS CONFIGURE_DEPENDS
    "${YZ_RECOMP_PRX_DIR}/pxd_shader_recomp_*.cpp")

add_library(ppu_recomp_objs OBJECT ${RECOMP_CHUNKS} ${PRX_CHUNKS})
set_target_properties(ppu_recomp_objs PROPERTIES JOB_POOL_COMPILE recomp_pool)
target_compile_definitions(ppu_recomp_objs PRIVATE
    PS3RECOMP_NATIVE_SPURS_PPU_WATCH=1)
target_include_directories(ppu_recomp_objs PRIVATE
    "${YZ_RECOMP_DIR}"
    "${YZ_RECOMP_PRX_DIR}")
option(YZ_RECOMP_OPT
    "Optimize the lifted PPU chunks. OFF builds them at /Od: much faster to compile, much slower to run."
    ON)
option(YZ_PPU_WHOLE_PROGRAM_LTCG
    "Compile the lifted PPU chunks with /O2 and link-time code generation (MSVC; requires YZ_RECOMP_OPT)."
    ON)
option(YZ_INCREMENTAL_LTCG
    "Use incremental link-time code generation for faster relinks (MSVC; requires YZ_PPU_WHOLE_PROGRAM_LTCG)."
    OFF)
if(YZ_INCREMENTAL_LTCG AND (NOT MSVC OR NOT YZ_PPU_WHOLE_PROGRAM_LTCG))
    message(FATAL_ERROR "YZ_INCREMENTAL_LTCG requires MSVC and YZ_PPU_WHOLE_PROGRAM_LTCG=ON")
endif()
if(MSVC)
    if(YZ_PPU_WHOLE_PROGRAM_LTCG AND NOT YZ_RECOMP_OPT)
        message(FATAL_ERROR
            "YZ_PPU_WHOLE_PROGRAM_LTCG requires YZ_RECOMP_OPT=ON")
    endif()
    if(YZ_RECOMP_OPT)
        # /O1 not /O2: on 400k-line machine-generated TUs MSVC's favor-size
        # optimization is far cheaper than /O2 and captures most of the win
        # vs /Od (inlining the vm helpers is the bulk of it).
        target_compile_options(ppu_recomp_objs PRIVATE /O1 /bigobj /W0)

        # Chunk 000 owns the orphanage worker's periodic cleanup/update path.
        # Optimize that generated unit for speed and lower its fixed-size memcpy
        # and byte-swap operations to native instructions.  This does not change
        # the lifted arithmetic or guest-memory contract.
        set_source_files_properties(
            "${YZ_RECOMP_DIR}/ppu_recomp_000.cpp"
            PROPERTIES COMPILE_OPTIONS "/O2;/Oi")

        if(YZ_PPU_WHOLE_PROGRAM_LTCG)
            # LTCG covers only the lifted PPU objects; the runtime, renderer
            # and SPU lifts keep their own flags.
            target_compile_options(ppu_recomp_objs PRIVATE
                /O2 /Ob3 /Oi /Ot /GL)
        endif()
    else()
        # /Od: fast iteration; optimizing costs minutes and gigabytes per chunk.
        target_compile_options(ppu_recomp_objs PRIVATE /Od /bigobj /W0)
    endif()
endif()

# Lifted SPU programs: tools/gen_spu_images.py writes one region-lifted twin
# per placement in yakuza/spu_images.json, plus the tables that register them.
file(GLOB SPU_KERNELS CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/yakuza/generated/spu/*.c")
if(NOT SPU_KERNELS)
    message(FATAL_ERROR "No yakuza/generated/spu sources found -- run tools/gen_spu_images.py first.")
endif()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)

set(YZ_IMPORT_BRIDGES "${CMAKE_CURRENT_BINARY_DIR}/import_bridges_gen.cpp")
# Import binding is derived from function definitions across the independent
# HLE/runtime sources.  Keep those files in the generator dependency graph so
# adding an implementation cannot leave a stale ENOSYS bridge in an otherwise
# successful incremental build.
file(GLOB_RECURSE _YZ_IMPORT_IMPL_SOURCES CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/libs/*.c"
    "${PS3RECOMP_DIR}/libs/*.cpp"
    "${PS3RECOMP_DIR}/libs/*.inc"
    "${PS3RECOMP_DIR}/runtime/*.c"
    "${PS3RECOMP_DIR}/runtime/*.cpp"
    "${PS3RECOMP_DIR}/runtime/*.inc")
list(FILTER _YZ_IMPORT_IMPL_SOURCES EXCLUDE REGEX "/tests/")
add_custom_command(
    OUTPUT "${YZ_IMPORT_BRIDGES}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_imports.py"
            --elf "${YZ_GAME_INPUT_DIR}/game/EBOOT.elf"
            --output "${YZ_IMPORT_BRIDGES}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_imports.py"
            "${YZ_GAME_INPUT_DIR}/game/EBOOT.elf"
            ${_YZ_IMPORT_IMPL_SOURCES}
    VERBATIM)

set(_YZ_FUNC_HEADERS
    "${YZ_RECOMP_DIR}/ppu_recomp.h"
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
    dispatch.cpp
    mwply_hle.cpp
    presentation_clock_hle.cpp
    native_scene_jobs.c
    native_scene_pose_job.c
    native_pose_generate.c
    native_visibility.c
    shims.cpp
    threads.cpp
    "${YZ_FUNC_TABLE}"
    "${YZ_IMPORT_BRIDGES}"
    import_overrides.cpp
    ${SPU_KERNELS}
    $<TARGET_OBJECTS:ppu_recomp_objs>
)
if(MSVC)
    set_source_files_properties(native_pose_generate.c PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(yakuza_recomp PRIVATE native_visibility_avx2.c)
    set_source_files_properties(native_visibility_avx2.c PROPERTIES
        COMPILE_OPTIONS "/arch:AVX2;/fp:strict")
    set_source_files_properties(native_visibility.c PROPERTIES
        COMPILE_DEFINITIONS "YZ_NATIVE_VISIBILITY_AVX2_AVAILABLE=1")
endif()
target_compile_definitions(yakuza_recomp PRIVATE
    PS3RECOMP_NATIVE_SPURS_PPU_WATCH=1
    YZ_SPU_SIMD_SHUFB=$<BOOL:${YZ_SPU_SIMD_SHUFB}>
    YZ_SPU_SIMD_ROTQBY=$<BOOL:${YZ_SPU_SIMD_ROTQBY}>
    YZ_SPU_SIMD_LS128=$<BOOL:${YZ_SPU_SIMD_LS128}>
    YZ_SPU_SIMD_ABSDB=$<BOOL:${YZ_SPU_SIMD_ABSDB}>
    YZ_SPU_SIMD_XFLOAT=$<BOOL:${YZ_SPU_SIMD_XFLOAT}>
    YZ_SPU_EXACT_IMAGE_BYTES=$<BOOL:${YZ_SPU_EXACT_IMAGE_BYTES}>)
if(MSVC)
    target_compile_options(yakuza_recomp PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    target_compile_definitions(yakuza_recomp PRIVATE
        $<$<CONFIG:Release>:NDEBUG>)
    if(YZ_PPU_WHOLE_PROGRAM_LTCG)
        if(YZ_INCREMENTAL_LTCG)
            target_link_options(yakuza_recomp PRIVATE /LTCG:INCREMENTAL)
        else()
            target_link_options(yakuza_recomp PRIVATE /LTCG)
        endif()
    endif()
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

# A shared FFmpeg build creates a load-time DLL dependency on the exe. Copy
# its DLLs next to the exe when the package has them; static builds and
# stubbed movie support need nothing.
file(GLOB _yz_ff_dlls
    "${YZ_FFMPEG_DIR}/bin/avcodec-*.dll"  "${YZ_FFMPEG_DIR}/bin/avformat-*.dll"
    "${YZ_FFMPEG_DIR}/bin/avutil-*.dll"   "${YZ_FFMPEG_DIR}/bin/swscale-*.dll"
    "${YZ_FFMPEG_DIR}/bin/swresample-*.dll")
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
        # C sources only: the generated SPU sources include spu_context.h, which carries
        # _Atomic fields; C++ never sees _Atomic.
        $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
    )
    target_compile_definitions(yakuza_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /MAP: symbol listing next to the exe, for reading crash-report addresses.
    # /OPT:NOICF: do not fold identical functions. Many lifted SPU functions
    # are empty and would otherwise share an address, which confuses crash
    # reports and the map file.
    target_link_options(yakuza_recomp PRIVATE /INCREMENTAL:NO /OPT:NOICF
        /MAP:${CMAKE_BINARY_DIR}/yakuza_recomp.map)
endif()
