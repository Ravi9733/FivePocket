#include "guest_import_registry.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

using namespace recomp::bootstrap;
namespace {
class FixtureBackend final : public GuestAudioBackend {
public:
    std::optional<NativeAudioConfiguration> value=NativeAudioConfiguration{48000,2,2048,256};
    std::optional<NativeAudioConfiguration> configuration() const noexcept override { return value; }
};
}
int main() {
    GuestAddressSpace s;assert(s.reserve());GuestImportRegistry r;
    auto backend=std::make_shared<FixtureBackend>();
    assert(!r.bindExperimentalAudioOutput(backend));
    GuestImportCall call;call.arguments[0]=0x1010;
    assert(r.dispatch("__imp__XAudioGetSpeakerConfig",call).state==GuestImportDispatchState::RegistryUnavailable);
    assert(r.initialize(s,0x10000000,0x10000));
    assert(!r.bindExperimentalAudioOutput(nullptr));
    assert(r.dispatch("__imp__XAudioGetSpeakerConfig",call).state==GuestImportDispatchState::Rejected);
    for (unsigned field=0;field<4;++field) for (auto bad:{0U,1U,3U,44100U,48001U,UINT32_MAX}) {
        NativeAudioConfiguration c{48000,2,2048,256};
        auto* target=field==0?&c.sampleRate:field==1?&c.channels:field==2?&c.bufferCapacityFrames:&c.framesPerBurst;
        *target=bad;backend->value=c;
        if (!validNativeAudioConfiguration(c)) assert(!r.bindExperimentalAudioOutput(backend));
    }
    backend->value=std::nullopt;assert(!r.bindExperimentalAudioOutput(backend));
    backend->value=NativeAudioConfiguration{48000,2,2048,256};
    assert(r.bindExperimentalAudioOutput(backend));assert(!r.bindExperimentalAudioOutput(backend));
    std::array<std::uint8_t,64> expected;expected.fill(0xA5);
    std::copy(expected.begin(),expected.end(),s.base()+0x1000);
    const auto invoke=[&](std::uint32_t output) {
        call.arguments={0xFEDCBA9800000000ULL|output,1,2,3,4,5,6,7};
        call.returnValue=0x123456789ABCDEF0ULL;call.stackPointer=0x7000;
        const auto args=call.arguments;
        const auto result=r.dispatch("__imp__XAudioGetSpeakerConfig",call);
        assert(call.arguments==args && call.stackPointer==0x7000);
        if (!result.handled()) assert(call.returnValue==0x123456789ABCDEF0ULL);
        return result;
    };
    for (auto bad:{0U,0x1011U,0x1012U,0x1013U,0xFFFFFFFDU,0xFFFFFFFFU,0x7FC80000U,0x7FC8FFFCU}) {
        assert(invoke(bad).state==GuestImportDispatchState::GuestMemoryFault);
        assert(std::equal(expected.begin(),expected.end(),s.base()+0x1000));
        assert(r.audioSpeakerQueries()==0);
    }
    for (unsigned i=0;i<100;++i) {
        assert(invoke(0x1010).handled() && call.returnValue==0);
        expected[16]=0;expected[17]=1;expected[18]=0;expected[19]=1;
        assert(std::equal(expected.begin(),expected.end(),s.base()+0x1000));
        assert(r.audioSpeakerQueries()==i+1);
    }
    backend->value=std::nullopt;
    assert(invoke(0x1010).state==GuestImportDispatchState::Rejected && r.audioSpeakerQueries()==100);
    assert(std::equal(expected.begin(),expected.end(),s.base()+0x1000));
    backend->value=NativeAudioConfiguration{44100,2,2048,256};
    assert(invoke(0x1010).state==GuestImportDispatchState::Rejected);
    // Configuration alone never enables a callback or PCM submission.
    for (auto name:{"__imp__XAudioRegisterRenderDriverClient","__imp__XAudioSubmitRenderDriverFrame","__imp__XAudioGetVoiceCategoryVolume"})
        assert(r.dispatch(name,call).state==GuestImportDispatchState::Rejected);
    for (auto name:{"__imp__XAudioUnregisterRenderDriverClient"})
        assert(r.dispatch(name,call).state==GuestImportDispatchState::NotRegistered);
    r.reset();assert(r.audioSpeakerQueries()==0);
    assert(r.initialize(s,0x10000000,0x10000));
    assert(invoke(0x1010).state==GuestImportDispatchState::Rejected);
    backend->value=NativeAudioConfiguration{48000,2,2048,256};
    assert(r.bindExperimentalAudioOutput(backend));
    assert(r.bindPhysicalMemoryArena(0xA0000000,0x10000,0));
    for (auto protection:{2U,4U}) {
        GuestImportCall allocation;allocation.arguments={1,4096,protection,0,UINT32_MAX,0};
        assert(r.dispatch("__imp__MmAllocatePhysicalMemoryEx",allocation).handled());
        const auto address=static_cast<std::uint32_t>(allocation.returnValue);assert(address);
        std::fill_n(s.base()+address,4096,0xA5);
        assert(invoke(address).handled()==(protection==4));
        assert(s.base()[address]==(protection==4?0:0xA5));
    }
    assert(invoke(0xA000FFF0).state==GuestImportDispatchState::GuestMemoryFault);
    const auto put=[&](std::uint32_t address,std::uint32_t value) {
        for(unsigned i=0;i<4;++i) s.base()[address+i]=static_cast<std::uint8_t>(value>>(24-8*i));
    };
    assert(invoke(0x10000000).state==GuestImportDispatchState::GuestMemoryFault);
    put(0x2000,0);put(0x2004,4096);
    GuestImportCall allocate;allocate.arguments={0x2000,0x2004,0x2000,4,0};
    assert(r.dispatch("__imp__NtAllocateVirtualMemory",allocate).handled() && allocate.returnValue==0);
    assert(invoke(0x10000000).state==GuestImportDispatchState::GuestMemoryFault);
    put(0x2000,0x10000000);put(0x2004,4096);allocate.arguments[2]=0x1000;
    assert(r.dispatch("__imp__NtAllocateVirtualMemory",allocate).handled() && allocate.returnValue==0);
    assert(invoke(0x10000FFC).handled());
    assert(invoke(0x10001000).state==GuestImportDispatchState::GuestMemoryFault);
    assert(r.bindGuestThreadToken(1));assert(r.bindGuestThreadTlsStorage(1,0x20000,64));
    assert(invoke(0x20000).state==GuestImportDispatchState::GuestMemoryFault);
    // Two synthetic XEX pages, one executable/read-only and one writable.
    constexpr std::uint32_t H=0x60000000,B=0x82000000,S=0x80;
    std::fill_n(s.base()+H,0x300,0);
    put(H,0x58455832);put(H+8,0x300);put(H+16,S);put(H+20,1);
    put(H+S,0x184+2*24);put(H+S+4,2*65536);put(H+S+0x110,B);put(H+S+0x180,2);
    put(H+S+0x184,0x11);put(H+S+0x184+24,0x12);
    assert(r.bindImagePageProtections(H,0x300,B,2*65536));
    put(B,0xA5A5A5A5);assert(invoke(B).state==GuestImportDispatchState::GuestMemoryFault);
    assert(s.base()[B]==0xA5);assert(invoke(B+65536).handled());
    std::puts("AUDIO_CONFIGURATION_V1 repeated_queries=100 guarded_playback=1");
}
