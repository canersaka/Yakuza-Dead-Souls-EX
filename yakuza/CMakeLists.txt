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

# Build the runtime library in this tree (with the CRT setting above).
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)

# The lifted PPU code is split into ppu_recomp_NNN.cpp chunks (one giant file
# exceeds the MSVC source-line limit). The chunks go into their own object
# library with a small job pool: each chunk compile takes a lot of memory,
# and letting ninja run one per core exhausts RAM and freezes the machine.
# Raise RECOMP_JOBS only if you have plenty of free memory.
set(RECOMP_JOBS 4 CACHE STRING "Concurrent lifted-chunk compiles")
set_property(GLOBAL PROPERTY JOB_POOLS recomp_pool=${RECOMP_JOBS})

file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/recomp/ppu_recomp_*.cpp")
if(NOT RECOMP_CHUNKS)
    message(FATAL_ERROR "No recomp/ppu_recomp_*.cpp chunks found -- run the lifter first.")
endif()

# LLE-lifted firmware PRX modules (currently libsre = Sony's SPURS library),
# produced by tools/lift_prx.py + the lifter into recomp_prx/. Optional: the
# runner builds without them until the libsre lift has been run.
file(GLOB PRX_CHUNKS CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/recomp_prx/libsre_recomp_*.cpp"
    "${PS3RECOMP_DIR}/recomp_prx/libgcm_sys_recomp_*.cpp"
    "${PS3RECOMP_DIR}/recomp_prx/pxd_shader_recomp_*.cpp")

add_library(ppu_recomp_objs OBJECT ${RECOMP_CHUNKS} ${PRX_CHUNKS})
set_target_properties(ppu_recomp_objs PROPERTIES JOB_POOL_COMPILE recomp_pool)
target_include_directories(ppu_recomp_objs PRIVATE
    "${PS3RECOMP_DIR}/recomp"
    "${PS3RECOMP_DIR}/recomp_prx")
if(MSVC)
    # /Od: machine-generated code; optimizing it costs minutes and gigabytes
    # per chunk for little runtime benefit at this stage.
    target_compile_options(ppu_recomp_objs PRIVATE /Od /bigobj /W0)
endif()

# Lifted SPU images (Sony's SPURS kernel + system-service workload, extracted
# from libsre by tools/lift_prx.py + tools/spu_lifter.py). Listed explicitly:
# spurs_kernel1.c is excluded (unused; its register fn would collide), and each
# image has a distinct --register-name so their globals don't clash.
file(GLOB SPU_KERNELS CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/recomp_prx/spurs_kernel2.c"
    "${PS3RECOMP_DIR}/recomp_prx/spurs_sysservice.c"
    "${PS3RECOMP_DIR}/recomp_prx/policy_module.c"   # taskset policy module (libsre 0x02023680), loaded to LS 0xA00 over the service
    "${PS3RECOMP_DIR}/recomp_prx/gs_task.c"   # game render task (Edge geometry), EBOOT SPU img #3 @0x0127A580
    "${PS3RECOMP_DIR}/recomp_prx/cri_audio.c"   # SOFDEC/ADX codec task, EBOOT SPU img #7 @0x012B4980 (overlaps gs_task LS -> image 3)
    "${PS3RECOMP_DIR}/recomp_prx/wkl4_task.c"   # 4-task worker pool, EBOOT SPU img #5 @0x01284200 (wid 4; overlaps LS 0x3000 -> image 4)
    "${PS3RECOMP_DIR}/recomp_prx/spuimg_*.c"   # remaining EBOOT task images, batch-lifted by tools/gen_spu_images.py (image ids 5+; see recomp_prx/spu_image_table.h)
    "${PS3RECOMP_DIR}/recomp_prx/ts_exit.c"   # SPURS taskset exit-handler overlay (libsre 0x02025500 -> LS 0x10000, image 12)
    "${PS3RECOMP_DIR}/recomp_prx/job_module.c"   # SPURS jobchain policy module (libsre 0x0202A180 -> LS 0xA00, image 13)
    "${PS3RECOMP_DIR}/recomp_prx/job_bin_a.c"   # jobchain bulk-worker JOB binary (EBOOT 0x01254500 -> ~LS 0x4C00, image 14)
    "${PS3RECOMP_DIR}/recomp_prx/job_bin_b.c"   # jobchain notify JOB binary (EBOOT 0x01275A00 -> ~LS 0xE400, image 15)
    "${PS3RECOMP_DIR}/recomp_prx/job_bin_a_e400.c"   # jobA lifted at the ALTERNATE slot base 0xE400 (jobs are relocatable; measured 2026-07-08)
    "${PS3RECOMP_DIR}/recomp_prx/job_bin_b_4c00.c")   # jobB lifted at the ALTERNATE slot base 0x4C00 (round-1 loads jobB there; measured 2026-07-08)

add_executable(yakuza_recomp
    main.cpp
    dispatch.cpp
    shims.cpp
    threads.cpp
    func_table_gen.cpp
    import_bridges_gen.cpp
    import_overrides.cpp
    ${SPU_KERNELS}
    $<TARGET_OBJECTS:ppu_recomp_objs>
)

target_include_directories(yakuza_recomp PRIVATE
    "${PS3RECOMP_DIR}/recomp"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/libs/video"   # rsx_commands.h / rsx_null_backend.h
    "${PS3RECOMP_DIR}/libs/codec"   # movie_ffmpeg.h (host movie decode)
)

# dbghelp: crash-handler symbol resolution (SymFromAddr against the PDB)
# user32/gdi32: the null RSX backend opens a Win32 window + GDI present
target_link_libraries(yakuza_recomp PRIVATE ps3recomp_runtime dbghelp user32 gdi32)

# FFmpeg (host movie decode) creates a load-time DLL dependency on the exe.
# Copy the runtime DLLs next to it from an in-repo RPCS3 install, if present.
# Harmless no-op when FFmpeg wasn't linked (movie support stubbed).
file(GLOB _yz_ff_dlls
    "${PS3RECOMP_DIR}/rpcs3-v*/avcodec-*.dll"  "${PS3RECOMP_DIR}/rpcs3-v*/avformat-*.dll"
    "${PS3RECOMP_DIR}/rpcs3-v*/avutil-*.dll"   "${PS3RECOMP_DIR}/rpcs3-v*/swscale-*.dll"
    "${PS3RECOMP_DIR}/rpcs3-v*/swresample-*.dll")
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
    )
    target_compile_definitions(yakuza_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /MAP: symbol->RVA listing so crash-handler RVAs resolve to functions.
    # /OPT:NOICF: do NOT fold identical functions -- the many empty/nop SPU
    # functions (e.g. gs_task 0x7444/0xB770) would otherwise collapse to shared
    # addresses and the SPU spin profiler would mis-attribute hops to them.
    target_link_options(yakuza_recomp PRIVATE /INCREMENTAL:NO /OPT:NOICF
        /MAP:${CMAKE_BINARY_DIR}/yakuza_recomp.map)
endif()
