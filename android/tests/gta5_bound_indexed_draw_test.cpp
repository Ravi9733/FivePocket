#include "gta5_bound_indexed_draw.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <type_traits>

int main() {
    using namespace recomp::gpu;
    static_assert(!std::is_convertible_v<Gta5BoundIndexedStorage,Gta5BoundIndexedDraw>);
    constexpr std::uint32_t device=0x10000000,ib=0x10010000,vb=0x10010100;
    std::map<std::uint32_t,std::vector<std::uint8_t>> memory;
    memory[device].resize(13860);memory[ib].resize(32);memory[vb].resize(32);
    memory[0x07001000]={0,1,0,2,0,3,0,4,0,5,0,6};
    memory[0x07002000].resize(256,0xA5);
    const auto put=[&](auto base,unsigned at,std::uint32_t v) {
        auto& bytes=memory.at(base);for(unsigned i=0;i<4;++i)bytes.at(at+i)=v>>(24-8*i);
    };
    put(device,12812,ib);put(device,12836,vb);
    put(device,13072,0x10020000);put(device,13068,0x10020100);
    put(device,1912,0x07002003);put(device,1916,0x10000102);memory[device][12904]=6;
    put(device,10444,0xFFFF);put(device,10448,0);
    put(device,10548,0x2);put(device,10460,0xF); // Actual driver state is not zero PM4 leftovers.
    put(device,1920,0x12345678);put(device,6016,0x87654321);
    put(ib,0,0x20000000);put(ib,24,0xA7001000);put(ib,28,12);
    put(vb,24,0xA7002003);put(vb,28,0x10000102);
    unsigned reads=0;
    const Gta5ResourceReader reader=[&](std::uint32_t at,std::uint32_t n)
        ->std::optional<std::vector<std::uint8_t>> {
        ++reads;auto i=memory.upper_bound(at);if(i==memory.begin())return std::nullopt;--i;
        if(std::uint64_t{at-i->first}+n>i->second.size())return std::nullopt;
        return std::vector<std::uint8_t>(i->second.begin()+at-i->first,i->second.begin()+at-i->first+n);
    };
    const DrawBinState bins{{0xFFFFFFFF,0,0xFFFFFFFF,0},15};
    Gta5BoundIndexedRequest request{4,0,0,3};
    const auto initial=memory;
    auto draw=captureGta5BoundIndexedDraw(device,request,bins,reader,reader);
    assert(draw && draw->rawIndices==std::vector<std::uint32_t>({1,2,3}));
    assert(draw->effectiveIndices==draw->rawIndices && draw->draw.vertices.size()==96);
    assert(draw->indexPhysicalAddress==0x07001000 && draw->draw.vertexAddress==0x07002000);
    assert(gta5DrawRegister(draw->draw,0x2200)==2 && gta5DrawRegister(draw->draw,0x2104)==15);
    assert(draw->draw.vertexConstants[0]==0x12345678 && draw->draw.pixelConstants[0]==0x87654321);
    assert(memory==initial); // Capture cannot clear flags, consume commands or mutate resources.
    request.startIndex=2;request.baseVertex=0xFFFFFFFF;
    draw=captureGta5BoundIndexedDraw(device,request,bins,reader,reader);
    assert(draw && draw->rawIndices==std::vector<std::uint32_t>({3,4,5}));
    assert(draw->effectiveIndices==std::vector<std::uint32_t>({2,3,4}));
    put(device,10444,3);put(device,10448,3);
    draw=captureGta5BoundIndexedDraw(device,request,bins,reader,reader);
    assert(draw && draw->effectiveIndices==std::vector<std::uint32_t>({3,3,3}));
    put(device,10444,0xFFFF);put(device,10448,0);
    request={4,0,0,3};
    // Physical conversion must handle C/D and the E-view page bias without
    // asking the reader to invent an alias or ignore the resource interval.
    put(vb,24,0xC7002003);assert(captureGta5BoundIndexedDraw(device,request,bins,reader,reader));
    put(vb,24,0xE7001003);assert(captureGta5BoundIndexedDraw(device,request,bins,reader,reader));
    put(vb,24,0xA7002003);
    put(device,1912,0x0700201B);put(device,1916,0x100000EA);
    assert(captureGta5BoundIndexedDraw(device,request,bins,reader,reader));
    put(device,1916,0x10000102);assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,reader));
    put(device,1912,0x07002003);
    const auto rejected=[&] {assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,reader));};
    put(device,1912,0x07002000);rejected();put(device,1912,0x07002003);
    put(device,1916,0x10000101);rejected();put(device,1916,0x10000102);
    // Actual resource objects reside in allocated A/B graphics memory, not
    // the CPU heap. Exercise both namespaces without a masked RAM fallback.
    constexpr std::uint32_t graphicsIb=0xB7004000,graphicsVb=0xB7004030;
    memory[graphicsIb]=memory[ib];memory[graphicsVb]=memory[vb];
    put(device,12812,graphicsIb);put(device,12836,graphicsVb);
    unsigned graphicsHeaderReads=0;
    const Gta5ResourceReader heapOnly=[&](auto a,auto n) {
        return a>=0xA0000000?std::optional<std::vector<std::uint8_t>>{}:reader(a,n);
    };
    const Gta5ResourceReader graphicsOnly=[&](auto a,auto n) {
        if(a>=0xA0000000)++graphicsHeaderReads;
        return a>=0x10000000 && a<0xA0000000?std::optional<std::vector<std::uint8_t>>{}:reader(a,n);
    };
    assert(captureGta5BoundIndexedDraw(device,request,bins,heapOnly,graphicsOnly));
    assert(graphicsHeaderReads==2);
    memory.erase(graphicsIb);assert(!captureGta5BoundIndexedDraw(device,request,bins,heapOnly,graphicsOnly));
    put(device,12812,ib);put(device,12836,vb);
    request.indexCount=9;rejected();request.indexCount=3;
    request.startIndex=1;rejected();request.startIndex=0;
    request.baseVertex=100000;rejected();request.baseVertex=0;
    put(ib,0,0x80000000);rejected();put(ib,0,0x40000000);rejected();put(ib,0,0x20000000);
    memory[device][11068]=1;rejected();memory[device][11068]=0;
    memory[device][12498]=1;rejected();memory[device][12498]=0;
    memory[device][12563]=1;rejected();memory[device][12563]=0;
    memory[device][12904]=0;rejected();memory[device][12904]=6;
    put(device,10568,1u<<21);rejected();put(device,10568,0); // Primitive restart needs a separate indexing policy.
    put(device,10448,0x10000);rejected();put(device,10448,0);
    put(ib,28,5);rejected();put(ib,28,12);
    put(vb,28,0x100000FE);rejected();put(vb,28,0x10000102);
    put(vb,24,0xA7002000);rejected();put(vb,24,0xA7002003);
    put(vb,28,0x20000102);rejected();put(vb,28,0x10000102);
    put(vb,28,0x10000101);rejected();put(vb,28,0x10000102);
    auto unknown=bins;unknown.knownWords=0;unknown.words={};
    reads=0;assert(!captureGta5BoundIndexedDraw(device,request,unknown,reader,reader) && !reads);
    auto skipped=bins;skipped.words[2]=0;
    assert(!captureGta5BoundIndexedDraw(device,request,skipped,reader,reader) && !reads);
    request.primitive=13;rejected();assert(!reads);
    request.primitive=4;request.indexCount=65538;rejected();assert(!reads);
    request.indexCount=3;
    const Gta5ResourceReader shortRead=[&](auto a,auto n) {
        auto b=reader(a,n);if(b && !b->empty())b->pop_back();return b;
    };
    assert(!captureGta5BoundIndexedDraw(device,request,bins,shortRead,reader));
    assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,shortRead));
    // A diagnostic may own the real recorded bytes, without manufacturing a
    // bin-pass token or making those bytes admissible to the execution helper.
    memory=initial;
    memory[device][11068]=0x21;
    const auto recordedMemory=memory;
    reads=0;
    assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,reader));
    assert(reads==1); // Same early rejection; no index/vertex payload read.
    const auto recorded=captureGta5RecordedIndexedStorage(device,request,reader,reader);
    assert(recorded && recorded->draw.deviceState[11068]==0x21);
    assert(recorded->rawIndices==std::vector<std::uint32_t>({1,2,3}));
    assert(recorded->effectiveIndices==recorded->rawIndices && recorded->draw.vertices.size()==96);
    assert(recorded->draw.vertices==std::vector<std::uint8_t>(96,0xA5));
    assert(memory==recordedMemory);
    // Original bounded CPU emitter windows:64 command bytes,16 list bytes.
    {
        constexpr unsigned command=0xA7200000,record=0xA7210004;
        memory[command+4].resize(64,0xCD);memory[record].resize(16,0xEF);
        auto input=*recorded;
        const auto set=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)input.draw.deviceState.at(at+i)=value>>(24-i*8);};
        set(48,command);set(52,command+64);set(56,command);set(13628,record);set(13632,record+16);
        auto window=describeGta5RecordedIndexedEmission(input,reader);assert(window && window->command==command && window->record==record);
        const auto good=input;
        for(auto entry:std::array<std::array<unsigned,2>,8>{{{48,command+1},{48,0},{52,command+63},{56,command-4},
                {13628,record+1},{13632,record+15},{13628,command+4},{48,0xFFFFFFFC}}}) {
            set(entry[0],entry[1]);assert(!describeGta5RecordedIndexedEmission(input,reader));input=good;
        }
        assert(!describeGta5RecordedIndexedEmission(input,shortRead));
        input.request.indexCount=65536;assert(!describeGta5RecordedIndexedEmission(input,reader));input=good;
        input.draw.deviceState[11068]=0x20;assert(!describeGta5RecordedIndexedEmission(input,reader));
        memory=recordedMemory;
    }
    const auto recordingRejected=[&] {
        assert(!captureGta5RecordedIndexedStorage(device,request,reader,reader));
    };
    for(auto flag:{0,1,0x20}) {memory[device][11068]=flag;reads=0;recordingRejected();assert(reads==1);}
    memory=recordedMemory;
    for(auto offset:{12498,12563}) {memory[device][offset]=1;reads=0;recordingRejected();assert(reads==1);memory=recordedMemory;}
    put(device,10568,1u<<21);reads=0;recordingRejected();assert(reads==1);memory=recordedMemory;
    request.indexCount=65538;reads=0;recordingRejected();assert(!reads);request.indexCount=3;
    request.primitive=13;recordingRejected();assert(!reads);request.primitive=4;
    request.baseVertex=100000;recordingRejected();request.baseVertex=0;
    put(ib,28,5);recordingRejected();memory=recordedMemory;
    put(vb,28,0x100000FE);recordingRejected();memory=recordedMemory;
    put(device,1916,0x10000101);recordingRejected();memory=recordedMemory;
    memory[0x07002000].resize(95);recordingRejected();memory=recordedMemory;
    memory.erase(ib);recordingRejected();memory=recordedMemory;
    assert(!captureGta5RecordedIndexedStorage(device,request,shortRead,reader));
    assert(!captureGta5RecordedIndexedStorage(device,request,reader,shortRead));
    memory[graphicsIb]=memory[ib];memory[graphicsVb]=memory[vb];
    put(device,12812,graphicsIb);put(device,12836,graphicsVb);graphicsHeaderReads=0;
    assert(captureGta5RecordedIndexedStorage(device,request,heapOnly,graphicsOnly));
    assert(graphicsHeaderReads==2);
    memory.erase(graphicsVb);
    assert(!captureGta5RecordedIndexedStorage(device,request,heapOnly,graphicsOnly));
    // Original single-packet counts, including live draw88's4359. The recorded
    // and execution capture domains both enforce extents; recording still cannot
    // manufacture a bin-pass token or enter the execution capture domain.
    static_assert(Gta5SingleIndexedDrawMaxIndices==65535);
    for(unsigned count:{4095u,4098u,4359u,65535u}) {
        memory=recordedMemory;
        constexpr unsigned largeIndices=0x07100000;
        memory[largeIndices].resize(count*2);
        for(unsigned i=0;i<count;++i)memory[largeIndices][i*2+1]=i%3+1;
        put(ib,24,0xA7100000);put(ib,28,count*2);request.indexCount=count;
        const auto before=memory;
        auto large=captureGta5RecordedIndexedStorage(device,request,reader,reader);
        assert(large && large->rawIndices.size()==count && large->draw.vertices.size()==96);
        assert(memory==before);
        reads=0;assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,reader) && reads==1);
        memory[device][11068]=0;
        const auto nativeMemory=memory;
        const auto executable=captureGta5BoundIndexedDraw(device,request,bins,reader,reader);
        assert(executable && executable->rawIndices==large->rawIndices && executable->effectiveIndices==large->effectiveIndices);
        assert(executable->draw.vertices==large->draw.vertices && memory==nativeMemory);
        memory[largeIndices].pop_back();rejected();memory=nativeMemory;
        put(ib,28,count*2-1);rejected();memory=nativeMemory;
        assert(!captureGta5BoundIndexedDraw(device,request,bins,reader,shortRead));
        for(unsigned invalid:{0u,65536u,65538u,0xFFFFFFFFu}) {
            request.indexCount=invalid;reads=0;rejected();assert(!reads);
        }
        request.indexCount=count;memory=before;
        memory[largeIndices].pop_back();recordingRejected();memory=before;
        put(ib,28,count*2-1);recordingRejected();memory=before;
        assert(!captureGta5RecordedIndexedStorage(device,request,reader,shortRead));
        constexpr unsigned command=0xA7200000,record=0xA7210004;
        memory[command+4].resize(64);memory[record].resize(16);
        for(auto item:std::array<std::array<unsigned,2>,5>{{{48,command},{52,command+64},{56,command},{13628,record},{13632,record+16}}})
            for(unsigned i=0;i<4;++i)large->draw.deviceState[item[0]+i]=item[1]>>(24-i*8);
        assert(describeGta5RecordedIndexedEmission(*large,reader));
        large->rawIndices.pop_back();assert(!describeGta5RecordedIndexedEmission(*large,reader));
    }
    std::puts("BOUND_INDEXED_SINGLE_DRAW_RANGE_PASS counts=4095,4098,4359,65535 physical_extents=1 oversize_reject_before_read=1 guest_writes=0");
    std::puts("BOUND_INDEXED_RECORDED_STORAGE_PASS original_flags=1 execution_rejected=1 physical_bounds=1 guest_writes=0");
    std::puts("BOUND_INDEXED_CAPTURE_PASS prepared_state=1 physical_bounds=1 index_wrap_clamp=1 guest_writes=0");
}
