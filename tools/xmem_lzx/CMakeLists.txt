cmake_minimum_required(VERSION 3.20)

project(xmem_lzx LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MSPACK_ROOT
    "${REPOSITORY_ROOT}/tools/XenonRecomp/thirdparty/libmspack/libmspack/mspack")

if(NOT EXISTS "${MSPACK_ROOT}/lzxd.c")
    message(FATAL_ERROR
        "Pinned XenonRecomp libmspack is unavailable; initialize submodules first")
endif()

add_library(xmem_lzx_lib STATIC
    xmem_lzx.cpp
    "${MSPACK_ROOT}/lzxd.c")
target_include_directories(xmem_lzx_lib
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE "${MSPACK_ROOT}")

if(MSVC)
    target_compile_options(xmem_lzx_lib PRIVATE /W4)
else()
    target_compile_options(xmem_lzx_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xmem_lzx main.cpp)
target_link_libraries(xmem_lzx PRIVATE xmem_lzx_lib)
set_target_properties(xmem_lzx PROPERTIES OUTPUT_NAME xmem-lzx)

enable_testing()
add_executable(xmem_lzx_tests tests/frame_parser_test.cpp)
target_link_libraries(xmem_lzx_tests PRIVATE xmem_lzx_lib)
add_test(NAME xmem_lzx_tests COMMAND xmem_lzx_tests)
