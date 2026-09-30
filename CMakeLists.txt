cmake_minimum_required(VERSION 3.20)
project(ps3recomp_runtime
    VERSION 0.3.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES CONFIGURE_DEPENDS
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
)
# Exclude test/replay harnesses (e.g. libs/video/tests/*): they have their own
# main() and standalone includes.
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
    "runtime/*.c"
)
# Exclude test harnesses (e.g. runtime/spu/tests/*): they have their own main()
# and #include per-game generated headers (spu_recomp.h) that don't exist in a
# plain runtime-library build.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

# Keep AVX2 isolated to the checked xfloat adapter.  The remainder of the
# runtime retains the production baseline ISA and the adapter performs its
# own OS/CPU availability check before any packed instruction is dispatched.
if(DEFINED YZ_SPU_SIMD_XFLOAT AND YZ_SPU_SIMD_XFLOAT AND MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu/spu_xfloat_simd.c"
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
        winmm       # timeBeginPeriod (runtime/syscalls/sys_timer.c)
        synchronization # WaitOnAddress/WakeByAddressAll for staged SPU work publication
    )
endif()

# ---------------------------------------------------------------------------
# FFmpeg (host movie decode: CRI Sofdec .sfd = MPEG-1 video + ADX audio).
# Optional: compiled into libs/codec/movie_ffmpeg.c only when the FFmpeg dev
# libs are present. Point -DYZ_FFMPEG_DIR at an FFmpeg development package
# (include/, lib/, and bin/ for shared builds); the default is third_party/ffmpeg.
# Without it, movie_ffmpeg.c stays a no-op stub so the runtime always links.
# Shared-build DLLs must be on PATH or beside the exe at run time.
# ---------------------------------------------------------------------------
set(YZ_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH
    "FFmpeg development package (include/, lib/, bin/)")
if(WIN32 AND EXISTS "${YZ_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    if(EXISTS "${YZ_FFMPEG_DIR}/lib/windows/x86_64/avcodec.lib")
        set(YZ_FFMPEG_LIB_DIR "${YZ_FFMPEG_DIR}/lib/windows/x86_64")
    elseif(EXISTS "${YZ_FFMPEG_DIR}/lib/avcodec.lib")
        set(YZ_FFMPEG_LIB_DIR "${YZ_FFMPEG_DIR}/lib")
    else()
        message(FATAL_ERROR
            "ps3recomp: FFmpeg headers found but libraries are missing under "
            "${YZ_FFMPEG_DIR}")
    endif()
    message(STATUS "ps3recomp: FFmpeg movie decode ENABLED (${YZ_FFMPEG_DIR})")
    target_compile_definitions(ps3recomp_runtime PRIVATE HAVE_FFMPEG=1)
    target_include_directories(ps3recomp_runtime PRIVATE "${YZ_FFMPEG_DIR}/include")
    target_link_libraries(ps3recomp_runtime PRIVATE
        "${YZ_FFMPEG_LIB_DIR}/avformat.lib"
        "${YZ_FFMPEG_LIB_DIR}/avcodec.lib"
        "${YZ_FFMPEG_LIB_DIR}/avutil.lib"
        "${YZ_FFMPEG_LIB_DIR}/swresample.lib"
        "${YZ_FFMPEG_LIB_DIR}/swscale.lib"
        secur32)
else()
    message(STATUS "ps3recomp: FFmpeg not found (${YZ_FFMPEG_DIR}) -- movie decode stubbed")
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()


# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
