#include "native_inline_shader_load.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned checks=0;
    const auto packet=[](unsigned stage,const std::vector<std::uint8_t>& code){
        const auto words=code.size()/4;assert(words && code.size()==words*4);
        std::vector<std::uint8_t>b;
        for(auto w:{unsigned(0xC0002B00u|((words+1)<<16)),stage,unsigned(words)})
            for(unsigned i=0;i<4;++i)b.push_back(w>>(24-i*8));
        b.insert(b.end(),code.begin(),code.end());return b;
    };
    const std::vector<std::uint8_t> code{0,1,2,3,4,5,6,7,8,9,10,11};
    const auto valid=packet(0,code);auto plan=NativeInlineShaderLoad::parse(valid);assert(plan && plan->bytes()==code && !plan->stage());++checks;
    NativeInlineShaderLoad::Programs raw{{{0xAA,0xBB},{0xCC,0xDD}}};const auto pixel=raw[1];plan->apply(raw);
    assert(raw[0]==code && raw[1]==pixel);++checks;
    auto pixelPlan=NativeInlineShaderLoad::parse(packet(1,code));assert(pixelPlan && pixelPlan->stage()==1);
    pixelPlan->apply(raw);assert(raw[0]==code && raw[1]==code);++checks;
    for(unsigned i=0;i<4;++i)for(unsigned bit=0;bit<8;++bit) {
        auto bad=valid;bad[i]^=1u<<bit;assert(!NativeInlineShaderLoad::parse(bad));++checks;
    }
    for(unsigned i=4;i<12;++i)for(unsigned bit=0;bit<8;++bit) {
        auto bad=valid;bad[i]^=1u<<bit;
        const bool alternate=i==7 && bit==0;assert(NativeInlineShaderLoad::parse(bad).has_value()==alternate);++checks;
    }
    for(unsigned n=0;n<valid.size();++n){assert(!NativeInlineShaderLoad::parse(std::span(valid).first(n)));++checks;}
    auto extra=valid;extra.push_back(0);assert(!NativeInlineShaderLoad::parse(extra));++checks;
    extra.resize(valid.size()+4);assert(!NativeInlineShaderLoad::parse(extra));++checks;
    // Arbitrary raw bytes are retained exactly, NOT implicitly accepted by a
    // translator or executable native-copy consumer.
    auto different=valid;different.back()^=0xFF;auto changed=NativeInlineShaderLoad::parse(different);assert(changed);
    changed->apply(raw);assert(raw[0].back()==(code.back()^0xFF) && raw[1]==code);++checks;
    // Source order: native geometry identities -> copy load -> next geometry.
    const NativeInlineShaderLoad::Programs geometry{{{4,3,2,1},{8,7,6,5}}};raw=geometry;plan->apply(raw);
    assert(raw[0]==code && raw[1]==geometry[1]);raw=geometry;assert(raw==geometry);++checks;
    std::printf("NATIVE_INLINE_SHADER_LOAD_PASS checks=%u exact_raw_bytes=1 other_stage_preserved=1 partial_load_rejected=1 malformed_packet_rejected=1 no_shader_execution=1\n",checks);
}
