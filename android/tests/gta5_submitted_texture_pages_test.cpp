#include "gta5_submitted_texture_pages.h"
#include <cassert>

int main() {
    using namespace recomp::gpu;
    Gta5SubmittedTextureSnapshot submitted{};
    submitted[7]=std::array<std::uint32_t,6>{2,0x12345014,0,0,0x83,0x23456800};
    const auto original=submitted;
    unsigned calls=0;
    const Gta5ResourceReader reader=[&](std::uint32_t at,std::uint32_t size)
        ->std::optional<std::vector<std::uint8_t>> {
        ++calls;assert(size==4096);
        assert((at>=0x12345000 && at<0x12355000) || (at>=0x23456000 && at<0x23466000));
        if(at==0x12345000)return std::vector<std::uint8_t>(size,0x12);
        if(at==0x23457000)return std::vector<std::uint8_t>(size,0x34);
        if(at==0x23456000)return std::vector<std::uint8_t>(size-1,0); // Short read is unavailable.
        return std::nullopt;
    };
    const auto capture=captureGta5SubmittedTexturePages(submitted,0,reader);
    assert(calls==32 && capture.planes.size()==2 && capture.capturedBytes==8192 && capture.unavailablePages==30);
    assert(capture.planes[0].slot==7 && !capture.planes[0].mip && capture.planes[1].mip);
    assert(capture.planes[0].pages[0] && capture.planes[0].pages[0]->at(0)==0x12);
    assert(!capture.planes[1].pages[0] && capture.planes[1].pages[1]->at(0)==0x34);
    assert(submitted==original);
    calls=0;assert(captureGta5SubmittedTexturePages(submitted,1u<<7,reader).planes.empty());assert(!calls);
    assert(captureGta5SubmittedTexturePages(submitted,0,{}).planes.empty());
    submitted[7]=std::array<std::uint32_t,6>{0,0x12345014,0,0,0,0x23456000};
    assert(captureGta5SubmittedTexturePages(submitted,0,reader).planes.empty());assert(!calls);
    submitted[7]=std::array<std::uint32_t,6>{2,0,0,0,0,0};
    assert(captureGta5SubmittedTexturePages(submitted,0,reader).planes.empty());assert(!calls);
    submitted[7]=std::array<std::uint32_t,6>{2,0xFFFFF000,0,0,0,0};
    const auto edge=captureGta5SubmittedTexturePages(submitted,0,[&](auto at,auto size) {
        ++calls;assert(at==0xFFFFF000 && size==4096);return std::optional(std::vector<std::uint8_t>(size));
    });
    assert(calls==1 && edge.planes.size()==1 && edge.planes[0].addressSpaceTruncated && edge.capturedBytes==4096);
    for(auto& slot:submitted)slot=std::array<std::uint32_t,6>{2,0x10000000,0,0,0,0x20000000};
    calls=0;
    const auto maximum=captureGta5SubmittedTexturePages(submitted,0,[&](auto at,auto size) {
        assert((at>=0x10000000 && at<0x10010000) || (at>=0x20000000 && at<0x20010000));
        ++calls;return std::optional(std::vector<std::uint8_t>(size));
    });
    assert(calls==1024 && maximum.planes.size()==64 && maximum.capturedBytes==Gta5SubmittedTexturePages::MaxBytes);
    assert(maximum.unavailablePages==0);
}
