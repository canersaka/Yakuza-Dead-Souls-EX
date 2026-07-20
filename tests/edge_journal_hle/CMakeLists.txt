cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(edge_journal_hle_test LANGUAGES CXX)
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(edge_journal_hle_test
    main.cpp
    "${PS3RECOMP_ROOT}/yakuza/edge_journal_hle.cpp"
)

target_include_directories(edge_journal_hle_test PRIVATE
    "${PS3RECOMP_ROOT}/yakuza"
)

if(MSVC)
    target_compile_options(edge_journal_hle_test PRIVATE /W4)
else()
    target_compile_options(edge_journal_hle_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME edge_journal_hle COMMAND edge_journal_hle_test)
set_tests_properties(edge_journal_hle PROPERTIES LABELS "unit;yakuza" TIMEOUT 30)
