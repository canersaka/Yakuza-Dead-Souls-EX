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

# ppu_recomp.obj was compiled with plain `cl /TP /bigobj` => static CRT (/MT).
# Force the same CRT for everything in this build tree (runtime lib included)
# so the link doesn't mix LIBCMT with MSVCRT*.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Build the runtime library in this tree (with the CRT setting above).
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)

add_executable(yakuza_recomp
    main.cpp
    dispatch.cpp
    shims.cpp
    func_table_gen.cpp
    import_bridges_gen.cpp
    import_overrides.cpp
    "${PS3RECOMP_DIR}/ppu_recomp.obj"
)

target_include_directories(yakuza_recomp PRIVATE
    "${PS3RECOMP_DIR}/recomp"
    "${PS3RECOMP_DIR}/include"
)

target_link_libraries(yakuza_recomp PRIVATE ps3recomp_runtime)

if(MSVC)
    target_compile_options(yakuza_recomp PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /bigobj
        /Zc:__cplusplus
    )
    target_compile_definitions(yakuza_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # The 590 MB object makes incremental/ILK machinery pointless.
    target_link_options(yakuza_recomp PRIVATE /INCREMENTAL:NO)
endif()
