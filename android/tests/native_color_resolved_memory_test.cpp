#include "native_color_resolved_memory.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    std::vector<std::uint8_t> shared(65536,0xA5),copy(4194304,0x6C),readOnly(4096,0xDE);
    NativeColorMemoryRequest request;request.inputCount=48;
    for(unsigned i=0;i<48;++i)request.inputs[i]={0x4000+i*32,16};
    request.controls={{{0x1000,4096},{0x3000,400},{0x2000,4},{0x2004,4}}};
    request.outputs={{{0x800000,3768320},{0x2100,4},{0x2104,4},request.controls[2]}};
    NativeColorResolvedMemory memory;
    const auto map=[&](NativePhysicalRange r){return NativeColorMappedRange{r,
        r.address>=0x800000?NativePhysicalRange{0x800000,4194304}:NativePhysicalRange{0x1000,65536},
        r.address>=0x800000?copy.data():shared.data(),true};};
    for(unsigned i=0;i<48;++i)memory.inputs[i]=map(request.inputs[i]);
    for(unsigned i=0;i<4;++i){memory.controls[i]=map(request.controls[i]);memory.outputs[i]=map(request.outputs[i]);}
    assert(validateNativeColorResolvedMemory(request,memory));
    unsigned rejections=0;
    const auto reject=[&](const auto& r,const auto& m){assert(!validateNativeColorResolvedMemory(r,m));++rejections;};
    auto minimum=request;minimum.inputCount=7;
    assert(validateNativeColorResolvedMemory(minimum,memory));
    minimum.inputCount=6;reject(minimum,memory);
    const auto entry=[](auto& m,unsigned i)->NativeColorMappedRange&{return i<48?m.inputs[i]:(i<52?m.controls[i-48]:m.outputs[i-52]);};
    for(unsigned i=0;i<56;++i)for(unsigned mode=0;mode<10;++mode){auto bad=memory;auto& m=entry(bad,i);
        if(mode==0)m.range.address+=4;if(mode==1)m.range.bytes+=4;if(mode==2)m.allocation.address=m.range.address+4;
        if(mode==3)m.allocation.bytes=0;if(mode==4)m.allocation.address=0x20000000;if(mode==5)m.allocationBytes=nullptr;
        if(mode==6)m.allocationBytes=reinterpret_cast<std::uint8_t*>(UINTPTR_MAX-3);
        if(mode==7)m.allocationBytes=i==52?shared.data():m.allocationBytes+4;
        if(mode==8)m.allocation.bytes=m.range.address-m.allocation.address+m.range.bytes-4;if(mode==9)m.range.address|=1;
        reject(request,bad);
    }
    for(unsigned i=0;i<4;++i){auto bad=memory;bad.outputs[i].writable=false;reject(request,bad);}
    for(unsigned mode=0;mode<8;++mode){auto r=request;
        if(mode==0)r.inputCount=0;if(mode==1)r.inputCount=49;if(mode==2)r.controls[0].bytes=0x2000000;
        if(mode==3)r.controls[1].bytes=16388;if(mode==4)r.controls[2].bytes=8;if(mode==5)r.controls[3].bytes=8;
        if(mode==6)r.outputs[0].bytes-=4;if(mode==7)r.outputs[3].address+=4;
        reject(r,memory);
    }
    // Coherent requested slices can share a read-only allocation, but no input
    // may share an output/control slice. The full copy padding is protected.
    for(unsigned i=0;i<48;++i){
        for(const auto& target:request.outputs){auto r=request;auto m=memory;r.inputs[i]={target.address+target.bytes-4,4};
            m.inputs[i]=map(r.inputs[i]);reject(r,m);}
        for(const auto& control:request.controls){auto r=request;auto m=memory;r.inputs[i]={control.address,4};
            m.inputs[i]=map(r.inputs[i]);reject(r,m);}
    }
    auto duplicate=request;auto dupeMemory=memory;duplicate.inputs[1]=duplicate.inputs[0];dupeMemory.inputs[1]=dupeMemory.inputs[0];
    assert(validateNativeColorResolvedMemory(duplicate,dupeMemory));
    auto r=request;auto m=memory;r.inputs[0]={0x100000,16};m.inputs[0]={r.inputs[0],{0x100000,4096},readOnly.data(),false};
    assert(validateNativeColorResolvedMemory(r,m));
    // A lone owner's host address is supplied by the provider, not discovered
    // by this metadata checker. Changing it to another valid buffer is allowed.
    std::vector<std::uint8_t> anotherReadOnly(4096,0xDE);m.inputs[0].allocationBytes=anotherReadOnly.data();
    assert(validateNativeColorResolvedMemory(r,m));
    // Disjoint requested host slices still reject overlapping whole owners.
    m.inputs[0].allocationBytes=shared.data()+32768;reject(r,m);
    auto alias=memory;alias.outputs[0].allocationBytes=shared.data();reject(request,alias);
    // One allocation cannot be represented by inconsistent ownership/protection.
    alias=memory;alias.inputs[0].writable=false;reject(request,alias);
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j){
        auto a=request;auto b=memory;a.outputs[i]=a.outputs[j];b.outputs[i]=b.outputs[j];reject(a,b);
        a=request;b=memory;a.controls[i]=a.controls[j];b.controls[i]=b.controls[j];reject(a,b);
    }
    assert(std::all_of(shared.begin(),shared.end(),[](auto b){return b==0xA5;}) &&
        std::all_of(copy.begin(),copy.end(),[](auto b){return b==0x6C;}) &&
        std::all_of(readOnly.begin(),readOnly.end(),[](auto b){return b==0xDE;}));
    std::printf("NATIVE_COLOR_RESOLVED_MEMORY: %u rejection controls; host/physical owners and exclusions; no reads, writes or lease acquisition PASS\n",rejections);
}
