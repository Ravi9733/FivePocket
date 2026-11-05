// Two distinct captured stage/resource pairs sharing one explicit native target.
// No PM4 predicate, verified retail format12 or guest writeback.
#ifdef RECOMP_PAIR_NATIVE_DEPTH
#include "depth_chain_initialization.h"
#include <charconv>
#include <string_view>
#endif
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include "r8_plane_images.h"
#include "texture32_image.h"
#include "texture_fragment_target.h"
#include "native_color_chain_commands.h"
#include "native_color_chain_resources.h"
#ifdef RECOMP_PAIR_STORE_COPY
#include "xenos_color_copy.h"
#include "native_gpu_output_publication.h"
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
namespace {
#if defined(RECOMP_PAIR_NATIVE_DEPTH)
constexpr bool StoreCopy=true,Initialize=true,NativeDepth=true;
#elif defined(RECOMP_PAIR_EDRAM_INITIALIZE)
constexpr bool StoreCopy=true,Initialize=true,NativeDepth=false;
#elif defined(RECOMP_PAIR_STORE_COPY)
constexpr bool StoreCopy=true,Initialize=false,NativeDepth=false;
#else
constexpr bool StoreCopy=false,Initialize=false,NativeDepth=false;
#endif
using Profile=recomp::gpu::NativeColorChainProfile<StoreCopy,Initialize,NativeDepth>;
constexpr unsigned OutputBytes=Profile::OutputBytes,CodeCount=Profile::CodeCount;
#ifdef RECOMP_PAIR_STORE_COPY
constexpr unsigned BackingBytes=Profile::BackingBytes,CopyBytes=Profile::CopyBytes,EventBytes=Profile::EventBytes;
constexpr unsigned RecordBytes=16,ReferenceBytes=96;
#else
constexpr unsigned RecordBytes=8,ReferenceBytes=64;
#endif
struct Resources:recomp::gpu::NativeColorChainResources<StoreCopy,Initialize,NativeDepth> {
    using NativeColorChainResources::NativeColorChainResources;
#if defined(RECOMP_PAIR_EDRAM_INITIALIZE) && !defined(RECOMP_PAIR_NATIVE_DEPTH)
    std::vector<std::uint8_t> initialEdram; // Fixture-only CPU initialization.
#endif
};
bool check(recomp::gpu::ProbeDevice& device,Resources& r,const std::array<std::vector<std::uint8_t>,2>& inputs,
        const std::vector<std::uint8_t>& expected,unsigned step){
    for(unsigned i=0;i<2;++i)std::memcpy(r.mapped[i],inputs[i].data(),inputs[i].size());
    std::memset(r.mapped[2],0x5A,OutputBytes);
    recomp::gpu::NativeWordUpdatePlan eventPlan;
#ifdef RECOMP_PAIR_STORE_COPY
    std::memset(r.mapped[3],0x5A,BackingBytes);std::memset(r.mapped[4],0x5A,CopyBytes);
    std::memset(r.mapped[5],0x5A,EventBytes);
    // Synthetic diagnostic markers, NOT the stopped title's event semantics.
    // These execute after native graphics/copy and share its actual fence.
    const recomp::gpu::NativeWordRequest events[]={{16,0x12345670+step,recomp::gpu::NativeWordPhase::AfterGraphics},
        {20,0xA1B2C300+step,recomp::gpu::NativeWordPhase::AfterGraphics}};
    if(!recomp::gpu::planNativeWordUpdates(events,2,EventBytes,eventPlan))return false;
#if defined(RECOMP_PAIR_EDRAM_INITIALIZE) && !defined(RECOMP_PAIR_NATIVE_DEPTH)
    if(step==0)std::memcpy(r.mapped[3],r.initialEdram.data(),BackingBytes);
#endif
#endif
    // Invalid stage/marker plans must reject before touching the command pool.
    if(recomp::gpu::recordNativeColorChainStage<StoreCopy,Initialize,NativeDepth>(r,4,eventPlan))return false;
#ifdef RECOMP_PAIR_STORE_COPY
    auto invalidEvents=eventPlan;invalidEvents.count=65;
    if(recomp::gpu::recordNativeColorChainStage<StoreCopy,Initialize,NativeDepth>(r,step,invalidEvents))return false;
    invalidEvents=eventPlan;invalidEvents.words[1].byteOffset=16;
    if(recomp::gpu::recordNativeColorChainStage<StoreCopy,Initialize,NativeDepth>(r,step,invalidEvents))return false;
#endif
    if(!recomp::gpu::recordNativeColorChainStage<StoreCopy,Initialize,NativeDepth>(r,step,eventPlan))return false;
    const auto* output=static_cast<const std::uint8_t*>(r.mapped[2]);
    for(unsigned i=0;i<OutputBytes;++i)if(output[i]!=0x5A)return false;
#ifdef RECOMP_PAIR_STORE_COPY
    for(unsigned i=0;i<EventBytes;++i)if(static_cast<const std::uint8_t*>(r.mapped[5])[i]!=0x5A)return false;
    for(unsigned b=3;b<5;++b)for(unsigned i=0;i<(b==3?BackingBytes:CopyBytes);++i){
        std::uint8_t initial=0x5A;
#if defined(RECOMP_PAIR_EDRAM_INITIALIZE) && !defined(RECOMP_PAIR_NATIVE_DEPTH)
        if(step==0 && b==3)initial=r.initialEdram[i];
#endif
        if(static_cast<const std::uint8_t*>(r.mapped[b])[i]!=initial)return false;
    }
#endif
    if(vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
        vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
    r.planes.ready=true;r.imageReady=true;
    bool okay=true;
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    okay&=r.depth.unchanged();
#endif
    for(unsigned i=0;i<2;++i)okay&=std::memcmp(r.mapped[i],inputs[i].data(),inputs[i].size())==0;
    for(unsigned i=0;i<OutputBytes;++i){
        const auto want=i<16 || i>=OutputBytes-16?0x5A:expected[16+step*RecordBytes+(i-16)%8];
        if(output[i]!=want){okay=false;std::printf("COLOR_PAIR_SEQUENCE_MISMATCH step=%u byte=%u actual=%02x expected=%02x\n",step,i,output[i],want);
            // One bounded pixel, not a raw image dump. Preserve exact channel
            // evidence for cross-driver storage/blend precision experiments.
            if(i>=16 && i<OutputBytes-16){
                const auto base=16+(i-16)/8*8;
                std::printf("COLOR_PAIR_HALF_WORDS step=%u pixel=%u actual=",step,(base-16)/8);
                for(unsigned c=0;c<4;++c)std::printf("%s%04x",c?",":"",unsigned(output[base+c*2])|(unsigned(output[base+c*2+1])<<8));
                std::printf(" expected=");
                for(unsigned c=0;c<4;++c){const auto at=16+step*RecordBytes+c*2;
                    std::printf("%s%04x",c?",":"",unsigned(expected[at])|(unsigned(expected[at+1])<<8));}
                std::printf("\n");
            }
            break;}
    }
#ifdef RECOMP_PAIR_STORE_COPY
    std::vector<std::uint8_t> matchBacking(BackingBytes,0x5A),matchCopy(CopyBytes,0x5A);
    const auto* record=expected.data()+16+step*RecordBytes;
    for(unsigned i=0;i<921600;++i)std::memcpy(matchBacking.data()+16+3686400+i*4,record+8,4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        std::uint32_t at=0;if(!recomp::gpu::colorCopyOffset(1280,720,x,y,at))return false;
        std::memcpy(matchCopy.data()+16+at,record+12,4);
    }
    for(unsigned b=3;b<5;++b){
        const auto& match=b==3?matchBacking:matchCopy;const auto* actual=static_cast<const std::uint8_t*>(r.mapped[b]);
        if(std::memcmp(actual,match.data(),match.size())!=0){okay=false;
            for(unsigned i=0;i<match.size();++i)if(actual[i]!=match[i]){
                std::printf("COLOR_PAIR_STORE_COPY_MISMATCH step=%u buffer=%u byte=%u actual=%02x expected=%02x\n",step,b,i,actual[i],match[i]);break;}
        }
    }
    std::printf("COLOR_PAIR_STORE_COPY_CHECK step=%u backing=%u tiled=%u padding_unchanged=81920\n",step,BackingBytes,CopyBytes);
    std::array<std::uint8_t,EventBytes> expectedEvents;expectedEvents.fill(0x5A);
    for(std::size_t i=0;i<eventPlan.count;++i){const auto& w=eventPlan.words[i];
        std::memcpy(expectedEvents.data()+w.byteOffset,w.bigEndianBytes.data(),4);}
    const auto* eventReadback=static_cast<const std::uint8_t*>(r.mapped[5]);
    okay&=std::memcmp(eventReadback,expectedEvents.data(),EventBytes)==0;
    if(okay){
        // Future guest publication plumbing, exercised ONLY against an owned
        // scratch mirror here. No guest pointer or live allocation lease.
        recomp::gpu::ColorCopyPlan copyPlan;
        if(!recomp::gpu::planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,0x1F2D8000,0x2D00500,0x1000300,copyPlan))return false;
        auto snapshot=recomp::gpu::NativeTiledCopyPublication::capture(copyPlan,
            static_cast<const std::uint8_t*>(r.mapped[4]),CopyBytes);
        if(!snapshot)return false;
        std::vector<std::uint8_t> mirror(CopyBytes);
        for(std::size_t i=0;i<mirror.size();++i)mirror[i]=static_cast<std::uint8_t>(i%251);
        auto expectedMirror=mirror;
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
            std::uint32_t at=0;if(!recomp::gpu::colorCopyOffset(1280,720,x,y,at))return false;
            std::memcpy(expectedMirror.data()+16+at,record+12,4);
        }
        const auto beforeMirror=mirror;
        std::array<std::uint8_t,12> outputWords;outputWords.fill(0xCD);
        const auto beforeWords=outputWords;
        std::array<std::uint8_t,4> cursor{{0,0,0,0}};
        const std::array<recomp::gpu::NativeOutputWord,2> destinations{{
            {20,{0x1000,outputWords.data(),4}},{16,{0x2000,outputWords.data()+4,4}}}};
        const recomp::gpu::NativeOutputSpan readPointer{0x3000,cursor.data(),4};
        const recomp::gpu::NativeOutputSpan copyTarget{copyPlan.destinationBase,mirror.data()+16,copyPlan.capacity};
        // Late event corruption and physical aliasing must not partially publish
        // an otherwise valid native image. These are CPU audit failure controls,
        // not simulated GPU-fence successes or injected physical device losses.
        auto badEvents=expectedEvents;badEvents[23]^=1;
        if(recomp::gpu::publishNativeGpuOutputs(eventPlan,badEvents.data(),EventBytes,destinations.data(),2,
                readPointer,0,1,&*snapshot,copyTarget))return false;
        auto aliases=destinations;aliases[1].target.physicalAddress=copyPlan.destinationBase;
        if(recomp::gpu::publishNativeGpuOutputs(eventPlan,eventReadback,EventBytes,aliases.data(),2,
                readPointer,0,1,&*snapshot,copyTarget))return false;
        if(mirror!=beforeMirror || outputWords!=beforeWords || cursor!=std::array<std::uint8_t,4>{0,0,0,0})return false;
        okay=recomp::gpu::publishNativeGpuOutputs(eventPlan,eventReadback,EventBytes,destinations.data(),2,
            readPointer,0,1,&*snapshot,copyTarget) && mirror==expectedMirror &&
            std::memcmp(outputWords.data(),eventReadback+20,4)==0 && std::memcmp(outputWords.data()+4,eventReadback+16,4)==0 &&
            std::memcmp(outputWords.data()+8,beforeWords.data()+8,4)==0 && cursor==std::array<std::uint8_t,4>{0,0,0,1} &&
            std::memcmp(r.mapped[4],matchCopy.data(),CopyBytes)==0 && std::memcmp(eventReadback,expectedEvents.data(),EventBytes)==0;
        std::printf("COLOR_PAIR_PUBLICATION_CHECK step=%u matched=%u active_bytes=3686400 padding_preserved=81920 guest_writeback=0\n",step,unsigned(okay));
        std::printf("COLOR_PAIR_OUTPUT_TRANSACTION step=%u matched=%u native_event_words=2 failure_controls=2 scratch_cursor=1 guest_writeback=0\n",step,unsigned(okay));
    }
#endif
    std::printf("COLOR_PAIR_SEQUENCE_CASE step=%u fence=1 matched=%u checked_output=%u checked_inputs=%zu guest_draws=0\n",
        step,unsigned(okay),OutputBytes,inputs[0].size()+inputs[1].size());return okay;
}
}
int main(int argc,char** argv){
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    constexpr int RequiredArgc=14;
#else
    constexpr int RequiredArgc=CodeCount==7?12:(CodeCount==6?10:8);
#endif
    if((argc!=RequiredArgc && argc!=RequiredArgc+1) || !recomp::gpu::initializeProbeProcess())return 2;
    const char* flag=argc==RequiredArgc+1?argv[RequiredArgc]:"";
    bool corrupt=false,clear=false,blend=false,reverse=false,reuse=false,skip=false,zero=false;
#ifdef RECOMP_PAIR_STORE_COPY
    bool skipStore=false,skipCopy=false;
#endif
#ifdef RECOMP_PAIR_EDRAM_INITIALIZE
    bool zeroInitial=false;
#endif
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    bool skipDepth=false,skipDepthStore=false,noDepthWrite=false,noStencilWrite=false,wrongStencil=false;
    unsigned depthMask=15;
#endif
    if(*flag){
        if(!std::strcmp(flag,"--corrupt-expected-control"))corrupt=true;
        else if(!std::strcmp(flag,"--clear-every-pass-control"))clear=true;
        else if(!std::strcmp(flag,"--disable-alpha-blend-control"))blend=true;
        else if(!std::strcmp(flag,"--reverse-pairs-control"))reverse=true;
        else if(!std::strcmp(flag,"--reuse-first-pair-control"))reuse=true;
        else if(!std::strcmp(flag,"--skip-second-control"))skip=true;
        else if(!std::strcmp(flag,"--zero-small-texture-control"))zero=true;
#ifdef RECOMP_PAIR_STORE_COPY
        else if(!std::strcmp(flag,"--skip-half-store-control"))skipStore=true;
        else if(!std::strcmp(flag,"--skip-tiled-copy-control"))skipCopy=true;
#endif
#ifdef RECOMP_PAIR_EDRAM_INITIALIZE
        else if(!std::strcmp(flag,"--skip-initial-unpack-control"))zeroInitial=true;
#endif
#ifdef RECOMP_PAIR_NATIVE_DEPTH
        else if(!std::strcmp(flag,"--skip-depth-draw-control"))skipDepth=true;
        else if(!std::strcmp(flag,"--skip-depth-store-control"))skipDepthStore=true;
        else if(!std::strcmp(flag,"--disable-depth-write-control"))noDepthWrite=true;
        else if(!std::strcmp(flag,"--disable-stencil-write-control"))noStencilWrite=true;
        else if(!std::strcmp(flag,"--wrong-stencil-reference-control"))wrongStencil=true;
        else if(std::string_view(flag).substr(0,13)=="--depth-mask="){
            const std::string_view value(flag+13);
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),depthMask);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || depthMask>15)return 2;
        }
#endif
        else return 2;
    }
    std::array<std::vector<std::uint32_t>,CodeCount> code;
    const std::array<unsigned,CodeCount> args{1,2,4,5
#ifdef RECOMP_PAIR_STORE_COPY
        ,8,9
#endif
#ifdef RECOMP_PAIR_EDRAM_INITIALIZE
        ,10
#endif
    };
    for(unsigned i=0;i<CodeCount;++i){std::vector<std::uint8_t> bytes;
        if(!recomp::gpu::readInput(argv[args[i]],bytes,65536) || bytes.size()<20 || bytes.size()%4)return 2;
        code[i].resize(bytes.size()/4);std::memcpy(code[i].data(),bytes.data(),bytes.size());
        if(code[i][0]!=0x07230203)return 2;
    }
    std::array<std::vector<std::uint8_t>,2> inputs;std::vector<std::uint8_t> expected;
    if(!recomp::gpu::readInput(argv[3],inputs[0],1382912) || inputs[0].size()<1382400 || inputs[0].size()%4 ||
       !recomp::gpu::readInput(argv[6],inputs[1],4608) || inputs[1].size()<4096 || inputs[1].size()%4 ||
       !recomp::gpu::readInput(argv[7],expected,ReferenceBytes) || expected.size()!=ReferenceBytes)return 2;
    for(unsigned i=0;i<16;++i)if(expected[i]!=0x5A || expected[ReferenceBytes-16+i]!=0x5A)return 2;
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    std::array<std::vector<std::uint32_t>,2> depthCode;std::vector<std::uint8_t> depthSource;
    for(unsigned i=0;i<2;++i){std::vector<std::uint8_t> bytes;
        if(!recomp::gpu::readInput(argv[11+i],bytes,65536) || bytes.size()<20 || bytes.size()%4)return 2;
        depthCode[i].resize(bytes.size()/4);std::memcpy(depthCode[i].data(),bytes.data(),bytes.size());
        if(depthCode[i][0]!=0x07230203)return 2;
    }
    if(!recomp::gpu::readInput(argv[13],depthSource,4096) || depthSource.empty() || depthSource.size()%4)return 2;
#elif defined(RECOMP_PAIR_EDRAM_INITIALIZE)
    std::vector<std::uint8_t> initialEdram;
    if(!recomp::gpu::readInput(argv[11],initialEdram,BackingBytes) || initialEdram.size()!=BackingBytes)return 2;
    for(unsigned i=0;i<16;++i)if(initialEdram[i]!=0x5A || initialEdram[BackingBytes-16+i]!=0x5A)return 2;
#endif
    if(corrupt)expected[16+RecordBytes]^=1;
    recomp::gpu::ProbeDevice device;if(!device.inspect() || !device.create({}))return 1;
    Resources r(device.device);r.forceClear=clear;r.disableBlend=blend;r.reversePairs=reverse;r.reuseFirst=reuse;
#ifdef RECOMP_PAIR_EDRAM_INITIALIZE
#ifndef RECOMP_PAIR_NATIVE_DEPTH
    r.initialEdram=std::move(initialEdram);
#endif
    r.zeroInitial=zeroInitial;
#endif
    r.skipSecond=skip;r.replaceImageControl=zero;
#ifdef RECOMP_PAIR_STORE_COPY
    r.skipStore=skipStore;r.skipCopy=skipCopy;
#endif
    Resources::DepthCode* depthModules=nullptr;
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    r.depth.source=std::move(depthSource);r.depth.skipDraw=skipDepth;r.depth.skipStore=skipDepthStore;
    r.depth.depthWrite=!noDepthWrite;r.depth.stencilWrite=!noStencilWrite;
    r.depth.stencilReference=wrongStencil?1:0;r.depth.sampleMask=depthMask;
    depthModules=&depthCode;
#endif
    const Profile::BufferSizes sizes{unsigned(inputs[0].size()),unsigned(inputs[1].size()),OutputBytes
#ifdef RECOMP_PAIR_STORE_COPY
        ,BackingBytes,CopyBytes,EventBytes
#endif
    };
    // Invalid setup must neither allocate resources nor permit retry. A valid
    // owner also rejects repeated setup without replacing any live handles.
    {Resources bad(device.device);auto invalid=sizes;invalid[2]-=4;
        if(bad.setup(device,code,invalid,depthModules) || bad.configured() || bad.buffers[0] || bad.pool ||
            bad.setup(device,code,sizes,depthModules))return 1;}
    if(!r.setup(device,code,sizes,depthModules) || !r.configured())return 1;
    const auto originalBuffers=r.buffers;const auto originalPool=r.pool;const auto originalFence=r.fence;
    if(r.setup(device,code,sizes,depthModules) || r.buffers!=originalBuffers || r.pool!=originalPool || r.fence!=originalFence)return 1;
    for(unsigned step=0;step<4;++step)if(!check(device,r,inputs,expected,step))return 1;
    std::puts("COLOR_PAIR_SEQUENCE_V1 distinct_pairs=2 shared_target=1 fresh_fences=4 native_images=4 retail_state_replayed=0 game_frame=0 guest_writeback=0");
#ifdef RECOMP_PAIR_STORE_COPY
    std::puts("COLOR_PAIR_STORE_COPY_V1 native_image_to_half_to_edram_to_tiled=1 checked_output=21626976 guest_writeback=0");
#endif
#ifdef RECOMP_PAIR_NATIVE_DEPTH
    std::puts("COLOR_PAIR_NATIVE_DEPTH_V1 native_depth_to_edram_to_half_to_image=1 cpu_supplied_initial_edram=0 live_depth_connected=1 retail_state_replayed=0");
#elif defined(RECOMP_PAIR_EDRAM_INITIALIZE)
    std::puts("COLOR_PAIR_EDRAM_INITIALIZE_V1 native_edram_to_half_to_image=1 cpu_supplied_initial_edram=1 live_depth_connected=0");
#endif
    return 0;
}
