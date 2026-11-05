#include "native_audio_render_client.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::audio;
namespace {
class Sink final : public NativeAudioPlaybackBackend {
public:
    recomp::bootstrap::NativeAudioConfiguration config{48000,2,4360,2180};
    bool live=true,starting=false,startOk=true,consume=false;
    int chunk=256;unsigned starts=0;
    std::uint64_t written=0,read=0;
    std::optional<recomp::bootstrap::NativeAudioConfiguration> configuration() const noexcept override {
        return live?std::optional{config}:std::nullopt;
    }
    bool started() const noexcept override {return starting;}
    bool requestStart() noexcept override {++starts;starting=startOk;return startOk;}
    std::optional<std::uint64_t> framesWritten() const noexcept override {return live?std::optional{written}:std::nullopt;}
    std::optional<std::uint64_t> framesRead() const noexcept override {return live?std::optional{read}:std::nullopt;}
    std::int32_t writeStereo(const float*,std::uint32_t frames) noexcept override {
        if(chunk<0) return chunk;
        const auto room=config.bufferCapacityFrames-(written-read);
        const auto n=std::min({std::uint64_t{frames},std::uint64_t(chunk),room});
        written+=n;if(consume) read=written;return static_cast<std::int32_t>(n);
    }
};
std::array<std::uint8_t,6144> Frame{};
constexpr std::uint32_t Token=0x64001000;
void packet(NativeAudioRenderClient& client) {
    assert(client.beginCallback(Token));
    assert(client.submit(Token,NativeAudioRenderClient::Handle,Frame.data(),Frame.size()));
    assert(!client.submit(Token,NativeAudioRenderClient::Handle,Frame.data(),Frame.size()));
    assert(client.finishCallback(Token));
}
}
int main() {
    using Demand=NativeAudioRenderClient::Demand;
    assert(!NativeAudioRenderClient(nullptr).canActivate());
    for(auto capacity:{128U,256U,2048U,4360U,48000U}) for(auto chunk:{1,127,256}) {
        auto sink=std::make_shared<Sink>();sink->config.bufferCapacityFrames=capacity;
        sink->config.framesPerBurst=std::min(2180U,capacity);sink->chunk=chunk;
        NativeAudioRenderClient client(sink);assert(client.canActivate());client.publishWorker(Token);
        assert(!client.canActivate());
        const auto target=(capacity+255)/256*256;
        unsigned loops=0;
        while(client.statistics().acceptedPackets*256<target || sink->written<capacity) {
            assert(++loops<100000);
            const auto demand=client.poll(Token);assert(demand!=Demand::Failed);
            if(demand==Demand::Callback) packet(client);
        }
        assert(client.statistics().acceptedPackets*256==target);
        assert(sink->written==capacity && sink->read==0 && sink->starts==1);
        // Buffered data is not consumption, even after many polls.
        for(unsigned i=0;i<50;++i) assert(client.poll(Token)==Demand::Wait);
        assert(client.statistics().serviceReadFrames==0);
        sink->consume=true;sink->read=sink->written;
        const auto before=client.statistics().callbacksCompleted;
        for(unsigned i=0;i<1000 && client.statistics().callbacksCompleted<before+24;++i)
            if(client.poll(Token)==Demand::Callback) packet(client);
        assert(client.statistics().callbacksCompleted==before+24);
        assert(client.statistics().callbacksEntered==client.statistics().callbacksCompleted);
        assert(sink->starts==1 && !client.statistics().failed);
    }
    for(unsigned failure=0;failure<7;++failure) {
        auto sink=std::make_shared<Sink>();NativeAudioRenderClient client(sink);client.publishWorker(Token);
        packet(client);
        if(failure==0) sink->live=false;
        if(failure==1) sink->config.channels=6;
        if(failure==2) sink->startOk=false;
        if(failure==3) sink->chunk=-1;
        if(failure==4) sink->written=1; // External writer violates exclusive ownership.
        if(failure==5) sink->read=9999;
        if(failure==6) {assert(client.poll(Token)==Demand::Callback);sink->read=128;
            assert(client.poll(Token)==Demand::Callback);sink->read=127;}
        assert(client.poll(Token)==Demand::Failed && client.statistics().failed);
        sink->live=true;sink->chunk=256;sink->startOk=true;
        assert(client.poll(Token)==Demand::Failed && !client.canActivate());
    }
    auto sink=std::make_shared<Sink>();NativeAudioRenderClient client(sink);client.publishWorker(Token);
    assert(!client.submit(Token,client.Handle,Frame.data(),Frame.size()));
    assert(client.beginCallback(Token));
    assert(!client.submit(Token+4,client.Handle,Frame.data(),Frame.size()));
    assert(!client.submit(Token,client.Handle+1,Frame.data(),Frame.size()));
    Frame[0]=0x7F;Frame[1]=0x80;
    assert(!client.submit(Token,client.Handle,Frame.data(),Frame.size()));
    Frame.fill(0);assert(!client.finishCallback(Token)); // No silent callback-credit recycling.
    assert(client.statistics().failed && client.statistics().acceptedPackets==0);
    std::puts("NATIVE_RENDER_CLIENT_V1 pacing_cases=15 failure_modes=7 synthetic_only=1");
}
