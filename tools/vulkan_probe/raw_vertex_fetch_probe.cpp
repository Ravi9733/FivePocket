#include "xenos_float_vertex_fetch.h"
#include "xenos_unorm8_vertex_fetch.h"
#include <charconv>
#include <iostream>
#include <iomanip>
#include <string_view>
// Bounded CPU differential probe. Args: hex index and three hex instruction
// words, optionally three hex previous-full words. Owned BE input via stdin.
// No GPU, game state, shader source emission or default input-register policy.
int main(int argc,char** argv) {
    if (argc!=5 && argc!=8) return 2;
    std::array<std::uint32_t,7> args{};
    for(int i=1;i<argc;++i) {
        const std::string_view text(argv[i]);
        const auto r=std::from_chars(text.data(),text.data()+text.size(),args[i-1],16);
        if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size())return 2;
    }
    std::optional<recomp::gpu::FloatVertexFetch> previous;
    if(argc==8){recomp::gpu::FloatVertexFetch p;
        if(!recomp::gpu::decodeFloatVertexFetch({args[4],args[5],args[6]}, {},p)) {
#ifdef RECOMP_UNORM8_PROBE
            recomp::gpu::Unorm8VertexFetch packed;
            if(!recomp::gpu::decodeUnorm8VertexFetch({args[4],args[5],args[6]}, {},packed))return 2;
            p=packed.layout;
#else
            return 2;
#endif
        }
        previous=p;}
#ifdef RECOMP_UNORM8_PROBE
    recomp::gpu::Unorm8VertexFetch packedPlan;
    const bool decoded=recomp::gpu::decodeUnorm8VertexFetch({args[1],args[2],args[3]},previous,packedPlan);
    const auto& plan=packedPlan.layout;
#else
    recomp::gpu::FloatVertexFetch plan;
    const bool decoded=recomp::gpu::decodeFloatVertexFetch({args[1],args[2],args[3]},previous,plan);
#endif
    if(!decoded){
        std::cout<<"{\"accepted\":false}\n";return 1;}
    std::array<std::uint8_t,4097> data{};
    std::cin.read(reinterpret_cast<char*>(data.data()),data.size());
    if(std::cin.bad())return 2;
    recomp::gpu::KnownVertexVector value;
#ifdef RECOMP_UNORM8_PROBE
    const bool read=recomp::gpu::readUnorm8VertexFetch(packedPlan,data.data(),static_cast<std::size_t>(std::cin.gcount()),args[0],{},value);
#else
    const bool read=recomp::gpu::readFloatVertexFetch(plan,data.data(),static_cast<std::size_t>(std::cin.gcount()),args[0],{},value);
#endif
    std::cout<<"{\"accepted\":true,\"read\":"<<(read?"true":"false")<<",\"slot\":"<<plan.fetchSlot
        <<",\"stride\":"<<plan.strideWords<<",\"offset\":"<<plan.offsetWords
        <<",\"src\":"<<plan.sourceRegister<<",\"src_component\":"<<plan.sourceComponent
        <<",\"dst\":"<<plan.destinationRegister<<",\"components\":"<<plan.components
        <<",\"mini\":"<<(plan.mini?"true":"false")<<",\"known\":"<<unsigned(value.known)<<",\"values\":[";
    for(unsigned i=0;i<4;++i){if(i)std::cout<<',';std::cout<<std::setprecision(9)<<value.values[i];}
    std::cout<<"],\"value_bits\":[";
    for(unsigned i=0;i<4;++i){if(i)std::cout<<',';std::uint32_t bits;
        std::memcpy(&bits,&value.values[i],4);std::cout<<bits;}
    std::cout<<"],\"gpu_executed\":false}\n";
}
