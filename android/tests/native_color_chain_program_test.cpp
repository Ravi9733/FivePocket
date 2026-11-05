#include "native_color_chain_program.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    // Synthetic framing records, deliberately NOT claimed valid executable
    // SPIR-V. Full validator/ABI checks remain the offline producer's job.
    const auto module=[](unsigned model){return std::vector<std::uint32_t>{0x07230203,0x10300,0,16,0,
        (2u<<16)|17,1,(5u<<16)|15,model,1,0x6E69616D,0};};
    NativeColorChainProgram::Translations translations;
    NativeColorChainProgram::Identities active;
    for(unsigned i=0;i<5;++i){
        translations[i].guest.stage=i==2 || i==4?1:0;
        translations[i].guest.bigEndianCode=std::vector<std::uint8_t>(12,static_cast<std::uint8_t>(i+1));
        translations[i].nativeWords=module(translations[i].guest.stage?4:0);active[i]=translations[i].guest;
    }
    NativeColorChainProgram::Support support;for(auto& s:support)s=module(5);
    support[3].insert(support[3].end(),{(2u<<16)|17,50});
    auto program=NativeColorChainProgram::create(translations,support);assert(program && program->matches(active));
    assert(program->code()[0]==translations[2].nativeWords && program->code()[1]==translations[1].nativeWords &&
        program->code()[2]==translations[4].nativeWords && program->code()[3]==translations[3].nativeWords &&
        program->code()[4]==support[0] && program->code()[5]==support[1] && program->code()[6]==support[2] &&
        program->depthCode()[0]==translations[0].nativeWords && program->depthCode()[1]==support[3]);
    unsigned failures=0;
    assert(program->coordinates()==NativeColorCoordinateProfile::DiagnosticPositive);
    auto mapped=NativeColorChainProgram::create(translations,support,1,NativeColorCoordinateProfile::Padded1280W1);
    assert(mapped && mapped->matches(active) && mapped->coordinates()==NativeColorCoordinateProfile::Padded1280W1);
    for(unsigned value:{2u,UINT32_MAX}) {
        assert(!NativeColorChainProgram::create(translations,support,1,static_cast<NativeColorCoordinateProfile>(value)));++failures;
    }
    for(unsigned i=0;i<5;++i){
        auto bad=active;bad[i].stage^=1;assert(!program->matches(bad));++failures;
        bad=active;bad[i].bigEndianCode.clear();assert(!program->matches(bad));++failures;
        bad=active;bad[i].bigEndianCode.resize(4104);assert(!program->matches(bad));++failures;
        bad=active;bad[i].bigEndianCode.push_back(0);assert(!program->matches(bad));++failures;
        for(unsigned byte=0;byte<12;++byte)for(unsigned bit=0;bit<8;++bit){
            bad=active;bad[i].bigEndianCode[byte]^=1u<<bit;assert(!program->matches(bad));++failures;}
        auto stages=translations;stages[i].guest.stage^=1;assert(!NativeColorChainProgram::create(stages,support));++failures;
    }
    auto swapped=active;std::swap(swapped[1],swapped[3]);assert(!program->matches(swapped));++failures;
    for(unsigned abi:{0u,2u,UINT32_MAX}){assert(!NativeColorChainProgram::create(translations,support,abi));++failures;}
    for(unsigned role=0;role<9;++role)for(unsigned mutation=0;mutation<13;++mutation){
        auto stages=translations;auto helpers=support;auto& words=role<5?stages[role].nativeWords:helpers[role-5];
        if(mutation==0)words[0]^=1;
        if(mutation==1)words[1]=0x10400;
        if(mutation==2)words[3]=0;
        if(mutation==3)words[4]=1;
        if(mutation==4)words[5]=0;
        if(mutation==5)words[5]=0xFFFF0011;
        if(mutation==6)words[6]=11; // Int64 is unavailable on the tested Thor profile.
        if(mutation==7)words[8]=99;
        if(mutation==8)words[9]=16;
        if(mutation==9)words[10]^=1;
        if(mutation==10)words[11]=1;
        if(mutation==11)words.insert(words.end(),{(2u<<16)|17,1});
        if(mutation==12)words.insert(words.end(),{(2u<<16)|10,0});
        assert(!NativeColorChainProgram::create(stages,helpers));++failures;
    }
    const auto savedCode=program->code();const auto savedDepth=program->depthCode();
    translations[0].guest.bigEndianCode[0]^=1;translations[2].nativeWords[0]^=1;support[0][0]^=1;
    assert(program->matches(active) && program->code()==savedCode && program->depthCode()==savedDepth);
    std::printf("NATIVE_COLOR_CHAIN_PROGRAM_V1 roles=5 native_modules=9 rejection_controls=%u keys_owned=1 spirv_validated=0\n",failures);
}
