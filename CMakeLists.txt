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
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

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
# Exclude test/replay harnesses (e.g. libs/video/tests/* from the Track B
# capture-replay work): they have their own main() and standalone includes.
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
    "runtime/*.c"
    "runtime/*.cpp"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
        winmm       # timeBeginPeriod (runtime/syscalls/sys_timer.c, audit item 6)
        synchronization # WaitOnAddress/WakeByAddressAll for staged SPU work publication
    )
endif()

# ---------------------------------------------------------------------------
# FFmpeg (host movie decode: CRI Sofdec .sfd = MPEG-1 video + ADX audio).
# Optional: compiled into libs/codec/movie_ffmpeg.c only when the FFmpeg dev
# libs are present. Defaults to the in-repo RPCS3 3rdparty copy; override with
# -DYZ_FFMPEG_DIR=<dir>. Without it, movie_ffmpeg.c stays a no-op stub so the
# runtime always links.  DLLs (avcodec/avformat/avutil/swscale) must be on PATH
# or beside the exe at run time.
# ---------------------------------------------------------------------------
if(NOT YZ_FFMPEG_DIR)
    set(YZ_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rpcs3/3rdparty/ffmpeg")
endif()
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
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    enable_testing()

    add_subdirectory(tests/sync_stress)
    add_subdirectory(tests/spu_atomics_stress)
    add_subdirectory(tests/spu_job_dispatch)
    add_subdirectory(tests/edge_journal_hle)
    add_subdirectory(libs/system/tests)
    add_subdirectory(tests/native_spurs)

    add_executable(fifo_publication_tests
        tests/fifo_publication/main.c
    )
    target_include_directories(fifo_publication_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    add_test(
        NAME fifo_publication_island_edge
        COMMAND fifo_publication_tests
    )
    set_tests_properties(
        fifo_publication_island_edge
        PROPERTIES LABELS "firmware-free;native;gcm;regression"
    )

    # These two suites currently use the MSVC-based generated-code harness.
    # Keep them visible to CTest where that harness is supported; the native
    # stress tests above remain portable across the other toolchains.
    if(MSVC)
        add_executable(rsx_vertex_compact_tests
            libs/video/tests/test_vertex_compact.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_dispatch.c
            libs/video/rsx_vp_decompiler.c
        )
        target_include_directories(rsx_vertex_compact_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_vertex_compact_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        add_test(
            NAME rsx_vertex_compact
            COMMAND rsx_vertex_compact_tests
        )

        add_executable(rsx_guest_pages_tests
            libs/video/tests/test_guest_pages.c
            libs/video/rsx_guest_pages.c
        )
        target_include_directories(rsx_guest_pages_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_guest_pages_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_guest_pages_tests PRIVATE
            /experimental:c11atomics
        )
        add_test(
            NAME rsx_guest_pages
            COMMAND rsx_guest_pages_tests
        )

        add_executable(rsx_gpu_mirror_tests
            libs/video/tests/test_gpu_mirror.c
            libs/video/rsx_gpu_mirror.c
            libs/video/rsx_guest_pages.c
        )
        target_include_directories(rsx_gpu_mirror_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_gpu_mirror_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_gpu_mirror_tests PRIVATE
            /experimental:c11atomics
        )
        add_test(
            NAME rsx_gpu_mirror
            COMMAND rsx_gpu_mirror_tests
        )

        add_executable(rsx_vertex_pull_tests
            libs/video/tests/test_vertex_pull.c
            libs/video/rsx_vertex_pull.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_dispatch.c
            libs/video/rsx_vp_decompiler.c
        )
        target_include_directories(rsx_vertex_pull_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_vertex_pull_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        add_test(
            NAME rsx_vertex_pull
            COMMAND rsx_vertex_pull_tests
        )

        # WARP execution parity: generated GPU decode vs the CPU vertex
        # path, on top of the real tracker -> mirror -> D3D12 upload chain.
        # Exit 2 = no WARP device on this machine (skip, not failure).
        add_executable(rsx_vertex_pull_gpu_tests
            libs/video/tests/test_vertex_pull_gpu.c
            libs/video/rsx_vertex_pull.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_dispatch.c
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_guest_pages.c
            libs/video/rsx_gpu_mirror.c
            libs/video/rsx_gpu_mirror_d3d12.c
        )
        target_include_directories(rsx_vertex_pull_gpu_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_vertex_pull_gpu_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_vertex_pull_gpu_tests PRIVATE
            /experimental:c11atomics
        )
        target_link_libraries(rsx_vertex_pull_gpu_tests PRIVATE
            d3d12 dxgi d3dcompiler
        )
        add_test(
            NAME rsx_vertex_pull_gpu
            COMMAND rsx_vertex_pull_gpu_tests
        )
        set_tests_properties(rsx_vertex_pull_gpu PROPERTIES
            SKIP_RETURN_CODE 2
        )

        add_executable(rsx_fp_despecialization_tests
            libs/video/tests/test_fp_despecialization.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(rsx_fp_despecialization_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_fp_despecialization_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_link_libraries(rsx_fp_despecialization_tests PRIVATE
            d3dcompiler
        )
        add_test(
            NAME rsx_fp_despecialization
            COMMAND rsx_fp_despecialization_tests
        )

        add_executable(rsx_wait_classifier_tests
            yakuza/tests/test_rsx_wait_classifier.cpp
            yakuza/rsx_wait_classifier.cpp
        )
        target_include_directories(rsx_wait_classifier_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/yakuza"
        )
        target_compile_definitions(rsx_wait_classifier_tests PRIVATE
            YZ_RSX_WAIT_CLASSIFIER_TEST=1
            _CRT_SECURE_NO_WARNINGS
        )
        add_test(
            NAME rsx_wait_classifier
            COMMAND rsx_wait_classifier_tests
        )

        add_executable(fe0_timeline_tests
            tests/fe0_timeline/main.c
            runtime/diag/yz_fe0_timeline.c
        )
        target_include_directories(fe0_timeline_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_compile_definitions(fe0_timeline_tests PRIVATE
            YZ_FE0_TIMELINE_TEST=1
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(fe0_timeline_tests PRIVATE
            /experimental:c11atomics
        )
        add_test(
            NAME fe0_timeline
            COMMAND fe0_timeline_tests
        )

        add_executable(wkl4_cycle_tests
            tests/wkl4_cycle/main.c
            runtime/diag/yz_wkl4_cycle.c
        )
        target_include_directories(wkl4_cycle_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_compile_definitions(wkl4_cycle_tests PRIVATE
            YZ_WKL4_CYCLE_TEST=1
            YZ_WKL4_CYCLE_DIAGNOSTIC=1
        )
        if(MSVC)
            target_compile_options(wkl4_cycle_tests PRIVATE
                /experimental:c11atomics
            )
        endif()
        add_test(
            NAME wkl4_cycle
            COMMAND wkl4_cycle_tests
        )

        foreach(ls128_mode IN ITEMS scalar simd)
            add_executable(spu_ls128_${ls128_mode}_tests
                tests/spu_ls128/main.c
            )
            target_include_directories(spu_ls128_${ls128_mode}_tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            )
            target_compile_definitions(spu_ls128_${ls128_mode}_tests PRIVATE
                YZ_SPU_SIMD_LS128=$<STREQUAL:${ls128_mode},simd>
                _CRT_SECURE_NO_WARNINGS
            )
            if(MSVC)
                target_compile_options(spu_ls128_${ls128_mode}_tests PRIVATE
                    /experimental:c11atomics
                )
            endif()
            add_test(
                NAME spu_ls128_${ls128_mode}
                COMMAND spu_ls128_${ls128_mode}_tests
            )
        endforeach()

        foreach(absdb_mode IN ITEMS scalar simd)
            add_executable(spu_absdb_${absdb_mode}_tests
                tests/spu_absdb/main.c
            )
            target_include_directories(spu_absdb_${absdb_mode}_tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            )
            target_compile_definitions(spu_absdb_${absdb_mode}_tests PRIVATE
                YZ_SPU_SIMD_ABSDB=$<STREQUAL:${absdb_mode},simd>
                _CRT_SECURE_NO_WARNINGS
            )
            if(MSVC)
                target_compile_options(spu_absdb_${absdb_mode}_tests PRIVATE
                    /experimental:c11atomics
                )
            endif()
            add_test(
                NAME spu_absdb_${absdb_mode}
                COMMAND spu_absdb_${absdb_mode}_tests
            )
            set_tests_properties(spu_absdb_${absdb_mode} PROPERTIES
                TIMEOUT 120
                LABELS "firmware-free;native;spu;performance;regression")
        endforeach()

        add_executable(spu_xfloat_simd_tests
            tests/spu_xfloat_simd/main.c
            runtime/spu/spu_xfloat_simd.c
        )
        target_include_directories(spu_xfloat_simd_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
        )
        target_compile_definitions(spu_xfloat_simd_tests PRIVATE
            YZ_SPU_SIMD_XFLOAT=1
            _CRT_SECURE_NO_WARNINGS
        )
        if(MSVC)
            target_compile_options(spu_xfloat_simd_tests PRIVATE
                /experimental:c11atomics
                /arch:AVX2
            )
        endif()
        add_test(
            NAME spu_xfloat_simd
            COMMAND spu_xfloat_simd_tests
        )
        set_tests_properties(spu_xfloat_simd PROPERTIES
            TIMEOUT 120
            LABELS "firmware-free;native;spu;performance;regression")

        # Native-render IR equivalence: packet path (FIFO/method stream ->
        # rsx_dispatch -> adapter) vs typed native path must fold to
        # identical action sequences; plus the submission ring and an
        # optional real-capture leg — captures are large untracked local
        # oracles, so CTest never depends on one.
        add_executable(rsx_nir_tests
            libs/video/tests/test_rsx_nir.c
            libs/video/rsx_nir.c
            libs/video/rsx_nir_emitter.c
            libs/video/rsx_nir_adapter.c
            libs/video/rsx_nr_intercept.c
            libs/video/rsx_nr_ring.c
            libs/video/rsx_nr_backend.c
            libs/video/rsx_dispatch.c
        )
        target_include_directories(rsx_nir_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_nir_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        add_test(
            NAME rsx_nir_equivalence
            COMMAND rsx_nir_tests
        )
        set_tests_properties(
            rsx_nir_equivalence
            PROPERTIES TIMEOUT 30 LABELS "regression;renderer;native-render"
        )

        add_executable(rsx_nr_span_router_tests
            libs/video/tests/test_nr_span_router.c
            libs/video/rsx_nr_span_router.c
        )
        target_include_directories(rsx_nr_span_router_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_nr_span_router_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_nr_span_router_tests PRIVATE
            /experimental:c11atomics
        )
        add_test(
            NAME rsx_nr_span_router
            COMMAND rsx_nr_span_router_tests
        )
        set_tests_properties(
            rsx_nr_span_router
            PROPERTIES TIMEOUT 30 LABELS "regression;renderer;native-render"
        )

        # Native-render resource model: mapping epochs, resource lifetime
        # cache over the guest-page tracker, PSO cache, timeline fences.
        add_executable(rsx_nr_resources_tests
            libs/video/tests/test_nr_resources.c
            libs/video/rsx_nr_resources.c
            libs/video/rsx_guest_pages.c
        )
        target_include_directories(rsx_nr_resources_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_nr_resources_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_nr_resources_tests PRIVATE
            /experimental:c11atomics
        )
        add_test(
            NAME rsx_nr_resources
            COMMAND rsx_nr_resources_tests
        )
        set_tests_properties(
            rsx_nr_resources
            PROPERTIES TIMEOUT 30 LABELS "regression;renderer;native-render"
        )

        # Native-render D3D12 sink on WARP: the typed command path drives
        # real rasterization offline (clear, vertex-pulled draws over the
        # guest mirror, dirty re-upload, in-shader index fetch, readback).
        # Exit 2 = no WARP device on this machine (skip, not failure).
        add_executable(rsx_nr_backend_d3d12_tests
            libs/video/tests/test_nr_backend_d3d12.c
            libs/video/rsx_nir.c
            libs/video/rsx_nir_emitter.c
            libs/video/rsx_nir_adapter.c
            libs/video/rsx_nr_ring.c
            libs/video/rsx_nr_backend.c
            libs/video/rsx_nr_backend_d3d12.c
            libs/video/rsx_nr_resources.c
            libs/video/rsx_guest_pages.c
            libs/video/rsx_gpu_mirror.c
            libs/video/rsx_gpu_mirror_d3d12.c
            libs/video/rsx_vertex_pull.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_dispatch.c
        )
        target_include_directories(rsx_nr_backend_d3d12_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_nr_backend_d3d12_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_compile_options(rsx_nr_backend_d3d12_tests PRIVATE
            /experimental:c11atomics
        )
        target_link_libraries(rsx_nr_backend_d3d12_tests PRIVATE
            d3d12 dxgi d3dcompiler
        )
        add_test(
            NAME rsx_nr_backend_d3d12
            COMMAND rsx_nr_backend_d3d12_tests
        )
        set_tests_properties(rsx_nr_backend_d3d12 PROPERTIES
            SKIP_RETURN_CODE 2
            TIMEOUT 60
            LABELS "regression;renderer;native-render"
        )

        # Manual capture differential: feeds one RXS frame through the exact
        # production live renderer.  It is intentionally not a CTest because
        # the title capture is a large, local, untracked oracle.
        add_executable(rsx_live_replay
            libs/video/tests/live_replay_main.c
        )
        target_include_directories(rsx_live_replay PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_compile_definitions(rsx_live_replay PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        target_link_libraries(rsx_live_replay PRIVATE
            ps3recomp_runtime
            d3d12 dxgi d3dcompiler user32 gdi32
        )

        find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
        add_test(
            NAME ppu_lifter_conformance
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_lift.py"
        )
        add_test(
            NAME spu_lifter_conformance
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_lift.py"
        )
        add_test(
            NAME stage_transform_producer_replay
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_stage_transform_producer.py"
        )
        add_test(
            NAME movie_fifo_handoff
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_movie_handoff.py"
        )
        add_test(
            NAME rsx_wait_classifier_mapping
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_rsx_wait_classifier_mapping.py"
        )
        add_test(
            NAME spu_profile_guest_pc
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_profile_pc.py"
        )
        add_test(
            NAME spu_inventory_completeness
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_inventory.py"
        )
        set_tests_properties(
            ppu_lifter_conformance
            spu_lifter_conformance
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 300
                LABELS "conformance;lifter"
        )
        set_tests_properties(
            stage_transform_producer_replay
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 30
                LABELS "regression;renderer"
        )
        set_tests_properties(
            movie_fifo_handoff
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 30
                LABELS "regression;renderer;movie"
        )
        set_tests_properties(
            spu_profile_guest_pc
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 30
                LABELS "regression;spu;profiling"
        )
        set_tests_properties(
            spu_inventory_completeness
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 30
                LABELS "regression;spu;inventory"
        )
    endif()
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
