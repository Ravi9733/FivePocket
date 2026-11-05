#include "xenos_tiled32_texture.h"
#include <charconv>
#include <iostream>
#include <string_view>
// Six hex descriptor words; bounded owned tile via stdin; row-major RGBA via
// stdout. Caller must keep private captured inputs/outputs under local-analysis.
int main(int argc,char** argv){
    if(argc!=7)return 2;
    std::array<std::uint32_t,6> words{};
    for(unsigned i=0;i<6;++i){const std::string_view text(argv[i+1]);
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),words[i],16);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())return 2;}
    recomp::gpu::Tiled32Texture plan;if(!recomp::gpu::decodeTiled32Texture(words,plan))return 1;
    std::array<std::uint8_t,4097> bytes{};std::array<std::uint8_t,4096> output{};
    std::cin.read(reinterpret_cast<char*>(bytes.data()),bytes.size());if(std::cin.bad())return 2;
    if(!recomp::gpu::decodeTiled32Pixels(plan,bytes.data(),std::size_t(std::cin.gcount()),output))return 1;
    std::cout.write(reinterpret_cast<const char*>(output.data()),output.size());return std::cout.good()?0:2;
}
