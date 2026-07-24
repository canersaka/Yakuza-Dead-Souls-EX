add_executable(cell_savedata_test
    test_cell_savedata.c
)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
target_include_directories(cell_savedata_test PRIVATE
    "${PS3RECOMP_ROOT}/include"
    "${PS3RECOMP_ROOT}/libs/system"
    "${PS3RECOMP_ROOT}/runtime"
)

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
