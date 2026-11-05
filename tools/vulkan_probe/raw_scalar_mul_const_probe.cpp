#include "xenos_scalar_mul_const.h"
#include <charconv>
#include <iostream>
#include <string_view>
int main(int argc,char** argv){
    if(argc!=5)return 2;std::array<std::uint32_t,4> args{};
    for(unsigned i=0;i<4;++i){const std::string_view text(argv[i+1]);
        const auto r=std::from_chars(text.data(),text.data()+text.size(),args[i],16);
        if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size())return 2;}
    if(args[3]>255)return 2;
    recomp::gpu::ScalarMulConst plan;
    if(!recomp::gpu::decodeScalarMulConst({args[0],args[1],args[2]},plan)){std::cout<<"{\"decoded\":false}\n";return 1;}
    std::array<std::uint8_t,33> input{};std::cin.read(reinterpret_cast<char*>(input.data()),input.size());
    if(std::cin.bad() || std::cin.gcount()!=32)return 2;
    recomp::gpu::KnownVertexVector temporary;temporary.known=static_cast<std::uint8_t>(args[3]);
    for(unsigned c=0;c<4;++c){const auto* p=input.data()+16+c*4;
        const std::uint32_t bits=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        std::memcpy(&temporary.values[c],&bits,4);}
    float value=1234.0f;const bool okay=recomp::gpu::evaluateScalarMulConst(plan,input.data(),16,temporary,value);
    std::uint32_t bits;std::memcpy(&bits,&value,4);
    std::cout<<"{\"decoded\":true,\"evaluated\":"<<(okay?"true":"false")<<",\"constant_register\":"<<plan.constantRegister
        <<",\"constant_component\":"<<plan.constantComponent<<",\"temporary_register\":"<<plan.temporaryRegister
        <<",\"temporary_component\":"<<plan.temporaryComponent<<",\"destination_register\":"<<plan.destinationRegister
        <<",\"write_mask\":"<<plan.writeMask<<",\"result_bits\":"<<bits<<",\"gpu_executed\":false}\n";
}
