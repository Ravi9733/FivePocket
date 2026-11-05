#include "native_swap_gpu_execution.h"
#include "process_limits.h"
#include <chrono>
#include <stdexcept>
#include <string_view>
using namespace recomp::gpu;
int main(int argc,char** argv){
    if(!initializeProbeProcess() || argc>2)return 2;
    const auto mode=argc==2?std::string_view(argv[1]):std::string_view{};
    const bool fault=!mode.empty();
    if(fault && mode!="--post-accept-clock-fault" && mode!="--post-accept-clock-throw" &&
        mode!="--post-accept-backwards-clock" && mode!="--post-accept-wrap-clock")return 2;
    ProbeDevice context;if(!context.inspect() || !context.create({}))return 1;
    const auto command=NativeSwapCommand::make({17,0x2028,0x100000,37});if(!command)return 1;
    std::array<std::uint8_t,360> bytes{};
    const auto put=[&](unsigned i,unsigned v){for(unsigned b=0;b<4;++b)bytes[i*4+b]=v>>(24-b*8);};
    const std::array<unsigned,10> prefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003,0x8000,0xDEADBEEF};
    const std::array<unsigned,16> tail{0xC0025800,0x80000003,0x8004,0xDEADBEEF,0xC0003B00,0x7FFF,
        0xC0001000,0x77777777,0xC0025800,3,0x9002,0x12345678,0xC0025800,3,0xA002,0xABCDEF01};
    for(unsigned i=0;i<prefix.size();++i)put(i,prefix[i]);
    for(unsigned i=0;i<tail.size();++i)put(i+74,tail[i]);
    std::copy(command->bytes().begin(),command->bytes().end(),bytes.begin()+40);
    const auto plan=NativeSwapWrapper::parse(*command,0x2000,bytes.data(),bytes.size());if(!plan)return 1;
    const auto epoch=std::chrono::steady_clock::now();
    unsigned samples=0,presents=0;
    NativeCompletionClock clock=[&]()->std::optional<std::uint64_t>{
        if(fault){if(!samples++)return 1000;
            if(mode=="--post-accept-clock-throw")throw std::runtime_error("post-accept source fault");
            if(mode=="--post-accept-backwards-clock")return 999;
            if(mode=="--post-accept-wrap-clock")return 1000+8'589'934'592ULL;
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch).count();
    };
    NativeSwapGpuResult result;result.nativeFences=99;
    if(!fault){
        if(executeNativeSwapGpuWrapper(context,*plan,{},[&]{++presents;return true;},result) || result.nativeFences!=99 || presents)return 1;
        if(executeNativeSwapGpuWrapper(context,*plan,[]()->std::optional<std::uint64_t>{return std::nullopt;},
            [&]{++presents;return true;},result) || result.nativeFences!=99 || presents)return 1;
        if(executeNativeSwapGpuWrapper(context,*plan,clock,[&]{++presents;return false;},result) || result.nativeFences!=99 || presents!=1)return 1;
        if(executeNativeSwapGpuWrapper(context,*plan,clock,[&]()->bool{++presents;throw std::runtime_error("synthetic pre-accept refusal");},result) ||
            result.nativeFences!=99 || presents!=2)return 1;
    }
    // Mock callback acceptance only: this probe does NOT create a window or
    // submit a real frame. GPU event writes/fences are genuine and audited.
    if(!executeNativeSwapGpuWrapper(context,*plan,clock,[&]{++presents;return true;},result) || fault)return 1;
    const auto value=[&](unsigned i,bool little){std::uint32_t v=0;
        for(unsigned b=0;b<4;++b)v|=std::uint32_t{result.eventBytes[i*4+b]}<<(little?8*b:24-8*b);return v;};
    if(result.nativeFences!=4 || presents!=3 || result.afterCompletionNs<result.beforeCompletionNs ||
        value(0,true)!=static_cast<std::uint32_t>(result.beforeCompletionNs/2) ||
        value(1,true)!=static_cast<std::uint32_t>(result.afterCompletionNs/2) ||
        value(2,false)!=0x12345678 || value(3,false)!=0xABCDEF01)return 1;
    std::printf("NATIVE_SWAP_GPU_EVENTS: four_fences=1 guarded_gpu_audit=1 little_counter_words=2 big_literal_words=2 before_ns=%llu after_ns=%llu rejected_callbacks=2 mock_acceptances=1 actual_presentations=0 guest_writes=0 PASS\n",
        static_cast<unsigned long long>(result.beforeCompletionNs),static_cast<unsigned long long>(result.afterCompletionNs));
}
