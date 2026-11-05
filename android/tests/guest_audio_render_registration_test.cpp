#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include "guest_thread_entry.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::bootstrap;
using namespace recomp::audio;
namespace {
constexpr std::uint32_t Parent=0x63001000,Code=0x82001000,Descriptor=0x2000,Output=0x2010;
const GuestThreadStorageConfig Config{{0x64000000,0x100000},{0x82000000,0x10000},{Code,0x1000},0,0,32,4,0x4000,2};
constexpr auto Register="__imp__XAudioRegisterRenderDriverClient",Submit="__imp__XAudioSubmitRenderDriverFrame";
class Sink final : public NativeAudioPlaybackBackend {
public:
    bool startedValue=false;std::uint64_t written=0;
    bool live=true;
    std::optional<NativeAudioConfiguration> configuration() const noexcept override {return live?std::optional{NativeAudioConfiguration{48000,2,4360,2180}}:std::nullopt;}
    bool requestStart() noexcept override {startedValue=true;return true;}
    bool started() const noexcept override {return startedValue;}
    std::optional<std::uint64_t> framesWritten() const noexcept override {return written;}
    std::optional<std::uint64_t> framesRead() const noexcept override {return written;}
    std::int32_t writeStereo(const float* data,std::uint32_t frames) noexcept override {
        for(unsigned i=0;i<frames;++i) {assert(data[i*2]==0.1f);assert(data[i*2+1]==0.0f);}
        written+=frames;return static_cast<std::int32_t>(frames);
    }
};
void put(GuestAddressSpace& s,std::uint32_t a,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) s.base()[a+i]=static_cast<std::uint8_t>(value>>(24-i*8));
}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t a) {
    const auto* b=s.base()+a;return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];
}
GuestImportDispatchResult invoke(GuestImportRegistry& r,const char* name,std::uint32_t a,std::uint32_t b) {
    GuestImportCall call;call.arguments={0xFEDCBA9800000000ULL|a,0x1234567800000000ULL|b,2,3,4,5,6,7};
    call.returnValue=0xDEADBEEF12345678ULL;const auto args=call.arguments;
    const auto result=r.dispatch(name,call);assert(call.arguments==args);
    assert(call.returnValue==(result.handled()?0:0xDEADBEEF12345678ULL));return result;
}
void setup(GuestImportRegistry& r,GuestAddressSpace& s,const std::shared_ptr<Sink>& sink) {
    assert(r.initialize(s,0x10000000,0x10000));assert(r.bindGuestThreadToken(Parent));
    assert(r.bindGuestThreadTlsStorage(Parent,0x63002000,4));
    assert(r.bindThreadStorageArena(Config,{},[](auto a){return a==Code;}));
    assert(r.bindExperimentalAudioOutput(sink));put(s,Descriptor,Code);put(s,Descriptor+4,0xDEADBEEF);put(s,Output,0xA5A5A5A5);
}
}
int main() {
    GuestAddressSpace s;assert(s.reserve());GuestImportRegistry r;
    auto sink=std::make_shared<Sink>();auto client=std::make_shared<NativeAudioRenderClient>(sink);
    assert(!r.bindExperimentalAudioRenderer(client,[](const auto&){return false;}));setup(r,s,sink);
    constexpr auto Gain="__imp__XAudioGetVoiceCategoryVolume";
    constexpr auto Ducker="__imp__XAudioEnableDucker";
    assert(invoke(r,Ducker,0,0).state==GuestImportDispatchState::Rejected);
    assert(!r.bindNativeCategoryOneUnityGain());assert(!invoke(r,Gain,1,Output).handled());
    assert(invoke(r,Register,Descriptor,Output).state==GuestImportDispatchState::Rejected);
    assert(!r.bindExperimentalAudioRenderer(nullptr,[](const auto&){return false;}));
    assert(!r.bindExperimentalAudioRenderer(client,{}));
    assert(!r.bindExperimentalAudioRenderer(std::make_shared<NativeAudioRenderClient>(std::make_shared<Sink>()),[](const auto&){return false;}));
    unsigned enqueues=0;bool throwEnqueue=false;GuestPreparedThread observed;
    assert(r.bindExperimentalAudioRenderer(client,[&](const auto& thread)->bool {
        ++enqueues;observed=thread;if(throwEnqueue) throw std::runtime_error("fixture enqueue failure");return false;
    }));
    assert(!r.bindExperimentalAudioRenderer(client,[](const auto&){return false;}));
    assert(r.bindNativeCategoryOneUnityGain());assert(!r.bindNativeCategoryOneUnityGain());
    for (unsigned i=0;i<2;++i) assert(invoke(r,Ducker,0,UINT32_MAX).handled());
    for (auto enabled : {1U,2U,UINT32_MAX})
        assert(invoke(r,Ducker,enabled,0).state==GuestImportDispatchState::Rejected);
    assert(get(s,Output)==0xA5A5A5A5 && r.nativeCategoryOneGainQueries()==0);
    for(auto category:{0U,2U,3U,31U,32U,UINT32_MAX}) {
        assert(invoke(r,Gain,category,Output).state==GuestImportDispatchState::Rejected);
        assert(get(s,Output)==0xA5A5A5A5 && r.nativeCategoryOneGainQueries()==0);
    }
    for(auto bad:{0U,Output+1,0xFFFFFFFFU,0x7FC80000U,0x10000000U,0x63002000U,0x64000000U}) {
        assert(invoke(r,Gain,1,bad).state==GuestImportDispatchState::GuestMemoryFault);
        assert(r.nativeCategoryOneGainQueries()==0);
    }
    for(unsigned i=0;i<100;++i) {
        assert(invoke(r,Gain,1,Output).handled() && get(s,Output)==0x3F800000);
        assert(r.nativeCategoryOneGainQueries()==i+1);
    }
    sink->live=false;assert(invoke(r,Gain,1,Output).state==GuestImportDispatchState::Rejected);
    assert(invoke(r,Ducker,0,0).state==GuestImportDispatchState::Rejected);
    assert(get(s,Output)==0x3F800000 && r.nativeCategoryOneGainQueries()==100);
    sink->live=true;put(s,Output,0xA5A5A5A5);
    for(auto bad:{0U,Descriptor+1,0xFFFFFFFCU,0x7FC80000U,0x10000000U,0x63002000U,0x64000000U}) {
        assert(invoke(r,Register,bad,Output).state==GuestImportDispatchState::GuestMemoryFault);
        assert(enqueues==0 && get(s,Output)==0xA5A5A5A5);
    }
    for(auto bad:{0U,Output+1,0xFFFFFFFFU,0x7FC80000U,0x10000000U,0x63002000U,0x64000000U}) {
        assert(invoke(r,Register,Descriptor,bad).state==GuestImportDispatchState::GuestMemoryFault);assert(enqueues==0);
    }
    put(s,Descriptor,Code+4);assert(invoke(r,Register,Descriptor,Output).state==GuestImportDispatchState::Rejected);
    assert(enqueues==0);put(s,Descriptor,Code);
    for(bool throws:{false,true}) {
        throwEnqueue=throws;bool caught=false;
        try {assert(invoke(r,Register,Descriptor,Output).state==GuestImportDispatchState::Rejected);}
        catch(const std::runtime_error&) {caught=true;}
        assert(caught==throws && r.createdThreadCount()==0 && client->canActivate());
        assert(get(s,Output)==0xA5A5A5A5 && get(s,Descriptor+4)==0xDEADBEEF);
        assert(std::all_of(s.base()+observed.allocation.base,s.base()+observed.allocation.base+observed.allocation.size,[](auto b){return b==0;}));
    }
    assert(enqueues==2);r.reset();assert(r.nativeCategoryOneGainQueries()==0);
    // Actual serialized worker, aliased descriptor/output, no retained source.
    sink=std::make_shared<Sink>();client=std::make_shared<NativeAudioRenderClient>(sink);setup(r,s,sink);
    assert(invoke(r,Gain,1,Output).state==GuestImportDispatchState::Rejected);
    GuestCooperativeExecutor executor;bool done=false,entered=false;std::uint32_t workerToken=0;
    assert(r.bindExperimentalAudioRenderer(client,[&](const auto& thread) {
        observed=thread;workerToken=thread.layout.threadAddress;
        return executor.enqueue(workerToken,[&,thread] {
            const auto initial=makeGuestThreadEntry(thread);assert(initial && get(s,initial->argument0)==0xDEADBEEF);
            assert(get(s,Descriptor)==client->Handle);entered=true;
            const auto samples=thread.layout.stackBase+128;
            assert(invoke(r,Ducker,0,0).handled());
            assert(invoke(r,Gain,1,samples).handled() && get(s,samples)==0x3F800000);
            for(unsigned packet=0;packet<24;++packet) {
                assert(client->beginCallback(workerToken));
                for(auto bad:{0U,samples+1,0xFFFFF000U,0x7FC80000U,0x10000000U,thread.layout.pcrAddress,thread.startContext})
                    assert(invoke(r,Submit,client->Handle,bad).state==GuestImportDispatchState::GuestMemoryFault);
                std::fill_n(s.base()+samples,AudioGuestPacketBytes,0);
                for(unsigned i=0;i<256;++i) put(s,samples+i*4,0x3E800000);
                assert(invoke(r,Submit,client->Handle+1,samples).state==GuestImportDispatchState::Rejected);
                assert(invoke(r,Submit,client->Handle,samples).handled());
                std::fill_n(s.base()+samples,AudioGuestPacketBytes,0xA5);
                assert(client->finishCallback(workerToken));
            }
            assert(client->poll(workerToken)==NativeAudioRenderClient::Demand::Callback);
            assert(r.markGuestThreadReturned(workerToken,0));done=true;
        });
    }));
    assert(r.bindNativeCategoryOneUnityGain());
    const auto result=executor.run(Parent,[&] {
        assert(invoke(r,Register,Descriptor,Descriptor).handled());assert(!entered);
        assert(client->statistics().workerToken==workerToken);
        assert(invoke(r,Register,Descriptor,Output).state==GuestImportDispatchState::Rejected);
        assert(invoke(r,Submit,client->Handle,Descriptor).state==GuestImportDispatchState::Rejected);
        assert(executor.waitUntil([&]{return done;}));
    },{100,1,2},[&](auto token){assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed() && result.workersEntered==1);
    const auto stats=client->statistics();assert(stats.callbacksCompleted==24 && stats.acceptedPackets==24);
    assert(stats.writtenFrames==6144 && stats.serviceReadFrames==6144 && !stats.failed && !stats.callbackActive);
    r.reset();assert(std::all_of(s.base()+observed.allocation.base,s.base()+observed.allocation.base+observed.allocation.size,[](auto b){return b==0;}));
    std::puts("AUDIO_RENDER_REGISTRATION_V1 rollback=2 actual_workers=1 synthetic_callbacks=24 snapshot_frames=6144 game_audio=0");
}
