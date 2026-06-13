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
file(GLOB PRX_CHUNKS CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/recomp_prx/libsre_recomp_*.cpp")

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
    "${PS3RECOMP_DIR}/recomp_prx/spurs_sysservice.c")

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
)

# dbghelp: crash-handler symbol resolution (SymFromAddr against the PDB)
target_link_libraries(yakuza_recomp PRIVATE ps3recomp_runtime dbghelp)

if(MSVC)
    target_compile_options(yakuza_recomp PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /bigobj
        /Zc:__cplusplus
    )
    target_compile_definitions(yakuza_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /MAP: symbol->RVA listing so crash-handler RVAs resolve to functions
    target_link_options(yakuza_recomp PRIVATE /INCREMENTAL:NO
        /MAP:${CMAKE_BINARY_DIR}/yakuza_recomp.map)
endif()
