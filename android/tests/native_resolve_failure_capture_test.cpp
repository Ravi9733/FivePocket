#include "native_resolve_failure_capture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace recomp::gpu;
int main() {
    using Bytes=std::vector<std::uint8_t>;
    using Stage=NativeResolveFailureStage;
    unsigned calls=0,captures=0;Stage last=Stage::Exception;
    const auto capture=[&](Stage s){++captures;last=s;};
    const auto success=[&]{++calls;return true;};
    assert(!observeNativeResolveAttempt(false,false,success,capture));
    assert(calls==0 && captures==1 && last==Stage::DisplayMissing);
    assert(!observeNativeResolveAttempt(true,false,success,capture));
    assert(calls==0 && captures==2 && last==Stage::DisplayInvalid);
    assert(observeNativeResolveAttempt(true,true,success,capture));
    assert(calls==1 && captures==2);
    assert(!observeNativeResolveAttempt(true,true,[&]{++calls;return false;},capture));
    assert(calls==2 && captures==3 && last==Stage::ReturnedFalse);
    struct OriginalFailure {};
    bool rethrown=false;
    try {observeNativeResolveAttempt(true,true,[]()->bool{throw OriginalFailure{};},
        [&](Stage s){capture(s);throw std::runtime_error("capture failed");});}
    catch(const OriginalFailure&){rethrown=true;}
    assert(rethrown && captures==4 && last==Stage::Exception);
    assert(!observeNativeResolveAttempt(true,false,success,[](Stage){throw OriginalFailure{};}));
    assert(calls==2);

    constexpr unsigned Device=0x10040000,Destination=0x10200000,Surface=0x10300000,Stack=0x64010000;
    std::map<unsigned,Bytes> memory;
    memory[Device]=Bytes(13860);memory[Destination]=Bytes(52,0x42);memory[Surface]=Bytes(40,0x31);
    memory[Stack+92]=Bytes{0,0,0,17};memory[Stack+100]=Bytes{0,0,0,23};
    memory[Stack+256]=Bytes(16,0x55);memory[Stack+288]=Bytes(8,0x66);memory[Stack+320]=Bytes(16,0x77);
    for(unsigned i=0;i<4;++i)memory[Device][12816+i]=Surface>>(24-8*i);
    const auto before=memory;unsigned reads=0;
    const Gta5ResourceReader reader=[&](unsigned a,unsigned n)->std::optional<Bytes> {
        ++reads;const auto it=memory.find(a);if(it==memory.end() || it->second.size()!=n)return std::nullopt;
        return it->second;
    };
    const std::array<unsigned,9> args{Device,0x123,Stack+256,Destination,Stack+288,7,5,Stack+320,0x823C0100};
    NativeResolveFailureCapture c;c.stage=Stage::DisplayInvalid;c.displayFound=true;c.ownsPresentation=true;
    c.boundary=captureGta5TileResolveBoundary(Stack,args,0x3FF0000000000000ull,reader,reader,reader,reader);
    assert(c.boundary.arguments==args && c.boundary.f1==0x3FF0000000000000ull);
    assert(c.boundary.state==memory.at(Device) && c.boundary.destinationHeader==memory.at(Destination));
    assert(c.boundary.surfaces && c.boundary.surfaces->surfaces[0].header==memory.at(Surface));
    assert(c.boundary.pointed.rectangle==memory.at(Stack+256) && c.boundary.pointed.point==memory.at(Stack+288));
    assert(c.boundary.pointed.color==memory.at(Stack+320));
    assert(c.boundary.pointed.stack92==memory.at(Stack+92) && c.boundary.pointed.stack100==memory.at(Stack+100));
    assert(!c.boundary.allocatorTail && !c.boundary.resolveGlobals && memory==before && reads>0);
    std::map<std::string,Bytes> files;
    const auto save=[&](const std::string& name,const Bytes& b){return files.emplace(name,b).second;};
    std::ostringstream out;assert(writeNativeResolveFailureCapture(c,out,save));
    const auto text=out.str();assert(text.find("display-owner-invalid")!=std::string::npos);
    assert(text.find("\"destination_storage_captured\":false")!=std::string::npos);
    assert(text.find("\"allocator-tail\":null")!=std::string::npos);
    assert(files.at("native-resolve-failure-destination-header.bin")==memory.at(Destination));
    std::ostringstream unavailable;NativeResolveFailureCapture empty;
    assert(writeNativeResolveFailureCapture(empty,unavailable,[](const auto&,const auto&){assert(false);return false;}));
    auto overflow=c;overflow.boundary.state=Bytes(65537);std::ostringstream rejected;
    assert(!writeNativeResolveFailureCapture(overflow,rejected,[](const auto&,const auto&){return true;}));
    std::ostringstream failedSave;assert(!writeNativeResolveFailureCapture(c,failedSave,[](const auto&,const auto&){return false;}));
    assert(memory==before);
    std::puts("NATIVE_RESOLVE_FAILURE_CAPTURE_PASS stages=4 original_exception_preserved=1 read_only_abi=1 null_and_budget=1 success_no_capture=1");
}
