cmake_minimum_required(VERSION 3.20)
project(edge_journal_hle_test LANGUAGES CXX)

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
