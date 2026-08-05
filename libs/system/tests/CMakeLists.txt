add_executable(cell_savedata_test
    test_cell_savedata.c
)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
target_include_directories(cell_savedata_test PRIVATE
    "${PS3RECOMP_ROOT}/include"
    "${PS3RECOMP_ROOT}/libs/system"
    "${PS3RECOMP_ROOT}/runtime"
)

# Standalone build (no SPU runtime linked): stub out the PPU->SPU lock-line
# coherence externs that runtime/ppu/ppu_memory.h's stores now use.
target_compile_definitions(cell_savedata_test PRIVATE PS3RECOMP_NO_SPU_COH=1)

if(MSVC)
    target_compile_definitions(cell_savedata_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cell_savedata_test PRIVATE
        /experimental:c11atomics
        /W3
        /wd4100
        /wd4244
        /wd4267
        /wd4996
    )
endif()

add_test(NAME cell_savedata COMMAND cell_savedata_test)

add_executable(process_exitspawn_test
    test_process_exitspawn.c
    ../process_exitspawn.c
)
target_include_directories(process_exitspawn_test PRIVATE
    "${PS3RECOMP_ROOT}/include"
    "${PS3RECOMP_ROOT}/libs/system"
)
if(MSVC)
    target_compile_definitions(process_exitspawn_test PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(process_exitspawn_test PRIVATE
        /experimental:c11atomics
        /W3
        /wd4100
        /wd4244
        /wd4267
        /wd4996
    )
endif()
add_test(NAME process_exitspawn COMMAND process_exitspawn_test)
