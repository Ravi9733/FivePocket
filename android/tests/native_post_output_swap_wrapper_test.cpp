#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    const auto command=NativeSwapCommand::make({2,0x2028,0x100000,67});assert(command);
    const auto bytes=test::postOutputSwapBytes(*command);
    const auto parse=[&](const auto& input){return NativePostOutputSwapWrapper::parse(*command,0x2000,input.data(),input.size());};
    const auto plan=parse(bytes);assert(plan && plan->scratchPhysical()==0x30000 && plan->callbackAddress()==0x82001000 &&
        plan->callbackContext()==0x100208 && plan->operations().size()==12 &&
        plan->operations()[8].kind==NativePostOutputSwapWrapper::Kind::Interrupt && plan->operations()[8].wordOffset==105 &&
        plan->operations()[9].wordOffset==107 && plan->operations()[10].kind==NativePostOutputSwapWrapper::Kind::WaitScratch &&
        plan->operations()[10].target==0 && plan->operations()[10].value==0 && plan->operations()[11].wordOffset==115);
    constexpr std::array<unsigned,4> eventOffsets{6,74,121,125};
    for(unsigned i=0;i<4;++i)assert(plan->events()[i].wordOffset==eventOffsets[i]);
    unsigned immutable=0;
    for(unsigned word=0;word<129;++word){if(word==123 || word==124 || word==127 || word==128)continue;
        for(unsigned bit=0;bit<32;++bit){auto changed=bytes;changed[word*4+3-bit/8]^=1u<<(bit%8);assert(!parse(changed));++immutable;}}
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned lane=0;lane<4;++lane)b[at*4+lane]=v>>(24-lane*8);};
    for(unsigned literal:{0u,0xFFFFFFFFu}){auto changed=bytes;put(changed,124,literal);put(changed,128,literal);assert(parse(changed));}
    for(unsigned at:{123u,127u})for(unsigned target:{0x2002u,0x21FEu,0x2202u,0x30002u,0x3001Eu,0x100002u,0x20000002u}){
        auto changed=bytes;put(changed,at,target);assert(!parse(changed));}
    for(unsigned callback:{0u,1u,0x0BADF00Du}){auto changed=bytes;put(changed,83,callback);put(changed,102,callback);assert(!parse(changed));}
    for(unsigned context:{0u,0x208u,0x101208u,0x100308u}){auto changed=bytes;put(changed,84,context);put(changed,96,context);assert(!parse(changed));}
    for(unsigned scratch:{0x2000u,0x2200u,0x8000u,0x9000u,0x100000u,0x1FFFFFF0u})assert(!parse(test::postOutputSwapBytes(*command,scratch)));
    for(unsigned size:{0u,360u,512u,520u})assert(!NativePostOutputSwapWrapper::parse(*command,0x2000,bytes.data(),size));
    assert(!NativePostOutputSwapWrapper::parse(*command,0x2000,nullptr,bytes.size()));
    const auto first=NativeSwapCommand::make({1,0x2028,0x100000,67});assert(first);
    assert(!NativePostOutputSwapWrapper::parse(*first,0x2000,bytes.data(),bytes.size()));
    assert(!NativeSwapWrapper::parse(*command,0x2000,bytes.data(),bytes.size()));
    assert(plan->sameRequest(*parse(bytes)) && plan->command().bytes()==command->bytes());
    unsigned identityControls=0;
    const auto different=[&](const auto& ticket,const auto& input,unsigned physical=0x2000){
        const auto candidate=NativePostOutputSwapWrapper::parse(ticket,physical,input.data(),input.size());
        assert(candidate && !candidate->sameRequest(*plan) && !plan->sameRequest(*candidate));++identityControls;};
    for(unsigned word:{123u,124u,127u,128u}){auto changed=bytes;put(changed,word,(word==123 || word==127)?0xB002u:0xFEDCBA98u);different(*command,changed);}
    different(*command,test::postOutputSwapBytes(*command,0x31000));
    different(*command,test::postOutputSwapBytes(*command,0x30000,0x82002000));
    different(*command,test::postOutputSwapBytes(*command,0x30000,0x82001000,0x8100));
    for(unsigned which=0;which<3;++which){auto identity=command->identity();
        if(which==0)identity.commandPhysical+=0x400;else if(which==1)identity.imagePhysical+=0x400000;else ++identity.previousReadWord;
        const auto changed=NativeSwapCommand::make(identity);assert(changed);
        different(*changed,test::postOutputSwapBytes(*changed),identity.commandPhysical-40);
    }
    assert(identityControls==10);
    assert(plan->scratchStateMatches(0x30000,0x20031) && plan->scratchStateMatches(0x30000,0x20037));
    for(unsigned bit:{0u,4u,5u,17u})assert(!plan->scratchStateMatches(0x30000,0x20031^(1u<<bit)));
    assert(!plan->scratchStateMatches(0x30004,0x20031) && immutable==4000);
    std::puts("POST_OUTPUT_SWAP_WRAPPER:4000immutable bits,12IRQ obligations,full129word alias bounds,nonzero surface context,old90word plan refused; no execution PASS");
    std::puts("POST_OUTPUT_SWAP_REQUEST_IDENTITY:10valid-but-different requests rejected,owned native ticket and full mutable event identity PASS");
}
