#pragma once
#include "xenos_maintenance_point_plan.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>

namespace recomp::gpu::test::maintenance {
using Stream=std::vector<std::uint32_t>;
inline void append(Stream& to,const Stream& from) {to.insert(to.end(),from.begin(),from.end());}
inline Stream idle() {return {0x5C8,0x20000,0xE00,1,0xE40,1};}
inline Stream settings() {
    return {0x2180,0x1000000E,0x2181,0,0x2104,0,0x2200,0,0x2208,4,0x2293,0,0x2280,0x80008};
}
inline Stream load(unsigned stage,const Bytes& data) {
    assert(data.size()%4==0 && !data.empty());
    const auto words=static_cast<std::uint32_t>(data.size()/4);
    Stream result{0xC0002B00|((words+1)<<16),stage,words};
    for(std::size_t i=0;i<data.size();i+=4) result.push_back((std::uint32_t{data[i]}<<24)|
        (std::uint32_t{data[i+1]}<<16)|(std::uint32_t{data[i+2]}<<8)|data[i+3]);
    return result;
}
inline Stream shaders() {
    auto result=load(0,program({alloc(2),exec(2,1,1),alloc(1),exec(3)},
        {alu(0,0,0,true,true),alu(62,8,8,true,true)}));
    append(result,load(1,program({alloc(),exec()},{alu()})));return result;
}
inline Stream draw() {return {0xC0003600,0x10081};}
inline Stream event(unsigned address=0x2000,unsigned value=7) {return {0xC0025800,3,address|2,value};}
inline Stream batch(unsigned count=96) {
    auto result=idle();append(result,settings());append(result,shaders());
    append(result,{0xC0004600,15});
    for(unsigned i=0;i<count;++i)append(result,draw());
    append(result,event());append(result,event(0x2004,0x12345678));return result;
}
inline Pm4Snapshot snapshot(const Stream& words) {
    Bytes bytes;for(auto word:words)for(int shift=24;shift>=0;shift-=8)bytes.push_back(word>>shift);
    Pm4Snapshot output;Pm4Error error;assert(decodePm4(bytes.data(),bytes.size(),{},output,error));return output;
}
inline Pm4IndirectGraph flat(const Pm4Snapshot& root) {
    Pm4IndirectGraph graph;graph.totalWords=root.words().size();graph.totalPackets=root.packets().size();return graph;
}
}
