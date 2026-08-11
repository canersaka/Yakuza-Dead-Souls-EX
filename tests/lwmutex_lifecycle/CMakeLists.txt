# ps3recomp - tests/lwmutex_lifecycle
#
# Conformance + churn-stress test for the sysPrxForUser lwmutex lifecycle
# (the 2026-08-05 boot-13 destroy->create resurrection fix).
# Standalone project: its OWN build dir, never yakuza\build.
#
# Build:
#   cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake -S tests\lwmutex_lifecycle -B tests\lwmutex_lifecycle\build -G Ninja && ninja -C tests\lwmutex_lifecycle\build"
#
# Run:
#   .\tests\lwmutex_lifecycle\build\lwmutex_lifecycle.exe
#   (YZ_LWM_RESURRECT_OK=1 restores the pre-fix behavior; the churn case is
#    EXPECTED TO FAIL under it — that is the pre-fix repro proof.)
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lwmutex_lifecycle LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(lwmutex_lifecycle
    main.c
    "${PS3RECOMP_ROOT}/libs/system/sysPrxForUser.c"
)

target_include_directories(lwmutex_lifecycle PRIVATE
    "${PS3RECOMP_ROOT}/libs/system"
    "${PS3RECOMP_ROOT}/include"
)

if(MSVC)
    target_compile_options(lwmutex_lifecycle PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
    )
    target_compile_definitions(lwmutex_lifecycle PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(lwmutex_lifecycle PRIVATE bcrypt)
else()
    target_compile_options(lwmutex_lifecycle PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(lwmutex_lifecycle PRIVATE pthread)
endif()

add_test(NAME lwmutex_lifecycle COMMAND lwmutex_lifecycle)
set_tests_properties(lwmutex_lifecycle PROPERTIES LABELS "conformance;runtime;sync" TIMEOUT 120)
