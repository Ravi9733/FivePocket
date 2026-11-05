#include "native_color8_clear_target.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
#include "image_sample_probe.h"
#ifndef COLOR8_SAMPLES_READER_ONLY
int main(int argc,char** argv) {
    assert(argc==3 || argc==7);ProbeDevice d;assert(d.inspect() && d.create({}));
    unsigned clears=0;std::uint64_t samplesRead=0;
    for(unsigned samples:{1u,2u,4u})for(unsigned format:{0u,1u}) {
        Gta5Color8ClearPlan p{0x1000,128,128,160,0,16*samples,samples,format,{}, {}};
        auto target=NativeColor8ClearTarget::create(d,p);assert(target && !target->readUniformResolved());
        assert(target->diagnosticMutationSerial()==0);
        assert(target->clear({0,0,0,0}) && target->diagnosticMutationSerial()==1);
        SampleReader reader(d,*target,argv[samples==1?1:2]);
        for(unsigned level=0;level<256;++level) {
            const auto linear=[&](unsigned b){return format?xenosPwlGammaByteToLinear(b):float(b)/255.f;};
            const std::array<std::uint8_t,4> expected{std::uint8_t(level),std::uint8_t(255-level),std::uint8_t(level^0xA5),std::uint8_t(level*37)};
            const auto color=packGta5Color8Clear({linear(expected[0]),linear(expected[1]),linear(expected[2]),float(expected[3])/255.f},format);
            const auto priorMutation=target->diagnosticMutationSerial();
            assert(color && *color==expected && target->clear(*color));reader.check(expected);
            assert(target->diagnosticMutationSerial()==priorMutation+1);
            const auto rgba=target->readUniformResolved();assert(rgba && rgba->size()==128*128*4);
            for(unsigned i=0;i<rgba->size();++i)assert((*rgba)[i]==expected[i%4]);
            ++clears;samplesRead+=128*128*samples;
        }
        std::printf("COLOR8_CLEAR_GPU_CASE_PASS samples=%u format=%u levels=256 individual_samples=1\n",samples,format);
    }
    Gta5Color8ClearPlan invalid{0x1000,128,128,160,0,32,2,1,{}, {}};
    for(unsigned bad:{0u,3u,8u}){invalid.samples=bad;assert(!NativeColor8ClearTarget::create(d,invalid));}
    invalid.samples=2;invalid.tileCount=31;assert(!NativeColor8ClearTarget::create(d,invalid));
    invalid.tileCount=32;invalid.baseTile=2047;assert(!NativeColor8ClearTarget::create(d,invalid));
    std::printf("COLOR8_CLEAR_GPU_PASS clears=%u individually_read_samples=%llu game_execution=0\n",clears,static_cast<unsigned long long>(samplesRead));
    if(argc==7) {
        unsigned coupledClears=0;
        for(unsigned samples:{1u,2u,4u}) {
            Gta5Color8ClearPlan p{0x1000,128,128,160,0,16*samples,samples,1,{0,0,0,255}, {}};
            p.depth=Gta5Depth24ClearPlan{0x2000,128,128,160,16*samples,16*samples,samples,1,*planExactXenosFloat24Clear(0,255)};
            auto target=NativeColor8ClearTarget::create(d,p);assert(target && target->clear(p.color));
            SampleReader color(d,*target,argv[samples==1?1:2]);
            SampleReader depth(d,*target,argv[samples==1?3:4],VK_IMAGE_ASPECT_DEPTH_BIT);
            SampleReader stencil(d,*target,argv[samples==1?5:6],VK_IMAGE_ASPECT_STENCIL_BIT);
            for(unsigned i=0;i<256;++i) {
                const unsigned encodings[]{0,1,0xFFFFF,0x100000,0x800001,0xE00000,0xEFFFFF,0xF00000};
                const float f=*decodeXenosFloat24(encodings[i%8]);
                const auto value=*planExactXenosFloat24Clear(f,i);
                const std::array<std::uint8_t,4> rgba{std::uint8_t(i),std::uint8_t(255-i),std::uint8_t(i^0xA5),255};
                assert(target->clear(rgba,value));color.check(rgba);
                unsigned bits;std::memcpy(&bits,&f,4);depth.checkWord(bits);stencil.checkWord(i);++coupledClears;
            }
            // Malformed requested depth must invalidate, never silently leave
            // the old depth and return a successful color-only clear.
            auto bad=p.depth->value;bad.encoded=1;
            const auto beforeFailure=target->diagnosticMutationSerial();
            assert(!target->clear(p.color,bad) && !target->readUniformResolved());
            assert(target->diagnosticMutationSerial()!=beforeFailure);
            assert(target->clear(p.color,p.depth->value));depth.checkWord(0);stencil.checkWord(255);
            std::printf("COUPLED_FLOAT24_CLEAR_CASE_PASS samples=%u levels=256 color_depth_stencil_individually_read=1\n",samples);
        }
        std::printf("COUPLED_FLOAT24_CLEAR_GPU_PASS clears=%u game_execution=0\n",coupledClears);
    }
}
#endif
