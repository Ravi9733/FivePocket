cmake_minimum_required(VERSION 3.20)

project(Rpf7KeyScan LANGUAGES CXX)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_executable(rpf7_key_scan_native native_key_scan.cpp)
target_compile_features(rpf7_key_scan_native PRIVATE cxx_std_17)
target_link_libraries(rpf7_key_scan_native PRIVATE OpenSSL::Crypto)
target_compile_options(rpf7_key_scan_native PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-deprecated-declarations>
)
