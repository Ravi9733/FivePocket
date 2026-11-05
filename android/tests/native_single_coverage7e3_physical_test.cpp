#include "native_single_coverage7e3_physical.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    constexpr unsigned pixels=1280*720;
    std::vector<std::uint8_t> bytes(pixels*4);
    std::vector<std::uint32_t> depth(pixels,7);
    for(unsigned i=0;i<pixels;++i) {
        const unsigned word=(i%1024)*0x100401u|((i%4)<<30);
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=word>>(8*c);
    }
    const auto seed=[&]{NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        assert(owner.recordCompletedDepthWords(1500,80,80,16,std::vector<unsigned>(1280,0x12345678),3));
        return owner;};
    auto owner=seed();const auto original=owner.readColorWords(720,1280,1280,720);
    const auto unrelated=owner.readDepthWords(1500,80,80,16);const auto serial=owner.nextSerial();
    {
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeSingleCoverage7e3Physical(mutation,[&](const auto& rgba,const auto& ds) {
            assert(!owner.knownTiles() && ds==depth);
            return std::optional{NativeSingleCoverage7e3Readback{std::vector<float>(rgba.size(),0),rgba,ds}};
        }));
    }
    assert(owner.readColorWords(720,1280,1280,720)==original && owner.nextSerial()==serial+1);
    assert(owner.readDepthWords(0,1280,1280,720)==depth && owner.readDepthWords(1500,80,80,16)==unrelated);
    // Actual covered pixels may change; uncovered pixels and all depth stay.
    {
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeSingleCoverage7e3Physical(mutation,[](const auto& rgba,const auto& ds) {
            NativeSingleCoverage7e3Readback r{std::vector<float>(rgba.size(),0),rgba,ds};
            for(unsigned c=0;c<4;++c){r.coverageRgba[4+c]=1;r.rgba[4+c]=.5f;}
            return std::optional{r};
        }));
    }
    const auto result=owner.readColorWords(720,1280,1280,720);assert(result && result->at(1)!=original->at(1));
    for(unsigned i=0;i<pixels;++i)if(i!=1)assert(result->at(i)==original->at(i));
    assert(owner.readDepthWords(0,1280,1280,720)==depth && owner.readDepthWords(1500,80,80,16)==unrelated);
    for(unsigned kind=0;kind<17;++kind) {
        owner=seed();unsigned calls=0;const char* why=nullptr;
        if(kind==0)assert(owner.invalidate(720,1));
        if(kind==1)assert(owner.invalidate(0,1));
        if(kind==15 || kind==16) {
            auto d=depth;d[1]=kind==15?0x100u|7u:6;
            assert(owner.recordCompletedDepthWords(0,1280,1280,720,d,owner.nextSerial()));
        }
        {
            NativeEdram32Mutation mutation(owner);
            assert(!executeNativeSingleCoverage7e3Physical(mutation,[&](const auto& rgba,const auto& ds)
                    ->std::optional<NativeSingleCoverage7e3Readback> {
                ++calls;NativeSingleCoverage7e3Readback r{std::vector<float>(rgba.size(),0),rgba,ds};
                if(kind==2)return std::nullopt;
                if(kind==3)r.rgba.pop_back();
                if(kind==4)r.coverageRgba.pop_back();
                if(kind==5)r.depthStencil.pop_back();
                if(kind==6)r.depthStencil[0]^=0x100;
                if(kind==7)r.depthStencil[0]^=1;
                if(kind==8)r.coverageRgba[0]=2;
                if(kind==9)r.coverageRgba[0]=.5f;
                if(kind==10)r.coverageRgba[0]=std::numeric_limits<float>::quiet_NaN();
                if(kind==11)r.coverageRgba[0]=std::numeric_limits<float>::infinity();
                if(kind==12)r.coverageRgba[1]=1;
                if(kind==13)r.rgba[0]=std::numeric_limits<float>::infinity();
                if(kind==14)r.rgba[0]=.5f; // Count0 must not change storage.
                if(kind>=15)for(unsigned c=0;c<4;++c)r.coverageRgba[4+c]=1;
                return r;
            },&why));
        }
        assert(why && !owner.knownTiles() && calls==(kind<2?0u:1u));
    }
    std::vector<float> counts(pixels*4,0),source(pixels*4,0);
    for(unsigned c=0;c<4;++c)counts[4+c]=1;
    source[4]=-.5f;source[5]=100;source[6]=.5f;source[7]=.5f;
    assert(validateNativeSingleCoverage7e3Source(counts,source)); // RGB is float, not fixed-point.
    for(float alpha:{-1.f,2.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        source[7]=alpha;assert(!validateNativeSingleCoverage7e3Source(counts,source));
    }
    source[7]=.5f;source[0]=1;assert(!validateNativeSingleCoverage7e3Source(counts,source));source[0]=0;
    counts[4]=2;assert(!validateNativeSingleCoverage7e3Source(counts,source));counts[4]=1;
    source.pop_back();assert(!validateNativeSingleCoverage7e3Source(counts,source));
    std::puts("SINGLE_COVERAGE_7E3_SOURCE_HOST_PASS alpha_preblend_domain=1 negative_cases=7 float_rgb_unclamped=1");
    std::puts("SINGLE_COVERAGE_7E3_PHYSICAL_HOST_PASS roundtrip_codes=1024 negatives=17 depth_unrelated_preserved=1 failure_invalidates=1 gpu=0");
}
