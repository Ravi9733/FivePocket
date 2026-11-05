#include "xenos_linear_r8_plane.h"
#include <charconv>
#include <iostream>
#include <string_view>
int main(int argc,char** argv){
    if(argc!=7)return 2;std::array<std::uint32_t,6> words{};
    for(unsigned i=0;i<6;++i){const std::string_view text(argv[i+1]);
        const auto r=std::from_chars(text.data(),text.data()+text.size(),words[i],16);
        if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size())return 2;}
    recomp::gpu::LinearR8Plane plan;if(!recomp::gpu::decodeLinearR8Plane(words,plan))return 1;
    std::vector<std::uint8_t> source(plan.pitch*plan.height+1),output;
    std::cin.read(reinterpret_cast<char*>(source.data()),source.size());
    if(std::cin.bad() || static_cast<std::size_t>(std::cin.gcount())!=source.size()-1)return 2;
    if(!recomp::gpu::unpackLinearR8Plane(plan,source.data(),source.size()-1,output))return 2;
    std::cout.write(reinterpret_cast<const char*>(output.data()),output.size());return std::cout.good()?0:2;
}
