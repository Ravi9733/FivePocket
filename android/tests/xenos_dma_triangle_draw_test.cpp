#include "xenos_dma_triangle_draw.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>

using namespace recomp::gpu;
using Words=std::vector<std::uint32_t>;
static Pm4Snapshot snapshot(const Words& words) {
    std::vector<std::uint8_t> bytes;
    for (auto w:words) for (unsigned lane=0;lane<4;++lane) bytes.push_back(w>>(24-8*lane));
    Pm4Snapshot s; Pm4Error error;
    assert(decodePm4(bytes.data(),bytes.size(),{},s,error)); return s;
}
int main() {
    const Words words{0xC0032201,0,0x60004,0x1000,0x40000006};
    const DrawBinState pass{{0xFFFFFFFF,0,0xFFFFFFFF,0},15};
    DmaTriangleInput output; DmaTriangleFailure error;
    std::vector<std::uint8_t> memory{0,0,0,1,0,2,0x12,0x34,0xAB,0xCD,0xFF,0xFF};
    unsigned reads=0;
    const Pm4PhysicalReader reader=[&](auto address,auto bytes) -> std::optional<std::vector<std::uint8_t>> {
        ++reads; assert(address==0x1000 && bytes==12); return memory;
    };
    assert(captureDmaTriangleInput(snapshot(words),0,pass,reader,output,error));
    const auto expected=std::vector<std::uint32_t>{0,1,2,0x1234,0xABCD,0xFFFF};
    assert(output.indices==expected && output.request.indexCount==6 && reads==1);
    // Values intentionally exceed the count: this is index decoding, not an
    // invented bound on vertices. Ownership is supplied by the reader.
    memory.assign(memory.size(),0); assert(output.indices==expected);
    const auto rejected=[&](Words w,DmaTriangleFailure why,DrawBinState bins=DrawBinState{{0xFFFFFFFF,0,0xFFFFFFFF,0},15}) {
        const auto before=reads;
        assert(!captureDmaTriangleInput(snapshot(w),0,bins,reader,output,error));
        assert(error==why && reads==before && output.indices==expected);
    };
    auto w=words; w[1]=0x100; rejected(w,DmaTriangleFailure::Layout);
    for(auto bit:{6u,8u,10u,11u,12u,13u}) {
        w=words; w[2]|=1u<<bit; rejected(w,DmaTriangleFailure::Layout);
    }
    for(auto size:{0x00000006u,0x80000006u,0xC0000006u,0x41000006u}) {
        w=words;w[4]=size;rejected(w,DmaTriangleFailure::Layout);
    }
    for(auto count:{0u,5u,65534u}) {
        w=words;w[2]=(count<<16)|4;w[4]=0x40000000|count;rejected(w,DmaTriangleFailure::Count);
    }
    w=words;w[4]++;rejected(w,DmaTriangleFailure::Count);
    for(auto address:{0x1001u,0x1FFFFFFCu,0x20000000u,0xFFFFFFFFu}) {
        w=words;w[3]=address;rejected(w,DmaTriangleFailure::Address);
    }
    rejected(words,DmaTriangleFailure::Predicate,{});
    rejected(words,DmaTriangleFailure::Predicate,{{0,0,0,0},15});
    rejected(words,DmaTriangleFailure::Predicate,{{1,0,0,0},0});
    assert(!captureDmaTriangleInput(snapshot(words),1,pass,reader,output,error));
    assert(error==DmaTriangleFailure::Packet);
    for(const auto& bad:std::vector<Pm4PhysicalReader>{
            {}, [](auto,auto)->std::optional<std::vector<std::uint8_t>> {return std::nullopt;},
            [](auto,auto)->std::optional<std::vector<std::uint8_t>> {return std::vector<std::uint8_t>(11);},
            [](auto,auto)->std::optional<std::vector<std::uint8_t>> {throw std::runtime_error("rejected");}}) {
        assert(!captureDmaTriangleInput(snapshot(words),0,pass,bad,output,error));
        assert(output.indices==expected);
    }
    // Unpredicated requests do not fabricate bin state; exact unknown-zero
    // state is valid and does not gate the draw. Still no execution occurs.
    w=words;w[0]&=~1u;
    DmaTriangleRequest request;
    assert(planDmaTriangleRequest(snapshot(w),0,{},request,error));
    assert(!request.predicate.predicated && request.predicate.state.knownWords==0);
    // Stopped diagnostic capture must not invent passing bin predicates. It
    // captures bytes only, while the execution planner still rejects unknown.
    Pm4IndirectGraph graph;graph.totalWords=5;graph.totalPackets=1;
    StoppedDmaInputs stopped;
    assert(captureStoppedDmaInputs(snapshot(words),graph,reader,stopped));
    assert(stopped.inputs.size()==1 && stopped.requestedBytes==12 && !stopped.unavailable);
    assert(stopped.inputs[0].layout.predicate.predicated);
    assert(stopped.inputs[0].layout.predicate.state.knownWords==0);
    assert(evaluateDrawBinPredicate(stopped.inputs[0].layout.predicate.state,true)==DrawBinDecision::Unknown);
    assert(stopped.inputs[0].bytes==memory);
    assert(!planDmaTriangleRequest(snapshot(words),0,{},request,error));
    // Record unsupported layout and unavailable reads distinctly, without
    // discarding later requests or fabricating any payload.
    w=words;w[4]=6;
    auto mixed=words;mixed.insert(mixed.end(),w.begin(),w.end());
    graph.totalWords=10;graph.totalPackets=2;
    const Pm4PhysicalReader unavailable=[](auto,auto)->std::optional<std::vector<std::uint8_t>>{return std::nullopt;};
    assert(captureStoppedDmaInputs(snapshot(mixed),graph,unavailable,stopped));
    assert(stopped.inputs.size()==2 && stopped.unavailable==2 && stopped.requestedBytes==12);
    assert(stopped.inputs[0].failure==DmaTriangleFailure::ReadRejected);
    assert(stopped.inputs[1].failure==DmaTriangleFailure::Layout);
    assert(stopped.inputs[0].bytes.empty() && stopped.inputs[1].bytes.empty());
    const auto root=snapshot({0xC0013F00,0x2000,5});
    Pm4IndirectGraph connected;Pm4GraphError graphError;
    const Pm4PhysicalReader graphReader=[&](auto address,auto count)->std::optional<std::vector<std::uint8_t>> {
        if(address==0x1000 && count==memory.size())return memory;
        if(address!=0x2000 || count!=20)return std::nullopt;
        std::vector<std::uint8_t> bytes;
        for(auto value:words)for(unsigned lane=0;lane<4;++lane)bytes.push_back(value>>(24-8*lane));
        return bytes;
    };
    assert(inspectPm4IndirectGraph(root,graphReader,connected,graphError));
    StoppedDmaInputs nested;
    assert(captureStoppedDmaInputs(root,connected,graphReader,nested));
    assert(nested.inputs.size()==1 && nested.inputs[0].node==1 && nested.inputs[0].bytes==memory);
    Words tooMany;
    for(unsigned i=0;i<257;++i)tooMany.insert(tooMany.end(),words.begin(),words.end());
    assert(!captureStoppedDmaInputs(snapshot(tooMany),{},reader,stopped));
    assert(stopped.inputs.size()==2 && stopped.unavailable==2);
    // Original driver packet count is16-bit, independent of unique vertices.
    // Exercise the full connected bounded reader, not only layout admission.
    unsigned largeCases=0;
    for(unsigned count:{4095u,4098u,4230u,4320u,4359u,4380u,4896u,5733u,6144u,7272u,65535u}) {
        auto large=words;large[2]=(count<<16)|4;large[4]=0x40000000u|count;
        std::vector<std::uint8_t> bytes(count*2);
        for(unsigned i=0;i<count;++i) {bytes[i*2]=i>>8;bytes[i*2+1]=i;}
        unsigned calls=0;const Pm4PhysicalReader bounded=[&](auto address,auto size)->std::optional<std::vector<std::uint8_t>> {
            ++calls;assert(address==0x1000 && size==bytes.size());return bytes;
        };
        DmaTriangleInput decoded;
        assert(captureDmaTriangleInput(snapshot(large),0,pass,bounded,decoded,error) && calls==1);
        assert(decoded.indices.size()==count && decoded.request.indexCount==count);
        for(unsigned i=0;i<count;++i)assert(decoded.indices[i]==i);
        const auto before=decoded.indices;
        const Pm4PhysicalReader truncated=[&](auto,auto)->std::optional<std::vector<std::uint8_t>> {
            return std::vector<std::uint8_t>(bytes.size()-1);
        };
        assert(!captureDmaTriangleInput(snapshot(large),0,pass,truncated,decoded,error) && decoded.indices==before);
        auto bad=large;bad[3]=0x20000000u-count*2+2;
        assert(!captureDmaTriangleInput(snapshot(bad),0,pass,bounded,decoded,error) && calls==1 && decoded.indices==before);
        ++largeCases;
    }
    // A count overflowing16bits cannot be hidden in DMA_SIZE.
    w=words;w[2]=4;w[4]=0x40010000u;rejected(w,DmaTriangleFailure::Count);
    std::printf("DMA_TRIANGLE_LARGE_INPUT_PASS cases=%u max_count=65535 truncated_preserves=1 range_checked_before_read=1 gpu_execution=0\n",largeCases);
    std::puts("DMA_TRIANGLE_INPUT_PASS ordered_be16=1 guards=1 owned_snapshot=1 guest_execution=0");
}
