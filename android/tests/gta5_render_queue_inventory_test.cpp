#include "gta5_render_queue_inventory.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <sstream>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
void put(Bytes& b,unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);}
int main() {
    constexpr unsigned Root=0x10004000,Bank=0xA0008000;
    std::map<unsigned,Bytes> h,g;h[Root]=Bytes(4096);g[Bank]=Bytes(65536);
    unsigned heapReads=0,graphicsReads=0;
    const auto reader=[](auto& map,unsigned& reads) {return [&map,&reads](unsigned at,unsigned n)->std::optional<Bytes> {
        ++reads;assert(n<=16380 && !(at&3));
        auto it=map.upper_bound(at);if(it==map.begin())return std::nullopt;--it;
        const auto offset=std::uint64_t{at}-it->first;
        if(offset+n>it->second.size())return std::nullopt;
        return Bytes(it->second.begin()+offset,it->second.begin()+offset+n);
    };};
    const auto heap=reader(h,heapReads),graphics=reader(g,graphicsReads);
    auto run=[&](unsigned head=0){return captureGta5RenderQueue(Root,head,heap,graphics);};
    put(h[Root],1032,Bank);put(h[Root],8+4*7,0x822F6500);put(h[Root],16,0);
    put(g[Bank],0,(5<<20)|(7<<12)|0xABC);put(g[Bank],20,2<<12);
    const auto beforeH=h,beforeG=g;
    auto inventory=run();assert(inventory.records.size()==2);
    assert(std::string(inventory.stop)=="linear-terminator");
    const auto& record=inventory.records[0];
    assert(record.advance==20 && record.handlerSlot==7 && record.handler==0x822F6500);
    assert(record.address==Bank && record.payload && record.payload->size()==20);
    assert(inventory.records[1].handler==0 && inventory.records[1].payload->size()==4);
    assert(inventory.payloadBytes==24 && !inventory.unavailableHandlers && !inventory.unavailablePayloads);
    assert(h==beforeH && g==beforeG && heapReads && graphicsReads);
    // Connected capture -> metadata/payload output requires no draw fixture.
    // A submission-only unwind must retain its queue without inventing shaders.
    std::map<std::string,Bytes> saved;
    std::ostringstream metadata;
    assert(writeGta5RenderQueueInventory(inventory,metadata,[&](const std::string& name,const Bytes& bytes){
        return saved.emplace(name,bytes).second;
    }));
    assert(saved.size()==2 && saved.at("queue-record-0.bin")==*record.payload &&
        saved.at("queue-record-1.bin")==*inventory.records[1].payload);
    assert(metadata.str().find("\"execution_proven\":false")!=std::string::npos &&
        metadata.str().find("potential-linear-fallthrough-at-capture-time")!=std::string::npos &&
        metadata.str().find("shader")==std::string::npos && h==beforeH && g==beforeG);
    std::ostringstream failedPayload;
    assert(!writeGta5RenderQueueInventory(inventory,failedPayload,[](const auto&,const auto&){return false;}));
    std::ostringstream failedMetadata;failedMetadata.setstate(std::ios::badbit);
    assert(!writeGta5RenderQueueInventory(inventory,failedMetadata,[](const auto&,const auto&){return true;}));
    // Missing payload and handler are independent. A null handler is available.
    Gta5ResourceReader sparseHeap=[&](unsigned at,unsigned n){return at==Root+36?std::optional<Bytes>{}:heap(at,n);};
    Gta5ResourceReader sparseGraphics=[&](unsigned at,unsigned n){return n>4?std::optional<Bytes>{}:graphics(at,n);};
    inventory=captureGta5RenderQueue(Root,0,sparseHeap,sparseGraphics);
    assert(inventory.records.size()==2 && inventory.unavailablePayloads==1 && inventory.unavailableHandlers==1);
    assert(!inventory.records[0].handler && !inventory.records[0].payload && inventory.records[1].handler==0);
    std::ostringstream sparseMetadata;saved.clear();
    assert(writeGta5RenderQueueInventory(inventory,sparseMetadata,[&](const std::string& name,const Bytes& bytes){
        return saved.emplace(name,bytes).second;
    }));
    assert(saved.size()==1 && !saved.count("queue-record-0.bin") &&
        sparseMetadata.str().find("\"handler\":null")!=std::string::npos &&
        sparseMetadata.str().find("\"unavailable_payloads\":1")!=std::string::npos);
    // Exact original bit extraction, independent rotate-and-mask oracle.
    for(unsigned size=0;size<4096;++size)for(unsigned slot=0;slot<256;++slot) {
        const auto w=(size<<20)|(slot<<12)|0xA59;
        assert((((w<<14)|(w>>18))&0x3FFC)==size*4);
        assert((((w<<20)|(w>>12))&0xFF)==slot);
    }
    put(g[Bank],0,7<<12);inventory=run();
    assert(inventory.records.size()==1 && std::string(inventory.stop)=="zero-advance-unknown-control-flow");
    // The decoder must not follow/execute a handler that could change the head.
    put(h[Root],1032+20,Bank);put(g[Bank],0,2<<12);
    inventory=run(0x01000000);assert(inventory.records.size()==1 && inventory.records[0].bankBase==Bank);
    inventory=run(0x80000000);assert(inventory.records.empty() && std::string(inventory.stop)=="negative-bank-uninventoried");
    put(h[Root],1032,0);assert(std::string(run().stop)=="null-bank");put(h[Root],1032,Bank);
    assert(std::string(run(1).stop)=="record-header-unavailable");
    assert(std::string(captureGta5RenderQueue(0,0,heap,graphics).stop)=="invalid-root");
    assert(std::string(captureGta5RenderQueue(UINT32_MAX-3,0,heap,graphics).stop)=="bank-pointer-unavailable");
    put(h[Root],1032,UINT32_MAX-3);assert(std::string(run(4).stop)=="record-header-unavailable");
    put(h[Root],1032,Bank);
    for(unsigned i=0;i<129;++i)put(g[Bank],i*4,(1<<20)|(7<<12));
    inventory=run();assert(inventory.records.size()==128 && std::string(inventory.stop)=="record-limit");
    g[Bank]=Bytes(300000);for(unsigned i=0;i<18;++i)put(g[Bank],i*16380,(4095<<20)|(7<<12));
    inventory=run();assert(inventory.records.size()==17 && inventory.payloadBytes==16*16380);
    assert(std::string(inventory.stop)=="payload-budget" && !inventory.records.back().payload);
    // Namespace selection is explicit: never retry missing graphics in heap.
    h[Bank]=g[Bank];g.clear();assert(run().records.empty());
    std::puts("GTA5_RENDER_QUEUE_INVENTORY_PASS bounded read-only potential fallthrough, queue-only export, independent missing inputs, no dispatch");
}
