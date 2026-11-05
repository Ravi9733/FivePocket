#include "native_display_output_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    const auto bytes=test::displayOutputBytes();
    const auto plan=NativeDisplayOutputWrapper::parse(0x10000,bytes.data(),bytes.size());assert(plan);
    assert(plan->physical()==0x10000 && plan->eventPhysical(0)==0x20000 && plan->eventPhysical(1)==0x20004 &&
        plan->eventValue(0)==0x1234 && plan->eventValue(1)==0xABCD);
    unsigned controls=0;
    for(unsigned i=0;i<64;++i)for(unsigned b=0;b<8;++b) {
        auto bad=bytes;bad[i]^=1u<<b;assert(!plan->matches(0x10000,bad.data(),bad.size()));
        if(i/4==10 || i/4==11 || i/4==14 || i/4==15)continue;
        assert(!NativeDisplayOutputWrapper::parse(0x10000,bad.data(),bad.size()));++controls;
    }
    assert(controls==384);
    for(unsigned size:{0u,1u,60u,68u})assert(!NativeDisplayOutputWrapper::parse(0x10000,bytes.data(),size));
    for(unsigned at:{1u,0x1FFFFFC4u,0x20000000u,0xFFFFFFFCu})assert(!NativeDisplayOutputWrapper::parse(at,bytes.data(),64));
    assert(!NativeDisplayOutputWrapper::parse(0,nullptr,64));
    assert(!plan->matches(0x10004,bytes.data(),64) && !plan->matches(0x10000,nullptr,64));
    for(unsigned at:{0x10000u,0x10004u,0x1003Cu,0x20004u,0x20000000u,0xFFFFFFFCu}) {
        const auto bad=test::displayOutputBytes(at);assert(!NativeDisplayOutputWrapper::parse(0x10000,bad.data(),64));
    }
    for(unsigned known:{0u,1u,0xAAAAAAAAu,0xFFFFFFFFu})for(unsigned value:{0u,1u,0x55555555u,0xAAAAAAAAu,0xFFFFFFFFu}) {
        const auto changed=NativeDisplayOutputWrapper::clearForceBit({value,known});
        assert(changed.first==(value&~1u) && changed.second==(known|1u));
        for(unsigned bit=1;bit<32;++bit)assert(((changed.first^value)&(1u<<bit))==0 && ((changed.second^known)&(1u<<bit))==0);
    }
    NativeDisplayOutputResult result{true,2,{}};
    for(unsigned i=0;i<2;++i)for(unsigned b=0;b<4;++b)result.eventBytes[i*4+b]=plan->eventValue(i)>>(24-b*8);
    assert(validNativeDisplayOutputResult(*plan,result));
    for(unsigned bit=0;bit<64;++bit){auto bad=result;bad.eventBytes[bit/8]^=1u<<(bit%8);assert(!validNativeDisplayOutputResult(*plan,bad));}
    for(unsigned bit=0;bit<32;++bit){auto bad=result;bad.nativeFences^=1u<<bit;assert(!validNativeDisplayOutputResult(*plan,bad));}
    result.presentQueued=false;assert(!validNativeDisplayOutputResult(*plan,result));
    std::puts("NATIVE_DISPLAY_OUTPUT:384packet bits,partial-register knowledge,96receipt bits/aliases PASS; no retail link equivalence");
}
