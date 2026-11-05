// Diagnostic raw-binding transport and oracle OUTSIDE native execution.
// Private fixture: 225 BE metadata words with fixed counts, seven source files.
// No game-derived payloads or modules are embedded in this program.
#include "native_color_chain_execution.h"
#include "native_gpu_output_publication.h"
#include "native_color_chain_events.h"
#include "native_color_copy_request.h"
#include "native_color_chain_sync.h"
#include "native_color_batch_capture.h"
#include "native_color_resolved_memory.h"
#include "native_color_output_publication.h"
#include "native_color_scratch_memory.h"
#include "private_input.h"
#include "process_limits.h"
#include <string>
using namespace recomp::gpu;
namespace {
bool bindings(const std::string& root,NativeColorChainBindings& input,
        std::array<std::vector<std::uint8_t>,7>& source){
    std::vector<std::uint8_t> metadata;
    if(!readInput((root+"/metadata.bin").c_str(),metadata,900) || metadata.size()!=900)return false;
    for(unsigned i=0;i<7;++i)
        if(!readInput((root+"/source-"+std::to_string(i)+".bin").c_str(),source[i],1048576))return false;
    std::size_t cursor=0;
    const auto word=[&](){const auto* p=metadata.data()+cursor;cursor+=4;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    for(unsigned i=0;i<3;++i){auto& v=i?input.pairs[i-1].vertices:input.depth;
        v.slot=word();for(auto& w:v.descriptor){w.bits=word();w.knownMask=word();}
        v.source.physicalAddress=word();v.source.size=word();v.source.bytes=source[i].data();
        if(v.source.size!=source[i].size())return false;
    }
    for(unsigned i=0;i<4;++i){NativeColorTextureBinding t;t.slot=word();
        for(auto& w:t.descriptor){w.bits=word();w.knownMask=word();}
        t.source.physicalAddress=word();t.source.size=word();t.source.bytes=source[i+3].data();
        if(t.source.size!=source[i+3].size())return false;input.pairs[i==3?1:0].textures.push_back(t);
    }
    for(unsigned i=0;i<4;++i){auto& p=input.pairs[i/2];auto& constants=i%2?p.pixelConstants:p.vertexConstants;
        for(unsigned c=0;c<(i<2?5u:(i==2?4u:2u));++c){NativeColorConstant value;value.index=word();
            for(auto& w:value.words){w.bits=word();w.knownMask=word();}constants.push_back(value);}
    }
    return cursor==metadata.size();
}
bool module(const std::string& path,std::vector<std::uint32_t>& output){
    std::vector<std::uint8_t> bytes;if(!readInput(path.c_str(),bytes,65536) || bytes.size()<20 || bytes.size()%4)return false;
    output.resize(bytes.size()/4);std::memcpy(output.data(),bytes.data(),bytes.size());return true;
}
// Reconstruct independent SCRATCH mappings, never live guest allocations. This
// exercises snapshot/host-alias preflight after the transport owner is gone.
bool checkBatchMemory(const NativeColorBatchCapture& capture){
    const auto& request=capture.memory();const auto& controls=capture.controls();
    auto scratch=NativeColorScratchMemory::create(capture);if(!scratch)return false;
    const auto& memory=scratch->memory();
    unsigned rejects=0;
    const auto changed=[&](std::uint8_t* p){*p^=1;const bool rejected=!matchesNativeColorBatchMemory(capture,memory);*p^=1;
        if(rejected)++rejects;return rejected;};
    for(unsigned i=0;i<request.inputCount;++i)if(!changed(memory.inputs[i].bytes()) ||
        !changed(memory.inputs[i].bytes()+memory.inputs[i].range.bytes-1))return false;
    if(!changed(memory.controls[1].bytes()) || !changed(memory.controls[1].bytes()+controls.child.bytes-1))return false;
    for(unsigned i=0;i<3;++i)if(!changed(memory.controls[0].bytes()+(controls.previousWord+i)*4))return false;
    for(unsigned i=0;i<4;++i)if(!changed(memory.controls[2].bytes()+i))return false;
    if(!matchesNativeColorBatchMemory(capture,memory))return false;
    // Publication destinations and identifier payload must be untouched.
    for(unsigned i=0;i<3;++i)if(!std::all_of(memory.outputs[i].bytes(),memory.outputs[i].bytes()+memory.outputs[i].range.bytes,
        [](auto b){return b==0x5A;}))return false;
    for(unsigned i=0;i<4;++i)if(memory.controls[3].bytes()[i]!=0x5A)return false;
    std::printf("NATIVE_BATCH_MEMORY_SCRATCH snapshot_match=1 stale_rejections=%u guest_owner=0 guest_writes=0\n",rejects);
    return true;
}
// Optional diagnostic transport: raw child plus up to sixteen independent
// physical source slices. Never a guest allocation resolver or live admission.
std::optional<NativeColorChainCapture> commandCapture(const std::string& root,const NativeColorChainProgram& program,
        NativeColorChainEventRequests& finalEvents,NativeColorCopyRequest& copyRequest,std::optional<NativeColorBatchCapture>& batchCapture){
    using Bytes=std::vector<std::uint8_t>;
    Bytes child,bins,table;
    if(!readInput((root+"/child.bin").c_str(),child,16384) ||
       !readInput((root+"/bins.bin").c_str(),bins,20) || bins.size()!=20 ||
       !readInput((root+"/sources.bin").c_str(),table,132) || table.size()<4)return std::nullopt;
    const auto word=[](const Bytes& b,std::size_t i){return (std::uint32_t{b[i]}<<24)|(std::uint32_t{b[i+1]}<<16)|
        (std::uint32_t{b[i+2]}<<8)|b[i+3];};
    const auto count=word(table,0);
    if(!count || count>16 || table.size()!=4+count*8)return std::nullopt;
    std::vector<Bytes> sources(count);std::size_t total=0;
    for(unsigned i=0;i<count;++i){const auto address=word(table,4+i*8),size=word(table,8+i*8);
        if((address&3) || !size || (size&3) || size>2*1024*1024-total ||
           std::uint64_t{address}+size>0x20000000)return std::nullopt;
        for(unsigned j=0;j<i;++j){const auto other=word(table,4+j*8),extent=word(table,8+j*8);
            if(address<std::uint64_t{other}+extent && other<std::uint64_t{address}+size)return std::nullopt;}
        if(!readInput((root+"/source-"+std::to_string(i)+".bin").c_str(),sources[i],size) || sources[i].size()!=size)return std::nullopt;
        total+=size;
    }
    const Pm4PhysicalReader reader=[&](auto address,auto size)->std::optional<Bytes>{
        for(unsigned i=0;i<count;++i)if(address==word(table,4+i*8) && size==sources[i].size())return sources[i];
        return std::nullopt;
    };
    DrawBinState initial;initial.knownWords=word(bins,0);for(unsigned i=0;i<4;++i)initial.words[i]=word(bins,4+i*4);
    Pm4Snapshot snapshot;Pm4Error decode;DrawCommandPlan plan;DrawCommandError error;
    if(!decodePm4(child.data(),child.size(),{},snapshot,decode) ||
       !planDrawCommandCandidates(snapshot,initial,reader,plan,error) || plan.draws.size()!=4 ||
       !planNativeColorChainFinalEvents(plan,finalEvents) || !planNativeColorCopyRequest(plan,3,reader,copyRequest))return std::nullopt;
    auto capture=NativeColorChainCapture::create(plan,{0,1,2},program,reader);
    if(capture && program.coordinates()==NativeColorCoordinateProfile::Padded1280W1){
        NativeColorChainSyncRequests sync;
        if(!planNativeColorChainSync(plan,sync))return std::nullopt;
        std::puts("NATIVE_COMMAND_SYNC depth_wait=1 idleclean_waits=3 invalidations=3 cache_flush=1 effects_executed=0");
        Bytes rawControls;if(!readInput((root+"/memory-controls.bin").c_str(),rawControls,52) || rawControls.size()!=52)return std::nullopt;
        NativeColorMemoryControls controls;
        std::array<NativePhysicalRange*,4> spans{{&controls.ring,&controls.child,&controls.readPointer,&controls.identifier}};
        for(unsigned i=0;i<4;++i)*spans[i]={word(rawControls,i*8),word(rawControls,i*8+4)};
        controls.previousWord=word(rawControls,32);controls.nextWord=word(rawControls,36);
        for(unsigned i=0;i<3;++i)controls.rootWords[i]=word(rawControls,40+i*4);
        NativeColorMemoryRequest memory;
        if(!planNativeColorMemory(capture->sourceRanges(),plan.commands.words().size(),copyRequest,finalEvents,controls,memory))return std::nullopt;
        unsigned rejections=0;
        for(unsigned i=0;i<memory.inputCount;++i){
            auto badEvents=finalEvents;badEvents.physicalAddresses[0]=memory.inputs[i].address;
            auto sentinel=memory;sentinel.inputCount=73;
            if(planNativeColorMemory(capture->sourceRanges(),plan.commands.words().size(),copyRequest,badEvents,controls,sentinel) || sentinel.inputCount!=73)return std::nullopt;
            ++rejections;
        }
        std::printf("NATIVE_COMMAND_MEMORY inputs=%u physical_shaders=%u constants=%u controls=4 outputs=4 alias_rejections=%u lifecycle_lease=0 guest_writes=0\n",
            memory.inputCount,capture->sourceRanges().shaderRanges,capture->sourceRanges().constantRanges,rejections);
        // Independently build the one-owner frontend from the original child.
        // Compare it against the manually assembled path before executing either.
        auto batch=NativeColorBatchCapture::create(snapshot,initial,program,controls,reader);
        if(!batch)return std::nullopt;
        auto moved=std::move(*batch);NativeColorChainBindings movedFrom;
        if(batch->color().bindings(movedFrom))return std::nullopt;
        // Retire the original external transport before touching the new owner.
        sources.clear();child.clear();bins.clear();table.clear();snapshot={};
        PackedNativeColorChainInputs oneOwner,manual;
        if(!moved.color().pack(oneOwner) || !capture->pack(manual) || oneOwner.depth!=manual.depth || oneOwner.pairs!=manual.pairs ||
            moved.commands().commands.words()!=plan.commands.words() || moved.commands().draws.size()!=4 ||
            moved.copy().vertexBytes!=copyRequest.vertexBytes || moved.copy().copy.destinationBase!=copyRequest.copy.destinationBase ||
            moved.events().physicalAddresses!=finalEvents.physicalAddresses || moved.synchronization().depthWaitOffset!=sync.depthWaitOffset ||
            moved.memory().inputCount!=memory.inputCount || moved.controls().rootWords!=controls.rootWords)return std::nullopt;
        std::puts("NATIVE_BATCH_CAPTURE native_frontend_match=1 move_lifetime=1 source_transport_destroyed=1 effects_executed=0");
        if(!checkBatchMemory(moved))return std::nullopt;
        batchCapture.emplace(std::move(moved));
    }
    if(capture)std::printf("NATIVE_COMMAND_CANDIDATES packets=%zu shader_loads=%zu draw_candidates=%zu wait_candidates=%zu guest_commands_executed=0\n",
        plan.actions.size(),plan.shaders.size(),plan.draws.size(),plan.waits.size());
    return capture;
}
}
int main(int argc,char** argv){
    if(argc<4 || argc>10 || !initializeProbeProcess())return 2;
    bool empty=false,corrupt=false,literal=false,mappedViewport=false,deferred=false;std::string commands;
    int failAudit=-1;
    for(int i=4;i<argc;++i){const std::string flag=argv[i];
        if(flag=="--empty-events" && !empty && !corrupt)empty=true;
        else if(flag=="--corrupt-expected-control" && !empty && !corrupt)corrupt=true;
        else if(flag=="--literal-events" && !literal)literal=true;
        else if(flag=="--mapped-viewport" && !mappedViewport)mappedViewport=true;
        else if(flag=="--deferred-prefix-audit" && !deferred)deferred=true;
        else if(flag.rfind("--fail-audit-stage=",0)==0 && failAudit<0){
            const auto value=flag.substr(std::string("--fail-audit-stage=").size());
            if(value.size()!=1 || value[0]<'0' || value[0]>'3')return 2;
            failAudit=value[0]-'0';
        }
        else if(flag.rfind("--command-candidates=",0)==0 && commands.empty())commands=flag.substr(21);
        else return 2;
        if(flag.rfind("--command-candidates=",0)==0 && commands.empty())return 2;
    }
    if(literal && (empty || commands.empty()))return 2;
    if(mappedViewport && commands.empty())return 2;
    if(failAudit>=0 && (!mappedViewport || !literal || empty || corrupt))return 2;
    if(deferred && (!mappedViewport || !literal || empty || corrupt))return 2;
    NativeColorChainBindings input;std::array<std::vector<std::uint8_t>,7> source;
    if(!bindings(argv[1],input,source))return 2;
    NativeColorChainResources<true,true,true>::Code code;
    NativeColorChainResources<true,true,true>::DepthCode depth;
    const std::string package=argv[2];
    const std::array<const char*,7> names{"r8/fragment","r8/vertex","small/fragment","small/vertex","store","copy","load"};
    for(unsigned i=0;i<7;++i)if(!module(package+"/"+names[i]+"/dataflow.spv",code[i]))return 2;
    if(!module(package+"/depth/vertex/dataflow.spv",depth[0]) || !module(package+"/depth/store/dataflow.spv",depth[1]))return 2;
    NativeColorChainProgram::Translations translations;
    NativeColorChainProgram::Identities active;
    const std::array<const char*,5> roleNames{"depth/vertex","r8/vertex","r8/fragment","small/vertex","small/fragment"};
    const std::array<const std::vector<std::uint32_t>*,5> nativeWords{&depth[0],&code[1],&code[0],&code[3],&code[2]};
    for(unsigned i=0;i<5;++i){
        translations[i].guest.stage=active[i].stage=i==2 || i==4?1:0;
        if(!readInput((package+"/"+roleNames[i]+"/guest-code.bin").c_str(),translations[i].guest.bigEndianCode,4096) ||
           !readInput((std::string(argv[1])+"/active-"+std::to_string(i)+".bin").c_str(),active[i].bigEndianCode,4096))return 2;
        translations[i].nativeWords=*nativeWords[i];
    }
    const NativeColorChainProgram::Support support{code[4],code[5],code[6],depth[1]};
    auto program=NativeColorChainProgram::create(translations,support,1,mappedViewport?
        NativeColorCoordinateProfile::Padded1280W1:NativeColorCoordinateProfile::DiagnosticPositive);
    if(!program)return 2;
    std::optional<NativeColorChainCapture> captured;
    std::optional<NativeColorBatchCapture> batchCaptured;
    NativeColorChainEventRequests literalRequests;
    NativeColorCopyRequest copyRequest;
    if(!commands.empty()){
        captured=commandCapture(commands,*program,literalRequests,copyRequest,batchCaptured);if(!captured)return 2;
        PackedNativeColorChainInputs fromCommands,fromBindings;
        if(!captured->pack(fromCommands) || !packNativeColorChainInputs(input,fromBindings) ||
           fromCommands.depth!=fromBindings.depth || fromCommands.pairs!=fromBindings.pairs)return 1;
        std::puts("NATIVE_COMMAND_INPUTS independently_packed_match=1 borrowed_plan_destroyed=1");
        std::puts("NATIVE_COMMAND_COPY float_copy_shader_proven=1 exact_full_origin_rectangle=1 live_ownership=0");
        if(mappedViewport)std::puts("NATIVE_COMMAND_COORDINATES viewport_requests=2 native_vertex_w1_proven=1 full_raster_state_admitted=0");
        if(mappedViewport)std::puts("NATIVE_COMMAND_RENDER_STATE known_words_per_draw=42 fixed_state_requests=2 full_live_admission=0");
        if(mappedViewport)std::puts("NATIVE_COMMAND_DEPTH known_words=28 passthrough_pixel_proven=1 whole_depth_geometry_proven=1 full_live_admission=0");
    }
    std::vector<std::uint8_t> predicateBytes;
    if(!readInput((std::string(argv[1])+"/predicates.bin").c_str(),predicateBytes,72) || predicateBytes.size()!=72)return 2;
    std::array<DrawBinPredicate,3> predicates{};
    const auto predicateWord=[&](unsigned index){const auto* p=predicateBytes.data()+index*4;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    for(unsigned i=0;i<3;++i){const auto requested=predicateWord(i*6);if(requested>1)return 2;
        predicates[i].predicated=requested!=0;predicates[i].state.knownWords=predicateWord(i*6+1);
        for(unsigned w=0;w<4;++w)predicates[i].state.words[w]=predicateWord(i*6+2+w);}
    std::vector<std::uint8_t> expected;
    if(!readInput(argv[3],expected,96) || expected.size()!=96)return 2;
    for(unsigned i=0;i<16;++i)if(expected[i]!=0x5A || expected[80+i]!=0x5A)return 2;
    if(corrupt)expected[76]^=1;
    NativeWordUpdatePlan events;events.bufferBytes=40;
    if(literal)events=literalRequests.staging;
    else if(!empty){const NativeWordRequest requests[]={{16,0x12345678,NativeWordPhase::AfterGraphics},
        {20,0xA1B2C3D4,NativeWordPhase::AfterGraphics}};
        if(!planNativeWordUpdates(requests,2,40,events))return 2;}
    ProbeDevice device;if(!device.inspect() || !device.create({}))return 1;
    NativeColorChainExecutionResult result;result.nativeFences=73;result.eventBytes.fill(0xCD);
    const auto unchanged=[&](){return result.nativeFences==73 && !result.copy && !result.edram && !result.pendingRegisters && !result.pendingState && !result.auditedTimeline &&
        std::all_of(result.eventBytes.begin(),result.eventBytes.end(),[](auto b){return b==0xCD;});};
    if(executeNativeColorChain(device,code,depth,input,0x1F2D8000,events,result,
        static_cast<NativeColorCoordinateProfile>(99)) || !unchanged())return 1;
    if(captured){
        auto wrongCoordinates=NativeColorChainProgram::create(translations,support,1,mappedViewport?
            NativeColorCoordinateProfile::DiagnosticPositive:NativeColorCoordinateProfile::Padded1280W1);
        if(!wrongCoordinates || executeCapturedNativeColorChain(device,*wrongCoordinates,*captured,0x1F2D8000,events,result) || !unchanged())return 1;
        if(batchCaptured && (executeCapturedNativeColorBatch(device,*wrongCoordinates,*batchCaptured,result) || !unchanged()))return 1;
        std::puts("NATIVE_COORDINATE_REJECTIONS invalid_profile=1 capture_program_mismatch=1 output_preserved=1");
    }
    for(unsigned mutation=0;mutation<6;++mutation){auto badInput=input;auto badCode=code;auto badDepth=depth;auto badEvents=events;
        std::uint32_t base=0x1F2D8000;
        if(mutation==0)badEvents.count=65;
        if(mutation==1)badInput.pairs[1].pixelConstants[0].words[3].knownMask=0;
        if(mutation==2)badInput.pairs[0].vertices.source.size-=4;
        if(mutation==3)badCode[0].clear();
        if(mutation==4)badDepth[1].clear();
        if(mutation==5)++base;
        if(executeNativeColorChain(device,badCode,badDepth,badInput,base,badEvents,result) || !unchanged())return 1;
    }
    for(unsigned role=0;role<5;++role){auto mismatched=active;mismatched[role].bigEndianCode[0]^=1;
        if(executeMatchedNativeColorChain(device,*program,mismatched,predicates,input,0x1F2D8000,events,result) || !unchanged())return 1;}
    for(unsigned stage=0;stage<3;++stage)for(unsigned mutation=0;mutation<3;++mutation){auto rejected=predicates;
        rejected[stage].predicated=true;rejected[stage].state={{},mutation==0?0u:(mutation==1?15u:16u)};
        if(executeMatchedNativeColorChain(device,*program,active,rejected,input,0x1F2D8000,events,result) || !unchanged())return 1;}
    if(mappedViewport){
        for(unsigned role=0;role<2;++role)for(unsigned vertex=0;vertex<4;++vertex){
            auto badInput=input;auto badVertices=source[1+role];badVertices[vertex*36]^=1;
            badInput.pairs[role].vertices.source.bytes=badVertices.data();
            if(executeMatchedNativeColorChain(device,*program,active,predicates,badInput,0x1F2D8000,events,result) || !unchanged())return 1;
        }
        std::puts("NATIVE_VERTEX_GEOMETRY_REJECTIONS whole_primitives=8 output_preserved=1");
        for(unsigned vertex=0;vertex<3;++vertex){
            auto badInput=input;auto badVertices=source[0];badVertices[vertex*28+8]^=1;
            badInput.depth.source.bytes=badVertices.data();
            if(executeMatchedNativeColorChain(device,*program,active,predicates,badInput,0x1F2D8000,events,result) || !unchanged())return 1;
        }
        std::puts("NATIVE_DEPTH_GEOMETRY_REJECTIONS whole_primitives=3 output_preserved=1");
    }
    const auto copyBase=captured?copyRequest.copy.destinationBase:0x1F2D8000;
    const bool singleBatch=batchCaptured.has_value() && literal;
#if defined(RECOMP_NATIVE_COLOR_FAULT_TESTING)
    nativeColorAuditFaultStage=failAudit;
#else
    if(failAudit>=0)return 2;
#endif
    if(deferred){
        // This reuses only independently verified scratch sources, NOT the
        // old PM4 completion. Exercise the prefix executor with no events and
        // real stage faults, never publication or guest execution.
        NativeWordUpdatePlan noEvents;noEvents.bufferBytes=40;
        NativeDeferredColorSyncProgress prefix;
        const auto execute=[&](const auto& words,bool old,auto& progress){
            return executeCapturedNativeColorChain(device,*program,*captured,copyBase,words,result,old,&progress);};
        if(execute(noEvents,false,prefix) || !unchanged() || prefix.leadingFences())return 1;
        // A fresh empty queue submission suffices for this isolated scratch
        // test's leading ordering fence (no prior live resources exist here).
        VkFence fence=VK_NULL_HANDLE;VkFenceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if(vkCreateFence(device.device,&info,nullptr,&fence)!=VK_SUCCESS)return 1;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if(vkQueueSubmit(device.queue,1,&submit,fence)!=VK_SUCCESS ||
            vkWaitForFences(device.device,1,&fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        vkDestroyFence(device.device,fence,nullptr);
        if(!prefix.finishAuditedLeadingFence())return 1;
        if(execute(events,false,prefix) || !unchanged() || !prefix.readyForStages())return 1;
        if(execute(noEvents,true,prefix) || !unchanged() || !prefix.readyForStages())return 1;
        auto inFlight=prefix;if(!inFlight.begin(0))return 1;
        if(execute(noEvents,false,inFlight) || !unchanged() || inFlight.completedStages())return 1;
        std::puts("DEFERRED_PREFIX_GPU_ADMISSION: 4 scope/order refusals; output unchanged PASS");
        const bool executed=execute(noEvents,false,prefix);
        if(failAudit>=0){
            if(executed || !unchanged() || !prefix.readyForStages() || prefix.completedStages())return 1;
            std::printf("DEFERRED_PREFIX_AUDIT_FAILURE stage=%d rejected=1 output_unchanged=1 progress_unchanged=1 publication_calls=0\n",failAudit);
            return 0;
        }
        unsigned busy=99;
        if(!executed || !prefix.complete() || prefix.leadingFences()!=1 || prefix.completedStages()!=4 ||
            prefix.invalidations()!=2 || prefix.copyIdleCleans()!=1 || prefix.depthWaits()!=1 || prefix.cacheFlushes()!=1 ||
            prefix.literalEvents() || !prefix.busyBit(busy) || busy || result.auditedTimeline ||
            result.nativeFences!=4 || !result.copy || !result.edram || result.pendingRegisters || result.pendingState ||
            !std::all_of(result.eventBytes.begin(),result.eventBytes.end(),[](auto b){return b==0x5A;}))return 1;
        std::puts("DEFERRED_PREFIX_GPU_TIMELINE: leading=1 stages=4 invalidations=2 copy_idle=1 depth_wait=1 flush=1 events=0 guest_writes=0 PASS");
        return 0;
    }
    const bool executed=singleBatch?executeCapturedNativeColorBatch(device,*program,*batchCaptured,result):
        captured?executeCapturedNativeColorChain(device,*program,*captured,copyBase,events,result):
            executeMatchedNativeColorChain(device,*program,active,predicates,input,copyBase,events,result);
    if(failAudit>=0){
        if(executed || !unchanged())return 1;
        std::printf("NATIVE_AUDIT_FAILURE_CONTROL stage=%d rejected=1 output_unchanged=1 publication_calls=0\n",failAudit);
        return 0;
    }
    if(!executed || result.nativeFences!=4 || !result.copy || !result.edram)return 1;
    if(singleBatch)std::puts("NATIVE_BATCH_EXECUTION owned_frontend=1 request_copy_events=1 sync_effects_executed=0 guest_writes=0");
    if(result.auditedTimeline.has_value()!=singleBatch)return 1;
    if(singleBatch){const auto& timeline=*result.auditedTimeline;std::uint32_t busy=99;
        if(!timeline.complete() || timeline.completedStages()!=4 || !timeline.depthWaitSatisfied() ||
            !timeline.busyBit(busy) || busy || timeline.depthWaits()!=1 || timeline.invalidations()!=3 ||
            timeline.idleCleans()!=3 || timeline.cacheFlushes()!=1 || timeline.literalEvents()!=2)return 1;
        std::puts("NATIVE_SYNC_TIMELINE actual_fences=4 audits=4 modeled_depth_wait=1 modeled_invalidations=3 modeled_idle_cleans=3 live_register_writes=0");
    }
    // The execution helper has already destroyed all native resources. Its
    // returned copy must still publish independently to this owned SCRATCH.
    std::vector<std::uint8_t> mirror(result.copy->capacity()+32,0xCD),want=mirror;
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){std::uint32_t at=0;
        if(!colorCopyOffset(1280,720,x,y,at))return 1;std::memcpy(want.data()+16+at,expected.data()+76,4);}
    std::array<std::uint8_t,8> words;words.fill(0xCD);std::array<std::uint8_t,4> cursor{};
    const std::array<NativeOutputWord,2> destinations{{
        {16,{literal?literalRequests.physicalAddresses[0]:0x1000,words.data(),4}},
        {20,{literal?literalRequests.physicalAddresses[1]:0x2000,words.data()+4,4}}}};
    if(!publishNativeGpuOutputs(events,result.eventBytes.data(),40,destinations.data(),events.count,
        {0x3000,cursor.data(),4},0,1,&*result.copy,{copyBase,mirror.data()+16,result.copy->capacity()}))return 1;
    if(singleBatch){
        auto scratch=NativeColorScratchMemory::create(*batchCaptured);if(!scratch)return 1;
        const auto& memory=scratch->memory();
        for(unsigned i=0;i<3;++i)std::memset(memory.outputs[i].bytes(),0xCD,memory.outputs[i].range.bytes);
        const auto before=[&](){std::array<std::vector<std::uint8_t>,4> output;
            for(unsigned i=0;i<4;++i)output[i].assign(memory.outputs[i].bytes(),memory.outputs[i].bytes()+memory.outputs[i].range.bytes);
            return output;}();
        const auto unchanged=[&](){
            for(unsigned i=0;i<4;++i){
                if(std::memcmp(memory.outputs[i].bytes(),before[i].data(),before[i].size()))return false;
            }
            return true;
        };
        unsigned rejections=0;
        for(auto* changed:{memory.inputs[0].bytes(),memory.controls[1].bytes()}){
            *changed^=1;const bool rejected=!publishCapturedNativeColorOutputs(*batchCaptured,memory,*result.copy,result.eventBytes.data(),40);
            *changed^=1;if(!rejected || !unchanged())return 1;++rejections;
        }
        auto bad=memory;bad.outputs[0].writable=false;
        if(publishCapturedNativeColorOutputs(*batchCaptured,bad,*result.copy,result.eventBytes.data(),40) || !unchanged())return 1;++rejections;
        bad=memory;bad.outputs[1].allocationBytes=memory.outputs[0].allocationBytes;
        if(publishCapturedNativeColorOutputs(*batchCaptured,bad,*result.copy,result.eventBytes.data(),40) || !unchanged())return 1;++rejections;
        auto corruptEvents=result.eventBytes;corruptEvents[23]^=1;
        if(publishCapturedNativeColorOutputs(*batchCaptured,memory,*result.copy,corruptEvents.data(),40) || !unchanged())return 1;++rejections;
        if(!publishCapturedNativeColorOutputs(*batchCaptured,memory,*result.copy,result.eventBytes.data(),40) ||
            std::memcmp(memory.outputs[0].bytes(),mirror.data()+16,result.copy->capacity()) ||
            std::memcmp(memory.outputs[1].bytes(),result.eventBytes.data()+16,4) ||
            std::memcmp(memory.outputs[2].bytes(),result.eventBytes.data()+20,4))return 1;
        const auto next=output_publication_detail::bigEndian(batchCaptured->controls().nextWord);
        if(std::memcmp(memory.outputs[3].bytes(),next.data(),4) ||
            publishCapturedNativeColorOutputs(*batchCaptured,memory,*result.copy,result.eventBytes.data(),40))return 1;
        const auto previous=output_publication_detail::bigEndian(batchCaptured->controls().previousWord);
        std::memcpy(memory.outputs[3].bytes(),previous.data(),4);
        if(!matchesNativeColorBatchMemory(*batchCaptured,memory))return 1;
        std::memcpy(memory.outputs[3].bytes(),next.data(),4);
        std::printf("NATIVE_BOUND_PUBLICATION inputs_rechecked=%u negative_controls=%u scratch_rptr=%u guest_writes=0\n",
            batchCaptured->memory().inputCount,rejections,batchCaptured->controls().nextWord);
    }
    // Both owned results outlive the GPU allocations. Compare their actual
    // data with each other; no expected scene color enters this check.
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){std::uint32_t from=0,to=0,packed=0;
        if(!aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},x,y,0,from) || !colorCopyOffset(1280,720,x,y,to) ||
            !result.edram->readWord(from,packed))return 1;
        const auto copied=copyColorWord(packed,true);
        for(unsigned c=0;c<4;++c)if(mirror[16+to+c]!=static_cast<std::uint8_t>(copied>>(c*8)))return 1;
    }
    for(unsigned at:{0u,NativeColorEdramSnapshot::Begin-4,NativeColorEdramSnapshot::Begin+NativeColorEdramSnapshot::Bytes,AliasEdramBytes-4}){
        std::uint32_t untouched=73;if(result.edram->readWord(at,untouched) || untouched!=73)return 1;
    }
    std::puts("NATIVE_EDRAM_RETAINED known_words=921600 copy_consistent=1 resources_destroyed=1 outside_unknown=1 guest_writes=0");
    bool okay=mirror==want && cursor==std::array<std::uint8_t,4>{0,0,0,1};
    if(empty)okay&=std::all_of(words.begin(),words.end(),[](auto b){return b==0xCD;});
    else okay&=std::memcmp(words.data(),result.eventBytes.data()+16,8)==0;
    if(literal)std::printf("NATIVE_LITERAL_EVENT_REQUESTS native_staging_match=%u request_count=2 cache_flush_executed=0 guest_words_written=0\n",unsigned(okay));
    std::printf("NATIVE_COLOR_CHAIN_EXECUTION_V1 native_fences=%u native_packed_sources=1 pre_submit_rejections=6 "
        "native_event_words=%zu shader_role_matches=5 wrong_shader_rejections=5 predicate_rejections=9 output_survived_resources=1 scratch_match=%u guest_writeback=0 game_frame=0\n",
        result.nativeFences,events.count,unsigned(okay));
    return okay?0:1;
}
