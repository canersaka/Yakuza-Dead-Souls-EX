cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(native_spurs_tests LANGUAGES C)
    enable_testing()
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(native_spurs_tests
    main.c
    "${PS3RECOMP_ROOT}/libs/spurs/cellSpurs.c"
    "${PS3RECOMP_ROOT}/libs/sync/cellSync.c"
    "${PS3RECOMP_ROOT}/runtime/spu/spu_lock_order.c"
    "${PS3RECOMP_ROOT}/runtime/spu/spu_workload.c")
target_compile_definitions(native_spurs_tests PRIVATE
    YZ_SPURS_LOCK_ORDER_TEST=1
    YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST=1
    YZ_SPURS_TEST_GUEST_SIZE=2097152)
target_include_directories(native_spurs_tests PRIVATE
    "${PS3RECOMP_ROOT}/include"
    "${PS3RECOMP_ROOT}/libs/spurs"
    "${PS3RECOMP_ROOT}/libs/sync"
    "${PS3RECOMP_ROOT}/runtime/spu"
    "${PS3RECOMP_ROOT}/runtime")
if(MSVC)
    target_compile_options(native_spurs_tests PRIVATE /W3 /experimental:c11atomics)
else()
    target_compile_options(native_spurs_tests PRIVATE -Wall -Wextra)
    target_link_libraries(native_spurs_tests PRIVATE pthread)
endif()
add_test(NAME native_spurs_tests COMMAND native_spurs_tests)
set_tests_properties(native_spurs_tests PROPERTIES LABELS "spurs;native;firmware-free" TIMEOUT 30)
