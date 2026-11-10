cmake_minimum_required(VERSION 3.22.1)

project(RecompBootstrap LANGUAGES CXX)

# This source repacks every displayed pixel, including in private debug runs.
# Preserve validation/fences while avoiding an unoptimized per-frame hot loop.
set_source_files_properties(vulkan_smoke.cpp PROPERTIES COMPILE_OPTIONS "-O2")

add_library(recomp_bootstrap SHARED
    guest_address_space.cpp
    guest_cooperative_executor.cpp
    guest_execution_guard.cpp
    guest_image_loader.cpp
    guest_import_registry.cpp
    guest_native_initialization.cpp
    guest_thread_environment.cpp
    guest_thread_storage_pool.cpp
    guest_title_environment.cpp
    guest_xex_header.cpp
    smoke_jni.cpp
    smoke_runtime.cpp
    vulkan_smoke.cpp
    xenos_command_stream.cpp
    shader_constant_upload.cpp
)

# The self-test library shares headers with the game runtime, and those use C++20 (std::span, <bit>) since
# September: with C++17 this target no longer compiles (ship week finding, 2026-10-05). Only this target moves to
# C++20; the loader target below keeps the exact flags the installed loader was built with.
target_include_directories(recomp_bootstrap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime")
target_compile_features(recomp_bootstrap PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(recomp_bootstrap PRIVATE Threads::Threads)
target_compile_options(recomp_bootstrap PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(recomp_bootstrap PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_compile_definitions(recomp_bootstrap PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)

find_library(android_log_library log REQUIRED)
find_library(android_native_library android REQUIRED)
find_library(vulkan_library vulkan REQUIRED)

# The bootstrap target has no libvulkan dependency. It must install the
# AdrenoTools hook before recomp_private_debug loads its Vulkan dependency.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ext/libadrenotools"
    "${CMAKE_CURRENT_BINARY_DIR}/libadrenotools")
add_library(recomp_turnip_bootstrap SHARED turnip_bootstrap.cpp)
target_compile_features(recomp_turnip_bootstrap PRIVATE cxx_std_17)
target_compile_options(recomp_turnip_bootstrap PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(recomp_turnip_bootstrap PRIVATE adrenotools "${android_log_library}" dl)
set_target_properties(recomp_turnip_bootstrap PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_link_libraries(recomp_bootstrap PRIVATE
    "${android_log_library}"
    "${android_native_library}"
    "${vulkan_library}"
)

# The game window's loader (the library keeps its historical name; the Java classes that load it are
# PrivateTraceActivity and ControllerCheckActivity). Built for every build type since ship week: the shipped app
# starts the game with it. It is a small asset-free adapter; the game library itself is never a CMake source or
# dependency. It reaches the app as a file the app verifies (see ShipFirstRun) and this loader opens.
# Sources and flags are exactly those of the debug-only target it replaces, so that a Debug build of it is the
# installed loader byte for byte except for the linker's build id (checked 2026-10-05).
add_library(recomp_private_debug SHARED ../../debug/cpp/private_trace_jni.cpp
    guest_address_space.cpp smoke_runtime.cpp vulkan_smoke.cpp)
target_compile_features(recomp_private_debug PRIVATE cxx_std_17)
target_include_directories(recomp_private_debug PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/host_guest_trace")
target_compile_definitions(recomp_private_debug PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
target_compile_options(recomp_private_debug PRIVATE -Wall -Wextra -Werror)
target_link_libraries(recomp_private_debug PRIVATE dl Threads::Threads
    "${android_log_library}" "${android_native_library}" "${vulkan_library}")
set_target_properties(recomp_private_debug PROPERTIES CXX_VISIBILITY_PRESET hidden)
# The build that is handed out is not debuggable: nobody can look into its storage, and a game library that
# Android's linker refuses would show only as "REJECT NATIVE RUN". With this option the loader writes three log
# lines (tag RecompPrivateTrace): LIBRARY_OPENED, LIBRARY_OPEN_REFUSED with the linker's reason, and
# LIBRARY_ENTRY_MISSING. Only the ship build type turns it on (app/build.gradle.kts); the debug build's loader
# stays the code every qualified session ran with (ship_checks.sh debug-apk compares it with the installed one).
option(RECOMP_LOADER_OPEN_LOG "The loader logs whether the game library opened, and the linker's reason when it did not" OFF)
if(RECOMP_LOADER_OPEN_LOG)
    target_compile_definitions(recomp_private_debug PRIVATE RECOMP_LOADER_OPEN_LOG=1)
endif()
