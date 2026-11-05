#include "xenos_texture_2d.h"
#include "xenos_color_copy.h"
#include "native_offscreen_resolve.h"
#include <cassert>
#include <cstdio>

using namespace recomp::gpu;
static XenosTextureDescription texture(unsigned w,unsigned h,unsigned pitch,unsigned format,bool tiled,unsigned endian=0) {
    std::array<std::uint32_t,6> f{2u|((pitch/32)<<22)|(tiled?0x80000000u:0),
        0x10000u|format|(endian<<6),(w-1)|((h-1)<<13),0x688,0,0x200};
    return *XenosTextureDescription::decode(f);
}
int main() {
    // Compare the generalized layout with the independently existing RGBA8
    // copy layout across tile boundaries and a non-tile-aligned image height.
    const auto d=texture(1280,720,1280,6,true);
    const auto p=planXenosTexture2D(d);
    assert(p && p->sourceBytes==1280*736*4 && p->linearBytes==1280*720*4);
    std::vector<std::uint8_t> tiled(p->sourceBytes,0);
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) {
        std::uint32_t at=0; assert(colorCopyOffset(1280,720,x,y,at));
        for (unsigned c=0;c<4;++c) tiled[at+c]=static_cast<std::uint8_t>(x+3*y+17*c);
    }
    const auto linear=linearizeXenosTexture2D(d,tiled); assert(linear);
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) for (unsigned c=0;c<4;++c)
        assert((*linear)[(y*1280+x)*4+c]==static_cast<std::uint8_t>(x+3*y+17*c));
    assert(!linearizeXenosTexture2D(d,tiled.data(),p->sourceBytes-1));

    // Linear pitch, all four byte-order modes; no swizzle baked into raw data.
    for (unsigned endian=0;endian<4;++endian) {
        auto t=texture(7,2,32,2,false,endian); const auto layout=planXenosTexture2D(t);
        assert(layout && layout->sourceBytes==40 && layout->linearBytes==14);
        std::vector<std::uint8_t> bytes(40); for(unsigned i=0;i<40;++i)bytes[i]=static_cast<std::uint8_t>(i);
        const auto result=linearizeXenosTexture2D(t,bytes); assert(result);
        constexpr unsigned mask[]{0,1,3,2};
        for (unsigned y=0;y<2;++y) for(unsigned x=0;x<7;++x) assert((*result)[y*7+x]==((y*32+x)^mask[endian]));
    }
    // Known R8 bank addresses (not contiguous 32x32 tiles).
    auto r8=texture(32,32,32,2,true); auto rp=planXenosTexture2D(r8);
    assert(rp && rp->sourceBytes==0xA00);
    std::vector<std::uint8_t> raw(rp->sourceBytes,0);
    raw[0]=11; raw[64]=12; raw[16]=13; raw[2048]=14;
    const auto red=linearizeXenosTexture2D(r8,raw); assert(red);
    assert((*red)[0]==11 && (*red)[8]==12 && (*red)[32]==13 && (*red)[16*32]==14);
    // BC1 authored block: red/green endpoints and all four palette selectors.
    // Check guest 8-in-16 byte swapping before little-endian BC1 decoding.
    const std::array<std::uint8_t,8> block{0,0xF8,0xE0,7,0xE4,0xE4,0xE4,0xE4};
    for(unsigned endian=0;endian<4;++endian) {
        auto bc=texture(4,4,32,18,false,endian);auto bp=planXenosTexture2D(bc);
        assert(bp && bp->sourceBytes==8 && bp->linearBytes==64 && bp->bytesPerTexel==4);
        std::vector<std::uint8_t> encoded(8);constexpr unsigned mask[]{0,1,3,2};
        for(unsigned i=0;i<8;++i)encoded[i^mask[endian]]=block[i];
        auto decoded=linearizeXenosTexture2D(bc,encoded);assert(decoded);
        const std::array<std::array<std::uint8_t,4>,4> expected{{{255,0,0,255},{0,255,0,255},{170,85,0,255},{85,170,0,255}}};
        for(unsigned i=0;i<16;++i)for(unsigned c=0;c<4;++c)assert((*decoded)[i*4+c]==expected[i%4][c]);
        assert(!linearizeXenosTexture2D(bc,encoded.data(),7));
    }
    auto alpha=texture(3,2,32,18,false); // Partial edge block, endpoint0<=endpoint1.
    std::vector<std::uint8_t> transparent{0,0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    auto alphaPixels=linearizeXenosTexture2D(alpha,transparent);assert(alphaPixels && alphaPixels->size()==24);
    for(auto c:*alphaPixels)assert(c==0);
    auto bt=texture(256,256,256,18,true);auto bl=planXenosTexture2D(bt);assert(bl && bl->sourceBytes==32768);
    std::vector<std::uint8_t> tiledBlocks(bl->sourceBytes);
    const std::array<unsigned,4> endpoints{0xF800,0x07E0,0x001F,0xFFFF};
    for(unsigned i=0;i<4;++i){tiledBlocks[i*8192]=endpoints[i]&255;tiledBlocks[i*8192+1]=endpoints[i]>>8;}
    auto blocks=linearizeXenosTexture2D(bt,tiledBlocks);assert(blocks);
    const std::array<std::array<std::uint8_t,4>,4> corners{{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}}};
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)
        assert((*blocks)[((i/2)*128*256+(i%2)*128)*4+c]==corners[i][c]);
    const auto actualSize=planXenosTexture2D(texture(1024,512,1024,18,true,1));
    assert(actualSize && actualSize->sourceBytes==262144 && actualSize->linearBytes==2097152);
    // BC3 always uses four RGB colors, with an independent alpha palette.
    for(unsigned mode=0;mode<2;++mode) {
        auto bc=texture(4,4,32,20,false,1);std::vector<std::uint8_t> rawBlock(16);
        std::array<std::uint8_t,16> plain{};plain[0]=mode?0:255;plain[1]=mode?255:0;
        std::uint64_t alphaIndices=0;for(unsigned i=0;i<16;++i)alphaIndices|=std::uint64_t{i%8}<<(3*i);
        for(unsigned i=0;i<6;++i)plain[2+i]=alphaIndices>>(8*i);
        plain[10]=plain[11]=255; // black->white, selectors3 must stay opaque RGB.
        for(unsigned i=12;i<16;++i)plain[i]=255;
        for(unsigned i=0;i<16;++i)rawBlock[i^1]=plain[i];
        auto rgba=linearizeXenosTexture2D(bc,rawBlock);assert(rgba);
        const std::array<std::uint8_t,8> expected=mode?std::array<std::uint8_t,8>{0,255,51,102,153,204,0,255}:
            std::array<std::uint8_t,8>{255,0,218,182,145,109,72,36};
        for(unsigned i=0;i<16;++i){for(unsigned c=0;c<3;++c)assert((*rgba)[i*4+c]==170);assert((*rgba)[i*4+3]==expected[i%8]);}
        assert(!linearizeXenosTexture2D(bc,rawBlock.data(),15));
    }
    auto bc3Size=planXenosTexture2D(texture(512,512,512,20,true,1));
    assert(bc3Size && bc3Size->sourceBytes==262144 && bc3Size->linearBytes==1048576);
    // DXT5A has BC4-style endpoint/selector bytes, not BC1 RGB endpoints.
    // Every endpoint pair, all eight selectors, and all endian modes. The
    // independent existing BC3 alpha path must produce the same channel.
    for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b) {
        const unsigned endian=(a+b)%4;constexpr unsigned mask[]{0,1,3,2};
        std::array<std::uint8_t,8> plain{static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(b)};
        std::uint64_t selectors=0;for(unsigned i=0;i<16;++i)selectors|=std::uint64_t{i%8}<<(3*i);
        for(unsigned i=0;i<6;++i)plain[2+i]=selectors>>(8*i);
        std::vector<std::uint8_t> encoded(8),bc3(16);
        for(unsigned i=0;i<8;++i)encoded[i^mask[endian]]=bc3[i^mask[endian]]=plain[i];
        auto dxt5a=texture(4,4,32,59,false,endian);auto plan=planXenosTexture2D(dxt5a);
        assert(plan && plan->sourceBytes==8 && plan->linearBytes==16 && plan->bytesPerTexel==1);
        const auto red=linearizeXenosTexture2D(dxt5a,encoded);
        const auto rgba=linearizeXenosTexture2D(texture(4,4,32,20,false,endian),bc3);
        assert(red && rgba);
        for(unsigned i=0;i<16;++i)assert((*red)[i]==(*rgba)[i*4+3]);
        assert(!linearizeXenosTexture2D(dxt5a,encoded.data(),7));
    }
    const auto dxtTiled=texture(256,256,256,59,true,1);const auto dxtPlan=planXenosTexture2D(dxtTiled);
    assert(dxtPlan && dxtPlan->sourceBytes==32768 && dxtPlan->linearBytes==65536);
    std::vector<std::uint8_t> dxtBytes(dxtPlan->sourceBytes);
    for(unsigned i=0;i<4;++i)dxtBytes[i*8192+1]=11+i*31;
    const auto dxtPixels=linearizeXenosTexture2D(dxtTiled,dxtBytes);assert(dxtPixels);
    for(unsigned i=0;i<4;++i)assert((*dxtPixels)[(i/2)*128*256+(i%2)*128]==11+i*31);
    assert(!storeXenosTexture2D(dxtTiled,*dxtPixels,dxtBytes)); // No invented compressed encoder.
    // DXN = two independent BC4_UNORM blocks, with Xenos RGGG source
    // components BEFORE image-view swizzling. Enumerate every endpoint pair
    // for each channel, reverse channel1 selectors, and cover all endian modes.
    for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b) {
        const unsigned endian=(a+b)%4;constexpr unsigned mask[]{0,1,3,2};
        std::array<std::uint8_t,16> plain{};
        plain[0]=a;plain[1]=b;plain[8]=b;plain[9]=a;
        for(unsigned c=0;c<2;++c) {
            std::uint64_t selectors=0;
            for(unsigned i=0;i<16;++i)selectors|=std::uint64_t{c?7-i%8:i%8}<<(i*3);
            for(unsigned j=0;j<6;++j)plain[c*8+2+j]=selectors>>(8*j);
        }
        std::vector<std::uint8_t> encoded(16),r(8),g(8);
        for(unsigned i=0;i<16;++i)encoded[i^mask[endian]]=plain[i];
        for(unsigned i=0;i<8;++i){r[i^mask[endian]]=plain[i];g[i^mask[endian]]=plain[i+8];}
        const auto dxn=texture(4,4,32,49,false,endian),bc4=texture(4,4,32,59,false,endian);
        const auto plan=planXenosTexture2D(dxn);assert(plan && plan->sourceBytes==16 && plan->linearBytes==64);
        const auto pixels=linearizeXenosTexture2D(dxn,encoded),red=linearizeXenosTexture2D(bc4,r),green=linearizeXenosTexture2D(bc4,g);
        assert(pixels && red && green);
        for(unsigned i=0;i<16;++i) {
            assert((*pixels)[i*4]==(*red)[i]);
            for(unsigned c=1;c<4;++c)assert((*pixels)[i*4+c]==(*green)[i]);
        }
        assert(!linearizeXenosTexture2D(dxn,encoded.data(),15));
    }
    // Four known 32x32-block tile origins; independent R/G values ensure
    // BC1-sized addressing or channel duplication cannot satisfy the test.
    for(unsigned endian=0;endian<4;++endian) {
        constexpr unsigned mask[]{0,1,3,2};
        const auto dxn=texture(256,256,256,49,true,endian);const auto p=planXenosTexture2D(dxn);
        assert(p && p->sourceBytes==65536 && p->linearBytes==262144);
        std::vector<std::uint8_t> bytes(p->sourceBytes);
        for(unsigned i=0;i<4;++i){bytes[(i*16384)^mask[endian]]=11+i*31;bytes[(i*16384+8)^mask[endian]]=200-i*17;}
        const auto rgba=linearizeXenosTexture2D(dxn,bytes);assert(rgba);
        for(unsigned i=0;i<4;++i) {
            const unsigned at=((i/2)*128*256+(i%2)*128)*4;
            assert((*rgba)[at]==11+i*31);
            for(unsigned c=1;c<4;++c)assert((*rgba)[at+c]==200-i*17);
        }
        assert(!storeXenosTexture2D(dxn,*rgba,bytes));
        auto bad=dxn;bad.fetch[0]|=4;assert(!planXenosTexture2D(bad));
        bad=dxn;bad.fetch[3]|=1;assert(!planXenosTexture2D(bad));
        bad=dxn;bad.fetch[3]|=1u<<13;assert(!planXenosTexture2D(bad));
    }
    auto unsupported=d; unsupported.fetch[1]=(unsupported.fetch[1]&~63u)|19; // BC2 remains separate work.
    assert(!planXenosTexture2D(unsupported));
    unsupported=d; unsupported.fetch[4]=1u<<6; assert(!planXenosTexture2D(unsupported));
    unsupported=d; unsupported.fetch[0]|=4; assert(!planXenosTexture2D(unsupported));
    unsigned storageCases=0;
    for(unsigned format:{2u,6u})for(bool tiledMode:{false,true})for(unsigned endian=0;endian<4;++endian)
    for(unsigned width:{1u,7u,32u,33u,127u}) {
        const auto t=texture(width,35,(width+63)&~31u,format,tiledMode,endian);
        const auto layout=planXenosTexture2D(t);assert(layout);
        std::vector<std::uint8_t> pixels(layout->linearBytes),before(layout->sourceBytes,0xA5);
        for(unsigned i=0;i<pixels.size();++i)pixels[i]=(i*37+11)%128; // Distinct from padding sentinel.
        const auto stored=storeXenosTexture2D(t,pixels,before);assert(stored);
        assert(linearizeXenosTexture2D(t,*stored)==std::optional{pixels});
        unsigned changed=0;for(auto b:*stored)changed+=b!=0xA5;
        assert(changed==pixels.size()); // Every texel byte once; no padded bytes changed.
        assert(before==std::vector<std::uint8_t>(layout->sourceBytes,0xA5));
        pixels.pop_back();assert(!storeXenosTexture2D(t,pixels,before));++storageCases;
    }
    // Independent established RGBA copy addressing, not only inverse round trips.
    const auto stored=storeXenosTexture2D(d,*linear,std::vector<std::uint8_t>(p->sourceBytes,0xA5));assert(stored);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
        for(unsigned c=0;c<4;++c)assert((*stored)[at+c]==(*linear)[(y*1280+x)*4+c]);
    }
    assert(!storeXenosTexture2D(bt,std::vector<std::uint8_t>(bl->linearBytes),tiledBlocks)); // No compressed encoder.
    auto shortBefore=std::vector<std::uint8_t>(p->sourceBytes-1);assert(!storeXenosTexture2D(d,*linear,shortBefore));
    unsigned resolveCases=0;
    for(unsigned endian=0;endian<4;++endian)for(unsigned red:{0u,2u})for(unsigned alphaChannel:{3u,5u}) {
        auto t=texture(33,17,64,6,true,endian);
        t.fetch[3]=((red|8u|((2-red)<<6)|(alphaChannel<<9))<<1);
        t=*XenosTextureDescription::decode(t.fetch);
        const auto layout=planXenosTexture2D(t);assert(layout);
        std::vector<std::uint8_t> rgba(33*17*4),before(layout->sourceBytes,0xAA);
        for(unsigned i=0;i<rgba.size();++i)rgba[i]=i%131;
        const NativeOffscreenDrawPlan source{0x1000,33,17,80,4,2};
        const auto result=packNativeOffscreenResolve(source,t,rgba,before);assert(result);
        const auto decoded=linearizeXenosTexture2D(t,*result);assert(decoded);
        for(unsigned i=0;i<rgba.size();i+=4)for(unsigned c=0;c<4;++c)
            assert((*decoded)[i+c]==rgba[i+(red==2 && c<3?2-c:c)]);
        auto bad=source;bad.width=32;assert(!packNativeOffscreenResolve(bad,t,rgba,before));
        t.fetch[0]&=~0x80000000u;assert(!packNativeOffscreenResolve(source,t,rgba,before));++resolveCases;
    }
    std::printf("STORAGE_INVERSE_PASS cases=%u resolve_cases=%u preserved_padding=1\n",storageCases,resolveCases);
    std::puts("DXN_TEXTURE_DECODE_PASS endpoint_pairs=65536 endian_modes=4 tiled_origins=4 source_components=RGGG signed_rejected=1");
    std::puts("XenosTexture2D: tiled 1280x720, R8 banks, padded rows and endian modes passed");
}
