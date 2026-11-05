#include "native_color_chain_inputs.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

using namespace recomp::gpu;
namespace {
bool same(const PackedNativeColorChainInputs& a,const PackedNativeColorChainInputs& b){return a.depth==b.depth && a.pairs==b.pairs;}
struct Fixture {
    std::array<std::vector<std::uint8_t>,7> raw;
    NativeColorChainBindings input;
    Fixture(){
        for(unsigned i=0;i<3;++i){
            raw[i].resize(i?144:84);
            for(unsigned n=0;n<raw[i].size();++n)raw[i][n]=static_cast<std::uint8_t>(n*19+i*31);
            auto& vertex=i?input.pairs[i-1].vertices:input.depth;
            vertex.slot=i?95:0;const auto address=0x1000+i*0x1000;
            vertex.descriptor={{{address|3u,0xFFFFFFFF},{0x10000002u|unsigned(raw[i].size()),0xFFFFFFFF}}};
            vertex.source={address,raw[i].data(),raw[i].size()};
        }
        for(unsigned i=0;i<3;++i){
            const unsigned width=i?640:1280,height=i?360:720,pitch=i?768:1280;
            raw[i+3].resize(pitch*height,0xE7);
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
                raw[i+3][y*pitch+x]=static_cast<std::uint8_t>((x*7)^(y*13)^(i*41));
            const auto address=0x1000000+i*0x100000;
            const std::array<std::uint32_t,6> words{(i?0x06024802u:0x0A024802u),address|2u,
                (i?0x002CE27Fu:0x0059E4FFu),0x00281400,3,0x210};
            NativeColorTextureBinding texture;texture.slot=i;
            for(unsigned n=0;n<6;++n)texture.descriptor[n]={words[n],0xFFFFFFFF};
            texture.source={address,raw[i+3].data(),raw[i+3].size()};input.pairs[0].textures.push_back(texture);
        }
        raw[6].resize(4096);
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){std::uint32_t at=0;assert(tiled32TexelOffset(x,y,at));
            raw[6][at]=255;raw[6][at+1]=static_cast<std::uint8_t>(x);raw[6][at+2]=static_cast<std::uint8_t>(y);
            raw[6][at+3]=static_cast<std::uint8_t>(x^y);}
        NativeColorTextureBinding texture;texture.slot=2;
        const std::array<std::uint32_t,6> words{0x80400002,0x02000086,0x3E01F,0xA80C14,3,0x210};
        for(unsigned n=0;n<6;++n)texture.descriptor[n]={words[n],0xFFFFFFFF};
        texture.source={0x02000000,raw[6].data(),raw[6].size()};input.pairs[1].textures.push_back(texture);
        const auto constants=[](std::initializer_list<unsigned> indices){
            std::vector<NativeColorConstant> result;
            for(auto index:indices){NativeColorConstant c;c.index=index;
                for(unsigned i=0;i<4;++i)c.words[i]={0xFEDC0000u^(index<<5)^i,0xFFFFFFFF};result.push_back(c);}
            return result;
        };
        input.pairs[0].vertexConstants=constants({36,11,10,9,8}); // Deliberately not sorted.
        input.pairs[0].pixelConstants=constants({255,35,34,33,32});
        input.pairs[1].vertexConstants=constants({11,10,9,8});input.pairs[1].pixelConstants=constants({62,16});
    }
};
}
int main(){
    Fixture f;PackedNativeColorChainInputs packed;
    assert(packNativeColorChainInputs(f.input,packed));
    assert(packed.depth==f.raw[0] && packed.pairs[0].size()==1382720 && packed.pairs[1].size()==4352);
    const auto equalAt=[](const auto& packed,std::size_t at,const auto& raw){
        return std::equal(raw.begin(),raw.end(),packed.begin()+at);
    };
    assert(equalAt(packed.pairs[0],1382496,f.raw[1]) && equalAt(packed.pairs[1],4144,f.raw[2]));
    std::size_t at=0;
    for(unsigned i=0;i<3;++i){const unsigned width=i?640:1280,height=i?360:720;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
            assert(packed.pairs[0][at++]==static_cast<std::uint8_t>((x*7)^(y*13)^(i*41)));}
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){const auto n=(y*32+x)*4;
        const auto& p=packed.pairs[1];assert(p[n]==x && p[n+1]==y && p[n+2]==(x^y) && p[n+3]==255);}
    for(unsigned n=0;n<16;++n)assert(packed.pairs[0][1382400+n]==0 && packed.pairs[1][4096+n]==0);
    const auto checkConstants=[&](unsigned pair,unsigned offset,std::initializer_list<unsigned> indices){
        for(auto index:indices)for(unsigned lane=0;lane<4;++lane){
            const auto word=0xFEDC0000u^(index<<5)^lane;
            for(unsigned byte=0;byte<4;++byte)assert(packed.pairs[pair][offset++]==static_cast<std::uint8_t>(word>>(24-byte*8)));
        }
    };
    checkConstants(0,1382416,{32,33,34,35,255});checkConstants(0,1382640,{8,9,10,11,36});
    checkConstants(1,4112,{16,62});checkConstants(1,4288,{8,9,10,11});
    const auto expected=packed;
    auto reordered=f.input;std::reverse(reordered.pairs[0].textures.begin(),reordered.pairs[0].textures.end());
    for(auto& pair:reordered.pairs){std::reverse(pair.vertexConstants.begin(),pair.vertexConstants.end());
        std::reverse(pair.pixelConstants.begin(),pair.pixelConstants.end());}
    assert(packNativeColorChainInputs(reordered,packed) && same(packed,expected));

    unsigned failures=0;
    const auto reject=[&](const NativeColorChainBindings& input){
        PackedNativeColorChainInputs sentinel{{1,2,3},{{{4,5},{6,7,8}}}};const auto before=sentinel;
        assert(!packNativeColorChainInputs(input,sentinel) && same(sentinel,before));++failures;
    };
    // Descriptor literals are represented without claiming a guest-memory
    // source. Every original vertex/constant byte must survive unchanged.
    auto literal=f.input;auto& zero=literal.pairs[1].textures[0];
    const std::array<std::uint32_t,6> zeroWords{0x02024802,2,0,0x00281249,3,0x210};
    for(unsigned i=0;i<6;++i)zero.descriptor[i]={zeroWords[i],0xFFFFFFFF};
    zero.source={};zero.sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
    PackedNativeColorChainInputs zeroPacked;
    assert(packNativeColorChainInputs(literal,zeroPacked));
    assert(zeroPacked.depth==expected.depth && zeroPacked.pairs[0]==expected.pairs[0]);
    assert(std::all_of(zeroPacked.pairs[1].begin(),zeroPacked.pairs[1].begin()+4096,[](auto b){return b==0;}));
    assert(std::equal(zeroPacked.pairs[1].begin()+4096,zeroPacked.pairs[1].end(),expected.pairs[1].begin()+4096));
    for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit)for(unsigned mode=0;mode<2;++mode){
        auto bad=literal;auto& descriptor=bad.pairs[1].textures[0].descriptor[word];
        if(mode)descriptor.bits^=1u<<bit;else descriptor.knownMask^=1u<<bit;reject(bad);
    }
    for(unsigned mode=0;mode<7;++mode){auto bad=literal;auto& texture=bad.pairs[1].textures[0];
        if(mode==0)texture.source.physicalAddress=0x2000000;
        if(mode==1)texture.source.bytes=f.raw[6].data();
        if(mode==2)texture.source.size=4096;
        if(mode==3)texture.sourceKind=NativeColorTextureBinding::SourceKind::GuestMemory;
        if(mode==4)texture.sourceKind=static_cast<NativeColorTextureBinding::SourceKind>(99);
        if(mode==5)texture.slot=0;
        if(mode==6)bad.pairs[0].textures[0].sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
        reject(bad);
    }
    auto mislabelled=f.input;mislabelled.pairs[1].textures[0].sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
    reject(mislabelled);
    std::puts("NATIVE_COLOR_CONSTANT_ZERO_INPUTS: 392 rejections; literal-only RGBA and preserved vertices/constants PASS");
    const auto vertex=[](NativeColorChainBindings& input,unsigned i)->NativeColorVertexBinding&{
        return i?input.pairs[i-1].vertices:input.depth;
    };
    for(unsigned v=0;v<3;++v){
        auto bad=f.input;vertex(bad,v).slot^=1;reject(bad);
        for(unsigned w=0;w<2;++w)for(unsigned bit=0;bit<32;++bit){
            bad=f.input;vertex(bad,v).descriptor[w].knownMask^=1u<<bit;reject(bad);}
        for(unsigned w=0;w<2;++w){bad=f.input;vertex(bad,v).descriptor[w].bits^=1;reject(bad);}
        for(unsigned mutation=0;mutation<6;++mutation){
            bad=f.input;auto& source=vertex(bad,v).source;
            if(mutation==0)source.bytes=nullptr;
            if(mutation==1)--source.size;
            if(mutation==2)++source.size;
            if(mutation==3)source.physicalAddress+=4;
            if(mutation==4)source.bytes=reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX-15);
            if(mutation==5){source.physicalAddress=0x1FFFFFFC;vertex(bad,v).descriptor[0].bits=0x1FFFFFFF;}
            reject(bad);
        }
    }
    for(unsigned pair=0;pair<2;++pair){
        auto bad=f.input;bad.pairs[pair].textures.push_back(bad.pairs[pair].textures[0]);reject(bad);
        bad=f.input;bad.pairs[pair].textures.clear();reject(bad);
        for(unsigned t=0;t<f.input.pairs[pair].textures.size();++t){
            bad=f.input;bad.pairs[pair].textures[t].slot=95;reject(bad);
            for(unsigned w=0;w<6;++w)for(unsigned bit=0;bit<32;++bit){
                bad=f.input;bad.pairs[pair].textures[t].descriptor[w].knownMask^=1u<<bit;reject(bad);}
            bad=f.input;bad.pairs[pair].textures[t].descriptor[0].bits^=1;reject(bad);
            for(unsigned mutation=0;mutation<5;++mutation){
                bad=f.input;auto& source=bad.pairs[pair].textures[t].source;
                if(mutation==0)source.bytes=nullptr;
                if(mutation==1)source.size-=4;
                if(mutation==2)source.size+=4;
                if(mutation==3)source.physicalAddress+=4096;
                if(mutation==4)source.bytes=reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX-15);
                reject(bad);
            }
        }
        for(unsigned stage=0;stage<2;++stage){
            const auto constants=[&](auto& input)->auto&{return stage?input.pairs[pair].pixelConstants:input.pairs[pair].vertexConstants;};
            bad=f.input;constants(bad).pop_back();reject(bad);
            bad=f.input;constants(bad).push_back(constants(bad)[0]);reject(bad);
            bad=f.input;constants(bad)[0].index=constants(bad)[1].index;reject(bad);
            for(unsigned c=0;c<constants(f.input).size();++c){
                bad=f.input;constants(bad)[c].index=999;reject(bad);
                for(unsigned w=0;w<4;++w){bad=f.input;constants(bad)[c].words[w].knownMask=0x7FFFFFFF;reject(bad);}
            }
        }
    }
    // Packing owns all output bytes, and can safely replace an output used as
    // an input snapshot: every read is finished before the final move.
    auto aliased=f.input;aliased.depth.source.bytes=packed.depth.data();
    assert(packNativeColorChainInputs(aliased,packed) && same(packed,expected));
    for(auto& bytes:f.raw)std::fill(bytes.begin(),bytes.end(),0);
    assert(same(packed,expected));
    std::printf("NATIVE_COLOR_CHAIN_INPUTS_V1 packed_bytes=1387156 failure_controls=%u output_owned=1 guest_writeback=0\n",failures);
}
