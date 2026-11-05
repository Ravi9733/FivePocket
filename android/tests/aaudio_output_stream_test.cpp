#include "aaudio_output_stream.h"
#include <cassert>
#include <cstdio>
#include <chrono>
#include <thread>

int main() {
    recomp::audio::AAudioOutputStream stream;
    assert(!stream.configuration());stream.close();stream.close();
    for (unsigned i=0;i<3;++i) {
        if (!stream.open()) {
            std::fprintf(stderr,"AAUDIO_OPEN_FAILED error=%d\n",stream.lastError());return 1;
        }
        const auto c=stream.configuration();assert(c);
        assert(!stream.open());assert(stream.configuration());
        std::printf("AAUDIO_OUTPUT_V1 rate=%u channels=%u capacity=%u burst=%u open_cycle=%u started=0 submitted_frames=0\n",
            c->sampleRate,c->channels,c->bufferCapacityFrames,c->framesPerBurst,i);
        stream.close();assert(!stream.configuration());
    }
    assert(stream.open());assert(!stream.started());
    assert(stream.framesWritten()==0 && stream.framesRead()==0);
    recomp::audio::StereoAudioPacket samples{};
    assert(stream.writeStereo(nullptr,1)<0 && stream.writeStereo(samples.data(),0)<0);
    assert(stream.writeStereo(samples.data(),257)<0);
    samples[511]=std::numeric_limits<float>::infinity();
    assert(stream.writeStereo(samples.data(),256)<0 && stream.framesWritten()==0);
    samples[511]=2;assert(stream.writeStereo(samples.data(),256)<0);samples[511]=0;
    assert(stream.requestStart());assert(!stream.requestStart());
    recomp::audio::NativeAudioQueue queue;
    std::array<std::uint8_t,recomp::audio::AudioGuestPacketBytes> silence{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(queue.acceptedPackets()<24 || queue.pendingPackets()) {
        assert(std::chrono::steady_clock::now()<deadline);
        if(queue.acceptedPackets()<24 && queue.pendingPackets()<queue.CapacityPackets)
            assert(queue.submit(silence.data(),silence.size()));
        const auto result=queue.drainOnce(stream);
        assert(result!=recomp::audio::NativeAudioQueue::Drain::Failed);
        if(result==recomp::audio::NativeAudioQueue::Drain::Backpressure)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(stream.framesWritten()==6144 && queue.writtenFrames()==6144);
    while(stream.framesRead() && *stream.framesRead()<6144 && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(stream.started() && stream.framesRead() && *stream.framesRead()>=6144);
    std::printf("AAUDIO_PCM_V1 fixture_silence_frames=6144 service_read_frames=%llu started=1 guest_frames=0 audible_verified=0\n",
        static_cast<unsigned long long>(*stream.framesRead()));
    stream.close();assert(!stream.configuration() && !stream.started());
    assert(!stream.framesWritten() && !stream.framesRead() && !stream.requestStart());
    assert(stream.writeStereo(samples.data(),1)<0);
    assert(stream.open() && stream.framesWritten()==0 && stream.framesRead()==0);
    stream.close();
    // Demand pacing must prime this device using actual submitted packets,
    // without fixture-only tail padding or a fixed eight-packet credit limit.
    auto native=std::make_shared<recomp::audio::AAudioOutputStream>();assert(native->open());
    recomp::audio::NativeAudioRenderClient client(native);assert(client.canActivate());client.publishWorker(1);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(client.statistics().acceptedPackets<32 || client.statistics().pendingPackets || client.statistics().serviceReadFrames<8192) {
        assert(std::chrono::steady_clock::now()<end);
        const auto demand=client.poll(1);assert(demand!=recomp::audio::NativeAudioRenderClient::Demand::Failed);
        if(demand==recomp::audio::NativeAudioRenderClient::Demand::Callback && client.statistics().acceptedPackets<32) {
            assert(client.beginCallback(1));assert(client.submit(1,client.Handle,silence.data(),silence.size()));
            assert(client.finishCallback(1));
        } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(client.statistics().callbacksCompleted==32 && native->framesWritten()==8192);
    std::printf("AAUDIO_DEMAND_V1 synthetic_callbacks=32 submitted_frames=8192 service_read_frames=%llu tail_padding_frames=0 game_audio=0\n",
        static_cast<unsigned long long>(client.statistics().serviceReadFrames));
}
