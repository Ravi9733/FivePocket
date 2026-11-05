#include "native_swap_command.h"
#include "xenos_command_stream.h"
#include "xenos_native_state_batch.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    const NativeSwapCommand::Identity id{0x0102030405060708,0x07902C68,0x1F2D8000,37};
    const auto command=NativeSwapCommand::make(id);assert(command);
    const auto& bytes=command->bytes();
    assert(command->matches(id.commandPhysical,bytes.data(),bytes.size()));
    const auto word=[&](unsigned i){const auto* p=bytes.data()+i*4;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    assert(word(0)==0xFFFF5357 && word(1)==0x41500001 && word(2)==64 && word(3)==1 &&
        word(4)==0x01020304 && word(5)==0x05060708 && word(6)==id.commandPhysical &&
        word(7)==id.imagePhysical && word(8)==37 && word(9)==1280 && word(10)==720 &&
        word(11)==3768320 && word(12)==3686400);
    for(unsigned i=13;i<64;++i)assert(word(i)==0);
    unsigned rejected=0;
    for(unsigned at=0;at<bytes.size();++at)for(unsigned bit=0;bit<8;++bit){
        auto bad=bytes;bad[at]^=1u<<bit;assert(!command->matches(id.commandPhysical,bad.data(),bad.size()));++rejected;
    }
    for(unsigned size=0;size<256;++size)assert(!command->matches(id.commandPhysical,bytes.data(),size));
    assert(!command->matches(id.commandPhysical,bytes.data(),257));
    assert(!command->matches(id.commandPhysical,nullptr,256));
    assert(!command->matches(id.commandPhysical+4,bytes.data(),256));
    for(unsigned field=0;field<4;++field){auto altered=id;
        if(field==0)++altered.ticket;else if(field==1)altered.commandPhysical+=4;
        else if(field==2)altered.imagePhysical-=4096;else ++altered.previousReadWord;
        const auto other=NativeSwapCommand::make(altered);assert(other);
        assert(!other->matches(altered.commandPhysical,bytes.data(),bytes.size()));
    }
    for(unsigned fault=0;fault<9;++fault){auto bad=id;
        switch(fault){
            case 0:bad.ticket=0;break;case 1:bad.commandPhysical|=1;break;
            case 2:bad.commandPhysical=0x1FFFFF04;break;case 3:bad.imagePhysical|=4;break;
            case 4:bad.imagePhysical=0x1FFFF000;break;case 5:bad.previousReadWord=4*1024*1024;break;
            case 6:bad.commandPhysical=bad.imagePhysical;break;
            case 7:bad.commandPhysical=bad.imagePhysical+NativeSwapImagePlan::Capacity-4;break;
            case 8:bad.commandPhysical=bad.imagePhysical-4;break;
        }
        assert(!NativeSwapCommand::make(bad));
    }
    // Prove ordinary PM4 paths fail closed; no engine may turn this envelope
    // into a completed no-op/state-only batch. No live guest data or GPU here.
    Pm4Snapshot decoded;Pm4Error error;
    assert(!decodePm4(bytes.data(),bytes.size(),{4096,512,false},decoded,error));
    assert(error.failure==Pm4Failure::TruncatedPacket);
    std::vector<std::uint32_t> words;for(unsigned i=0;i<64;++i)words.push_back(word(i));
    NativeStateBatch state;
    assert(!planNativeStateBatch(words,state,error));
    std::printf("NATIVE_SWAP_COMMAND_RECORD: %u bit controls, truncation/identity checks, ordinary PM4 rejects; guest writes=0 GPU work=0 PASS\n",rejected);
}
