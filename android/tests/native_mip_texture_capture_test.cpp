#include "native_mip_texture_capture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    // Authored BC3 texture: three unpacked levels before the packed tail.
    const std::array<std::uint32_t,6> fetch{0x81000002,0xA1000014,127|(127u<<13),0,2u<<6,0xA2000A00};
    const auto plan=planXenosUnpackedMipChain(fetch);assert(plan && plan->baseBytes==16384 && plan->mipBytes==32768);
    std::vector<std::array<unsigned,2>> calls;
    const auto reader=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
        calls.push_back({address,size});
        if(address==0xA1000000 && size==16384)return std::vector<std::uint8_t>(size,37);
        if(address==0xA2000000 && size==32768)return std::vector<std::uint8_t>(size,91);
        return std::nullopt;
    };
    const auto result=captureNativeMipTexture(fetch,reader);assert(result && result->fetch==fetch);
    assert(result->base==std::vector<std::uint8_t>(16384,37) && result->mips==std::vector<std::uint8_t>(32768,91));
    assert((calls==std::vector<std::array<unsigned,2>>{{0xA1000000,16384},{0xA2000000,32768}}));
    // Diagnostic aggregate budgets must reject BEFORE allocating/reading either
    // plane, not after collecting an oversized texture. Original fetch retained.
    for(unsigned budget:{0u,16384u,49151u}) {
        calls.clear();assert(!captureNativeMipTexture(fetch,reader,budget));assert(calls.empty());
    }
    calls.clear();assert(captureNativeMipTexture(fetch,reader,49152));assert(calls.size()==2);
    for(unsigned failure=0;failure<6;++failure) {
        unsigned seen=0;
        const auto bad=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
            ++seen;const bool atBase=address==0xA1000000;
            if(atBase==(failure<3)) {
                if(failure%3==0)return std::nullopt;
                return std::vector<std::uint8_t>(size+(failure%3==1?-1:1));
            }
            return std::vector<std::uint8_t>(size);
        };
        assert(!captureNativeMipTexture(fetch,bad) && seen==(failure<3?1u:2u));
    }
    auto single=fetch;single[4]=0;single[5]=0x200;calls.clear();
    const auto baseOnly=captureNativeMipTexture(single,reader);assert(baseOnly && baseOnly->mips.empty() && calls.size()==1);
    // An unsupported sampler must not prevent a diagnostic storage snapshot.
    // The existing renderer-facing name still rejects before reading anything.
    auto biased=fetch;biased[4]|=1u<<12;biased[3]|=3u<<19;
    calls.clear();assert(!captureNativeMipTexture(biased,reader) && calls.empty());
    const auto storage=captureNativeMipTextureStorage(biased,reader);
    assert(storage && storage->fetch==biased && storage->base==result->base && storage->mips==result->mips);
    calls.clear();assert(!captureNativeMipTextureStorage(biased,reader,49151) && calls.empty());
    auto packed=biased;packed[4]=(packed[4]&~0x3FCu)|(3u<<6);
    assert(!captureNativeMipTextureStorage(packed,reader) && calls.empty());
    for(unsigned failure=0;failure<4;++failure) {
        auto bad=fetch;
        if(failure==0)bad[4]=3u<<6; // Packed tail is not independent full planes.
        if(failure==1)bad[5]=0xFFFFFA00; // Mips cross the namespace boundary.
        if(failure==2)bad[3]|=3u<<19; // Unsupported sampler.
        if(failure==3)bad[1]=0; // Missing base owner/address.
        calls.clear();assert(!captureNativeMipTexture(bad,reader) && calls.empty());
    }
    // Actual scene slot2 descriptor (odoefnbf draw6): unsigned tiled DXN,
    // 128x128, levels0..2 before packed tail3, physical namespace unchanged.
    const std::array<std::uint32_t,6> dxn{0x81000002,0x1E940071,0x000FE07F,0x06A81690,0x00000C83,0x0EEA8A10};
    const auto dxnPlan=planXenosUnpackedMipChain(dxn);
    assert(dxnPlan && dxnPlan->baseBytes==16384 && dxnPlan->mipBytes==32768 && dxnPlan->levels.size()==3);
    unsigned dxnReads=0;
    const auto dxnReader=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
        ++dxnReads;
        if(address==0x1E940000 && size==16384)return std::vector<std::uint8_t>(size,19);
        if(address==0x0EEA8000 && size==32768)return std::vector<std::uint8_t>(size,137);
        return std::nullopt;
    };
    assert(!captureNativeMipTextureStorage(dxn,dxnReader,49151) && dxnReads==0);
    const auto dxnCapture=captureNativeMipTextureStorage(dxn,dxnReader,49152);
    assert(dxnCapture && dxnReads==2 && dxnCapture->fetch==dxn);
    const auto levels=linearizeXenosUnpackedMipChain(dxn,dxnCapture->base,dxnCapture->mips);
    assert(levels && levels->size()==3);
    for(unsigned i=0;i<3;++i)assert((*levels)[i].size()==std::size_t{128u>>i}*(128u>>i)*4);
    auto shortMip=dxnCapture->mips;shortMip.pop_back();
    assert(!linearizeXenosUnpackedMipChain(dxn,dxnCapture->base,shortMip));
    auto dxnPacked=dxn;dxnPacked[4]=(dxnPacked[4]&~0x3FCu)|(3u<<6);
    assert(!planXenosUnpackedMipChain(dxnPacked));
    std::puts("DXN_MIP_CAPTURE_PASS actual_descriptor=1 authored_storage=1 levels=3 byte_budget=49152 physical_namespace=1 packed_tail_rejected=1");
    assert(!captureNativeMipTexture(fetch,{}));
    const auto owner=[](unsigned a,unsigned)->std::optional<unsigned>{return a;};
    const auto use=describeNativeMipTextureUse(fetch,owner);assert(use && sameNativeMipTextureStorage(*use,*use));
    auto alternate=fetch;alternate[3]^=1u<<19; // Filter does not change storage lifetime.
    const auto other=describeNativeMipTextureUse(alternate,owner);assert(other && sameNativeMipTextureStorage(*use,*other));
    alternate=fetch;alternate[1]=(alternate[1]&0xFFF)|0x11000000;alternate[5]=(alternate[5]&0xFFF)|0x12000000;
    const auto physical=describeNativeMipTextureUse(alternate,[](unsigned a,unsigned)->std::optional<unsigned>{
        if(a==0x11000000)return 0xA1000000;if(a==0x12000000)return 0xA2000000;return std::nullopt;
    });assert(physical && sameNativeMipTextureStorage(*use,*physical));
    for(unsigned failure=0;failure<8;++failure) {
        auto bad=*use;
        if(failure==0)++bad.baseAddress;if(failure==1)++bad.mipAddress;
        if(failure==2)++bad.baseBytes;if(failure==3)++bad.mipBytes;
        if(failure==4)++bad.description.width;if(failure==5)++bad.description.endian;
        if(failure==6)++bad.description.mipMax;if(failure==7)bad.description.packedMips=!bad.description.packedMips;
        assert(!sameNativeMipTextureStorage(*use,bad));
    }
    assert(!describeNativeMipTextureUse(fetch,{}));
    assert(!describeNativeMipTextureUse(fetch,[](unsigned,unsigned)->std::optional<unsigned>{return std::nullopt;}));
    assert(!describeNativeMipTextureUse(fetch,[](unsigned,unsigned)->std::optional<unsigned>{return 0xFFFFFFF0;}));
    assert(!describeNativeMipTextureUse(single,owner));
    std::puts("NATIVE_MIP_TEXTURE_CAPTURE_PASS two_exact_owned_planes=1 namespace_preserved=1 missing_short_long_rejected=1 no_live_admission=1");
}
