#include "xenos_edram_alias_clear.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::gpu;
int main(){
    for(std::uint32_t n=0;n<0x1000000;++n){
        std::uint32_t bits=0,packed=0;assert(canonicalClearDepthBits(n,bits));
        assert(packCanonicalDepthStencil(bits,n&255,packed));assert(packed==((n<<8)|(n&255)));
    }
    for(auto bits:{0x80000000u,0x7F800000u,0x7FC00000u,1u,0x3F000000u,0x3F800001u,0xBF400001u}){
        std::uint32_t out=17;assert(!packCanonicalDepthStencil(bits,0,out));assert(out==17);
    }
    std::uint32_t word=17;assert(!canonicalClearDepthBits(0x1000000,word));assert(word==17);
    assert(!packCanonicalDepthStencil(0x3F400001,256,word));assert(word==17);
    const AliasView depth{0x0A020280,0x2D0,640,360,true},color{0x14000500,0xC02D0,1280,720,false};
    AliasFootprint d,c;assert(fullAliasFootprint(depth,d));assert(fullAliasFootprint(color,c));
    assert(d.begin==3686400 && d.bytes==3686400 && d.pitchTiles==16 && d.tileRows==45 && d.samples==4);
    assert(c.begin==d.begin && c.bytes==d.bytes && c.samples==1);
    OpaqueAliasClear plan;assert(planOpaqueAliasClear(depth,color,0x3F400001,0,plan));assert(plan.word==0xC0000000);
    std::uint32_t a=0,b=0;assert(aliasSampleOffset(depth,0,0,0,a));assert(aliasSampleOffset(color,0,0,0,b));
    assert(a==d.begin+160 && b==d.begin); // Depth swaps40-sample half-rows.
    std::uint32_t checked=0;
    for(auto v:{depth,color}){
        AliasFootprint f;assert(fullAliasFootprint(v,f));std::vector<std::uint8_t> visits(f.bytes/4);
        for(unsigned y=0;y<v.height;++y)for(unsigned x=0;x<v.width;++x)for(unsigned sample=0;sample<f.samples;++sample){
            std::uint32_t at=0;assert(aliasSampleOffset(v,x,y,sample,at));assert(at>=f.begin && at<f.begin+f.bytes && at%4==0);
            assert(visits[(at-f.begin)/4]==0);visits[(at-f.begin)/4]=1;++checked;
        }
        for(auto seen:visits)assert(seen==1);
        for(auto bad:std::vector<std::vector<unsigned>>{{v.width,0,0},{0,v.height,0},{0,0,f.samples}}){
            word=17;assert(!aliasSampleOffset(v,bad[0],bad[1],bad[2],word));assert(word==17);
        }
    }
    // One-byte guards and untouched EDRAM outside the aliased footprint.
    std::vector<std::uint8_t> memory(AliasEdramBytes+32,0x5A);
    assert(fillOpaqueAliasClear(depth,color,0x3F400001,0,memory.data()+16,AliasEdramBytes));
    for(std::size_t i=0;i<memory.size();++i){
        auto expected=std::uint8_t{0x5A};
        if(i>=16+plan.begin && i<16+plan.begin+plan.bytes)expected=(i-16)%4==3?0xC0:0;
        assert(memory[i]==expected);
    }
    const auto before=memory;
    for(unsigned bytes:{0u,AliasEdramBytes-1,AliasEdramBytes+1})
        assert(!fillOpaqueAliasClear(depth,color,0x3F400001,0,memory.data()+16,bytes));
    assert(!fillOpaqueAliasClear(depth,color,0x3F000000,0,memory.data()+16,AliasEdramBytes));
    assert(!fillOpaqueAliasClear(depth,color,0x3F400001,1,memory.data()+16,AliasEdramBytes));
    assert(memory==before);
    for(AliasView bad:std::vector<AliasView>{{0xA020281,0x2D0,640,360,true},{0xA030280,0x2D0,640,360,true},
        {0xA020280,0x102D0,640,360,true},{0xA020280,2047,640,360,true},
        {0xA020280,0xAD0,640,360,true},{0xA020280,0x2D0,640,359,true}}){
        AliasFootprint f{17,17,17,17,17};assert(!fullAliasFootprint(bad,f));assert(f.begin==17 && f.bytes==17);
    }
    std::printf("EDRAM_ALIAS_CLEAR_V1 canonical_depth_values=16777216 sample_addresses=%u shared_bytes=%u gpu_executed=0\n",checked,plan.bytes);
}
