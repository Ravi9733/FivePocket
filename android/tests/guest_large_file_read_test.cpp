#include "guest_import_registry.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Meta=0x30000000,Name=Meta+32,Text=Meta+64,Out=Meta+128,
    Iosb=Meta+144,Offset=Meta+160,Query=Meta+176,Stack=Meta+4096;
constexpr auto Read="__imp__NtReadFile";
constexpr auto Limit=GuestImportRegistry::MaximumFileReadBytes;
constexpr auto Chunk=GuestImportRegistry::FileReadChunkBytes;
class Source final:public GuestReadOnlyFile {
public:
    mutable unsigned calls=0;
    unsigned failAt=0;
    mutable std::uint64_t nextOffset=0;
    std::uint64_t size() const noexcept override {return Limit+53;}
    bool read(std::uint64_t offset,std::uint8_t* out,std::size_t bytes) const noexcept override {
        assert(bytes && bytes<=Chunk && offset==nextOffset && offset+bytes<=size());
        ++calls;nextOffset=offset+bytes;
        for(std::size_t i=0;i<bytes;++i)out[i]=static_cast<std::uint8_t>((offset+i)%251);
        return calls!=failAt; // A failed host chunk may already contain partial bytes.
    }
};
void put(GuestAddressSpace& s,std::uint32_t a,std::uint64_t v,unsigned size=4) {
    for(unsigned i=0;i<size;++i)s.base()[a+i]=static_cast<std::uint8_t>(v>>((size-i-1)*8));
}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t a) {
    const auto* p=s.base()+a;return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
std::uint64_t invoke(GuestImportRegistry& r,const char* name,std::initializer_list<std::uint32_t> args,std::uint32_t sp=0) {
    GuestImportCall c;std::copy(args.begin(),args.end(),c.arguments.begin());c.stackPointer=sp;
    const auto result=r.dispatch(name,c);
    if(!result.handled())std::fprintf(stderr,"fixture %s: %s\n",name,result.diagnostic.c_str());
    assert(result.handled());return c.returnValue;
}
void run(GuestAddressSpace& s,bool physical) {
    GuestImportRegistry r;assert(r.initialize(s,0x10000000,0x04000000));
    auto source=std::make_shared<Source>();
    assert(r.bindFileMetadataQuery([&](auto path) {
        assert(path=="game:\\large.bin");return GuestFileQueryResult{GuestFileQueryState::Found,{{0,0,0,0,Limit+4096,source->size()},1}};
    }));
    assert(r.bindReadOnlyFiles([&](auto path)->std::shared_ptr<const GuestReadOnlyFile> {assert(path=="game:\\large.bin");return source;}));
    constexpr std::string_view path="game:\\large.bin";
    put(s,Meta,0);put(s,Meta+4,Name);put(s,Meta+8,0x40);
    put(s,Name,path.size(),2);put(s,Name+2,path.size(),2);put(s,Name+4,Text);
    std::memcpy(s.base()+Text,path.data(),path.size());put(s,Stack+0x54,0x60);
    assert(invoke(r,"__imp__NtCreateFile",{Out,0x80100080,Meta,Iosb,0,0x80,1,1},Stack)==0);
    const auto handle=get(s,Out);assert(handle);
    std::uint32_t base=0;
    if(physical) {
        assert(r.bindPhysicalMemoryArena(0xA0000000,0x02000000,0));
        base=static_cast<std::uint32_t>(invoke(r,"__imp__MmAllocatePhysicalMemory",{0,Limit+4096,4}));
    } else {
        put(s,Query,0);put(s,Query+4,Limit+4096);
        assert(invoke(r,"__imp__NtAllocateVirtualMemory",{Query,Query+4,0x3000,4,0})==0);base=get(s,Query);
    }
    assert(base);const auto buffer=base+1; // File byte buffers need no word alignment.
    const auto read=[&](std::uint32_t length,std::uint32_t target=0) {
        GuestImportCall c;c.arguments={handle,0,0,Iosb,Iosb,target?target:buffer,length,Offset};c.returnValue=0xABCDEF;
        const auto result=r.dispatch(Read,c);return std::make_pair(result,c.returnValue);
    };
    const auto position=[&] {
        assert(invoke(r,"__imp__NtQueryInformationFile",{handle,Iosb,Query,8,14})==0);
        assert(get(s,Query)==0);return get(s,Query+4);
    };
    for(auto length:{0U,1U,Chunk,Chunk+1,10439600U,Limit}) {
        std::fill_n(s.base()+base,Limit+4096,0xA5);put(s,Offset,17,8);source->calls=0;source->nextOffset=17;
        const auto result=read(length);assert(result.first.handled() && result.second==0);
        assert(get(s,Iosb)==0 && get(s,Iosb+4)==length && source->calls==(length+Chunk-1)/Chunk);
        for(unsigned i=0;i<length;++i)assert(s.base()[buffer+i]==(17+i)%251);
        assert(s.base()[base]==0xA5 && s.base()[buffer+length]==0xA5);
        if(length)assert(position()==17+length);
    }
    // Later-chunk failure preserves the entire destination, IOSB, return value,
    // file cursor and I/O counters, including chunks successfully staged earlier.
    const auto savedPosition=position();const auto stats=r.fileIoStatistics();
    for(unsigned fail=1;fail<=Limit/Chunk;++fail) {
        std::fill_n(s.base()+base,Limit+4096,0xA5);std::fill_n(s.base()+Iosb,8,0xCC);
        put(s,Offset,0,8);source->calls=0;source->nextOffset=0;source->failAt=fail;
        const auto result=read(Limit);assert(result.first.state==GuestImportDispatchState::Rejected && result.second==0xABCDEF);
        assert(source->calls==fail && std::all_of(s.base()+base,s.base()+base+Limit+4096,[](auto b){return b==0xA5;}));
        assert(get(s,Iosb)==0xCCCCCCCC && get(s,Iosb+4)==0xCCCCCCCC);
        assert(r.fileIoStatistics().reads==stats.reads && r.fileIoStatistics().bytes==stats.bytes);
        assert(position()==savedPosition);
    }
    source->failAt=0;source->calls=0;
    for(const auto& [length,target]:std::array<std::pair<std::uint32_t,std::uint32_t>,5>{{
        {Limit+1,buffer},{16,base+Limit+4096-8},{16,0x7FC80000},{16,0xFFFFFFF8},{16,Iosb}}}) {
        assert(!read(length,target).first.handled());assert(source->calls==0);
    }
    if(!physical) {
        put(s,Query,0);put(s,Query+4,8192);
        assert(invoke(r,"__imp__NtAllocateVirtualMemory",{Query,Query+4,0x2000,4,0})==0);
        const auto reserved=get(s,Query);put(s,Query+4,4096);
        assert(invoke(r,"__imp__NtAllocateVirtualMemory",{Query,Query+4,0x1000,4,0})==0);
        assert(!read(4097,reserved).first.handled() && !source->calls); // Second page is reserved, not committed.
    }
    // Validate the WHOLE requested destination, even where the source would hit EOF.
    put(s,Offset,source->size()-7,8);source->nextOffset=source->size()-7;
    std::fill_n(s.base()+buffer,32,0xA5);auto result=read(32);
    assert(result.first.handled() && !result.second && get(s,Iosb+4)==7 && source->calls==1);
    for(unsigned i=0;i<7;++i)assert(s.base()[buffer+i]==(source->size()-7+i)%251);
    assert(s.base()[buffer+7]==0xA5);put(s,Offset,source->size(),8);source->calls=0;
    result=read(32);assert(result.first.handled() && result.second==0xC0000011 && get(s,Iosb+4)==0 && !source->calls);
    // A read cannot overwrite an actively owned lock embedded in its destination.
    assert(r.bindGuestThreadToken(1));(void)invoke(r,"__imp__RtlInitializeCriticalSection",{base+64});
    (void)invoke(r,"__imp__RtlEnterCriticalSection",{base+64});put(s,Offset,0,8);
    assert(!read(Chunk).first.handled() && !source->calls);(void)invoke(r,"__imp__RtlLeaveCriticalSection",{base+64});
}
}
int main() {
    GuestAddressSpace s;assert(s.reserve());run(s,false);run(s,true);
    std::puts("LARGE_FILE_READ_V1 request_limit=16777216 source_chunk_limit=1048576 storage_cases=2 failed_chunk_positions=32 atomic_outputs=1");
}
