#include "native_swap_image_file.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

int main() {
    namespace fs=std::filesystem;
    using recomp::trace::saveNativeSwapImage;
#if defined(__ANDROID__)
    char pattern[]="/data/local/tmp/recomp-swap-file-test.XXXXXX";
    const auto directory=mkdtemp(pattern);
#else
    char hostPattern[]="/tmp/recomp-swap-file-test.XXXXXX";
    const auto directory=mkdtemp(hostPattern);
#endif
    assert(directory);
    const fs::path root(directory),executable=root/"worker",image=root/"native-swap.rgba",link=root/"link";
    std::vector<std::uint8_t> pixels(recomp::gpu::NativeSwapImagePlan::PixelBytes);
    for(std::size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<std::uint8_t>(i*37+i/720);
    assert(!saveNativeSwapImage("relative/worker",pixels));
    assert(!saveNativeSwapImage(root/"missing"/"worker",pixels));
    assert(!saveNativeSwapImage(executable,{}));
    assert(!fs::exists(image));
    assert(chmod(directory,0777)==0);
    assert(!saveNativeSwapImage(executable,pixels));
    assert(chmod(directory,0700)==0);
    assert(symlink(directory,link.c_str())==0);
    assert(!saveNativeSwapImage(link/"worker",pixels));
    assert(unlink(link.c_str())==0);
    // A final file symlink is rejected without modifying its target.
    assert(symlink("sentinel",image.c_str())==0);
    assert(!saveNativeSwapImage(executable,pixels) && !fs::exists(root/"sentinel"));
    assert(unlink(image.c_str())==0);
    assert(saveNativeSwapImage(executable,pixels));
    struct stat info{};assert(stat(image.c_str(),&info)==0 && info.st_size==static_cast<off_t>(pixels.size()));
    assert((info.st_mode&0777)==0600);
    assert(!saveNativeSwapImage(executable,pixels));
    std::ifstream input(image,std::ios::binary);
    const std::vector<std::uint8_t> actual((std::istreambuf_iterator<char>(input)),{});
    assert(actual==pixels);input.close();
    assert(unlink(image.c_str())==0 && rmdir(directory)==0);
    std::puts("NATIVE_SWAP_IMAGE_FILE: complete private output, no overwrite, size/path/permission/symlink rejection PASS");
}
