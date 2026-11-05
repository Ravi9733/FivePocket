#include "native_deferred_shader_capture.h"
#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
int main(){
    const auto frame=[](const std::vector<unsigned>& words){
        const auto command=NativeSwapCommand::make({3,0x2000+unsigned(words.size())*4+40,0x100000,73});assert(command);
        std::vector<std::uint8_t> bytes;
        for(auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(w>>(24-i*8));
        const auto tail=test::postOutputSwapBytes(*command);bytes.insert(bytes.end(),tail.begin(),tail.end());
        return NativeDeferredSwapSubmission::parse(*command,0x2000,bytes.data(),bytes.size());
    };
    const std::vector<unsigned> words{0xC0042B00,0,3,0x11223344,0x55667788,0x99AABBCC,0xC0012700,0x40001,3};
    const auto request=frame(words);assert(request);
    unsigned reads=0;NativeDeferredShaderCapture captured;
    const Pm4PhysicalReader reader=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>>{
        assert(a==0x40000 && n==12);++reads;return std::vector<std::uint8_t>(n,0x4A);};
    assert(captureNativeDeferredPrefixShaders(*request,reader,captured) && reads==1 && captured.loads.size()==2 && captured.bytes.size()==2);
    assert(captured.loads[0].node==1 && captured.loads[0].load.immediate && captured.loads[0].load.packetOffsetWords==0 &&
        !captured.loads[1].load.immediate && captured.loads[1].load.packetOffsetWords==6 && captured.bytes[1][0]==0x4A &&
        captured.bytes[0][0]==0x11 && captured.bytes[0][11]==0xCC);
    const auto saved=captured.bytes;
    for(unsigned fault=0;fault<4;++fault){
        const Pm4PhysicalReader failed=[&](auto,auto n)->std::optional<std::vector<std::uint8_t>>{
            if(fault==0)return std::nullopt;if(fault==1)throw std::runtime_error("read failure");
            return std::vector<std::uint8_t>(n+(fault==2?1:-1));};
        assert(!captureNativeDeferredPrefixShaders(*request,failed,captured) && captured.bytes==saved);
    }
    assert(!captureNativeDeferredPrefixShaders(*request,{},captured) && captured.bytes==saved);
    unsigned rejects=0;
    for(const auto& bad:std::vector<std::vector<unsigned>>{
            {0xC0042B01,0,3,1,2,3},{0xC0042B00,2,3,1,2,3},
            {0xC0012700,0x40002,3},{0xC0012700,0x40001,4},{0xC0012700,0x40001,0x10003},
            {0xC0012700,0x100001,3},{0xC0012700,0xFFFF9,3},{0xC0012700,0x1FFFFFFD,3}}){
        const auto candidate=frame(bad);assert(candidate);
        assert(!captureNativeDeferredPrefixShaders(*candidate,reader,captured) && captured.bytes==saved && reads==1);++rejects;
    }
    std::vector<unsigned> excessive;
    for(unsigned i=0;i<9;++i)excessive.insert(excessive.end(),{0xC0012700,0x40001,3});
    const auto tooMany=frame(excessive);assert(tooMany && !captureNativeDeferredPrefixShaders(*tooMany,reader,captured) && reads==1);
    const auto tooLarge=frame({0xC0012700,0x40001,1026});assert(tooLarge && !captureNativeDeferredPrefixShaders(*tooLarge,reader,captured) && reads==1);
    assert(rejects==8 && captured.bytes==saved);
    std::puts("DEFERRED_PREFIX_SHADERS: 8metadata/image-alias refusals,4atomic reader failures,8program/4KiB caps,owned inline/external sources,no GPU or rendering proof PASS");
}
