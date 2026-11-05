cmake_minimum_required(VERSION 3.20)

project(XexImageTool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)

set(XENON_RECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../XenonRecomp")
set(THIRDPARTY_ROOT "${XENON_RECOMP_ROOT}/thirdparty")

add_subdirectory("${THIRDPARTY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty")
add_subdirectory(
    "${XENON_RECOMP_ROOT}/XenonUtils"
    "${CMAKE_CURRENT_BINARY_DIR}/XenonUtils"
)

add_executable(xex_image xex_image.cpp)
target_link_libraries(xex_image PRIVATE XenonUtils)
target_compile_options(xex_image PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
)
