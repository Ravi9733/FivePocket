#pragma once
#include "indexed_lod_fixture.h"
#include "native_raw10_color_resolve.h"
#include "native_serialized_resource_batch.h"
#include "native_raw10_texture_capture.h"

namespace recomp::gpu::test {
inline Gta5TileResolveCapture readRaw10ResolveConnection(const std::string& dir) {
    const auto read=[&](const std::string& name,unsigned bytes) {
        auto b=IndexedLodFixture::read(dir+"/"+name);assert(b.size()==bytes);return b;
    };
    const auto word=[](const auto& b,unsigned at){return IndexedLodFixture::be(b,at);};
    const auto call=read("resolve-call.bin",48);Gta5TileResolveCapture c;
    for(unsigned i=0;i<9;++i)c.arguments[i]=word(call,4*i);
    c.stack=word(call,36);c.f1=(std::uint64_t{word(call,40)}<<32)|word(call,44);
    const auto payload=[&](const std::string& key,unsigned n) {
        return read("native-resolve-failure-"+key+".bin",n);
    };
    c.state=payload("state",13860);c.destinationHeader=payload("destination-header",52);c.surfaces.emplace();
    for(unsigned i=0;i<5;++i) {
        auto& s=c.surfaces->surfaces[i];s.object=word(*c.state,12816+4*i);
        if(s.object)s.header=payload("surface-"+std::to_string(i),40);
    }
    c.pointed.rectangle=payload("rectangle",16);c.pointed.stack92=payload("stack92",4);
    c.pointed.stack100=payload("stack100",4);c.pointed.color=payload("clear-color",16);
    return c;
}
// Connect actual original GPU draw outputs in an AUTHORED initial EDRAM owner
// to the new actual resolve call. This is not a live destination-owner receipt.
inline void checkRaw10ResolveConnection(const Gta5TileResolveCapture& c,
        const NativeEdram32Bands& owner,const std::vector<unsigned>& color) {
    const auto p=planNativeRaw10ColorResolve(c);assert(p && p->destination.exponentBias==3);
    assert(color.size()==1280*720);const auto serial=owner.nextSerial();
    std::vector<std::uint8_t> before(p->Bytes,0xA5),expected=before,destination=before;
    std::vector<bool> touched(p->Bytes/4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        // Independent bank/pipe tiling, NOT production colorCopyOffset.
        const unsigned outer=((y/32)*40+x/32)*64,inner=(y/2%8)*8+x%8;
        const unsigned v=(outer+inner)*4,bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
        const unsigned at=(v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
        assert(at%4==0 && at+4<=p->Bytes && !touched[at/4]);touched[at/4]=true;
        const unsigned raw=color[y*1280+x];
        const unsigned swapped=(raw/1073741824u)*1073741824u+(raw%1024)*1048576+
            ((raw/1024)%1024)*1024+(raw/1048576)%1024;
        for(unsigned b=0;b<4;++b)expected[at+b]=swapped>>(24-b*8);
    }
    unsigned stores=0;const char* why=nullptr;
    const auto publish=[&](const auto& result) {
        ++stores;const NativeSerializedResourceUpdate u{0xA1000000,{destination.data(),destination.size()},&before,&result};
        return publishNativeSerializedResourceBatch(&u,1);
    };
    assert(publishNativeRaw10ColorResolve(c,owner,before,[&]{return sameNativeRaw10ColorInputs(c,c);},publish,&why));
    assert(stores==1 && destination==expected && owner.nextSerial()==serial);
    auto submitted=p->destination.fetch;submitted[0]=0x8A004802;submitted[3]=0x00A86C14;
    submitted[4]=3;submitted[5]=0x210;
    const auto physical=p->destination.baseAddress>=0xA0000000u?
        p->destination.baseAddress-0xA0000000u:p->destination.baseAddress;
    submitted[1]=physical|(submitted[1]&0xFFFu);
    // Original resolve producer -> unchanged published bytes -> complete
    // diagnostic capture. This does not execute or validate raw10 sampling.
    const auto captured=captureNativeRaw10Base(5,submitted,[&](unsigned at,unsigned bytes)
            ->std::optional<std::vector<std::uint8_t>> {
        if(at!=physical || bytes!=destination.size())return std::nullopt;
        return destination;
    });
    assert(captured && captured->source==expected && captured->fetch==submitted);
    assert(std::count(touched.begin(),touched.end(),true)==1280*720);
    auto unknown=owner;assert(unknown.invalidate(1439,1));
    assert(!publishNativeRaw10ColorResolve(c,unknown,before,[]{return true;},publish));assert(stores==1);
    assert(!publishNativeRaw10ColorResolve(c,owner,before,[]{return false;},publish));assert(stores==1);
    destination=before;destination.back()^=1;const auto stale=destination;
    assert(!publishNativeRaw10ColorResolve(c,owner,before,[]{return true;},publish));assert(stores==2 && destination==stale);
    std::puts("INDEXED602_RAW10_CONNECTION_PASS actual_resolve=1 sampling_bias=3 raw_copy_bias=0 original_gpu_producer=1 pixels=921600 padding_preserved=81920 metadata_preserved=1 source_unchanged=1 unknown_stale_refused=1 authored_initial_owner=1 live_destination_unproven=1");
    std::puts("RAW10_RESOLVE_CAPTURE_CONNECTION_PASS complete_bytes=3768320 producer_bytes_unchanged=1 sampling_admission=0");
}
}
