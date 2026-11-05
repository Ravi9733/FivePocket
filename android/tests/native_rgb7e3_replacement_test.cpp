#include "native_rgb7e3_replacement.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    constexpr unsigned width=640,height=360,parentWidth=1280,parentHeight=720;
    const NativeOffscreenDrawPlan plan{0x1000,width,height,width,720,184,3};
    std::vector<float> rgba(width*height*4);
    std::vector<unsigned> expected(width*height);
    const auto decode=[](unsigned c) {const auto e=c/128,m=c%128;
        return std::ldexp(float(e?128+m:m),e?int(e)-10:-9);};
    for(unsigned i=0;i<expected.size();++i) {
        const auto r=i%1024,g=(i*3)%1024,b=(i*7)%1024,a=i%4;
        expected[i]=r|(g<<10)|(b<<20)|(a<<30);
        rgba[i*4]=decode(r);rgba[i*4+1]=decode(g);rgba[i*4+2]=decode(b);rgba[i*4+3]=float(a)/3;
    }
    std::vector<std::uint8_t> initial(parentWidth*parentHeight*4);
    for(unsigned i=0;i<initial.size();++i)initial[i]=(i*17+(i>>10))&255;
    NativeEdram32Bands original;
    assert(original.recordCompletedRawColorRows(720,parentWidth,parentWidth,parentHeight,initial,1));
    const auto before=original.readColorWords(720,parentWidth,parentWidth,parentHeight);assert(before);
    auto wanted=*before;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        // Independent8-tiles/row ->16-tiles/row physical alias mapping.
        const unsigned tile=(y/16)*8+x/80;
        const unsigned aliasX=(tile%16)*80+x%80,aliasY=(tile/16)*16+y%16;
        wanted[aliasY*parentWidth+aliasX]=expected[y*width+x];
    }
    auto owner=original;
    {NativeEdram32Mutation mutation(owner);
     assert(recordCompletedNativeRgb7e3Replacement(mutation,plan,rgba));mutation.commit();}
    assert(owner.readColorWords(720,parentWidth,parentWidth,parentHeight)==std::optional{wanted});
    assert(owner.knownTiles()==720 && owner.nextSerial()==3);
    // A fresh unknown owner only learns the actually completed words. The
    // eight padding rows of the final8 tiles must remain unknown.
    NativeEdram32Bands partial;
    {NativeEdram32Mutation mutation(partial);
     assert(recordCompletedNativeRgb7e3Replacement(mutation,plan,rgba));mutation.commit();}
    assert(partial.knownTiles()==176);
    assert(!partial.readColorWords(720,width,width,368));
    assert(partial.readColorWords(720,width,width,352));
    unsigned rejected=0;
    for(unsigned change=0;change<14;++change) {
        auto bad=plan;auto values=rgba;
        if(change==0)bad.format=2;
        if(change==1)bad.format=12;
        if(change==2)bad.tileCount=183;
        if(change==3)bad.tileCount=185;
        if(change==4)bad.pitch=641;
        if(change==5)bad.baseTile=2000;
        if(change==6)bad.width=0;
        if(change==7)bad.height=0;
        if(change==8)bad.object=0;
        if(change==9)values.pop_back();
        if(change==10)values[0]=std::numeric_limits<float>::quiet_NaN();
        if(change==11)values.back()=std::numeric_limits<float>::infinity();
        if(change==12)values[values.size()-2]=-std::numeric_limits<float>::infinity();
        owner=original;
        {NativeEdram32Mutation mutation(owner);
         assert(!recordCompletedNativeRgb7e3Replacement(mutation,bad,values,
             change==13?static_cast<Color7e3Rounding>(255):Color7e3Rounding::RequireNearestEvenAgreement));
         assert(mutation.candidate().readColorWords(720,parentWidth,parentWidth,parentHeight)==before);}
        assert(!owner.knownTiles());++rejected;
    }
    // Saturation is the explicit fixed-format store policy, not an undefined
    // conversion of arbitrary float words. All alpha endpoints remain separate.
    for(unsigned i=0;i<expected.size();++i) {
        rgba[i*4]=-1;rgba[i*4+1]=64;rgba[i*4+2]=.5f;rgba[i*4+3]=i%2?2.f:-1.f;
    }
    owner=original;
    {NativeEdram32Mutation mutation(owner);
     assert(recordCompletedNativeRgb7e3Replacement(mutation,plan,rgba,Color7e3Rounding::XeniaShiftRound));mutation.commit();}
    const auto output=owner.readColorWords(720,width,width,352);assert(output);
    for(unsigned i=0;i<output->size();++i)assert((*output)[i]==((1023u<<10)|(256u<<20)|((i%2?3u:0u)<<30)));
    std::printf("RGB7E3_REPLACEMENT_HOST_PASS pixels=230400 all1024codes=1 padding8rows_preserved=1 unknown_padding_not_invented=1 negatives=%u candidate_unchanged_on_failure=1 gpu=0\n",rejected);
}
