#include "native_audio_queue.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace recomp::audio;
namespace {
class Writer final : public StereoFrameWriter {
public:
    std::vector<float> samples;
    std::int32_t limit{256};unsigned calls{0};
    Writer() {samples.reserve(65536);}
    std::int32_t writeStereo(const float* data,std::uint32_t frames) noexcept override {
        ++calls;if(limit<0) return limit;
        if(limit==999) return static_cast<std::int32_t>(frames+1);
        const auto accepted=std::min(frames,static_cast<std::uint32_t>(limit));
        assert(samples.size()+accepted*2<=samples.capacity());
        samples.insert(samples.end(),data,data+accepted*2);return accepted;
    }
};
}
int main() {
    std::array<std::uint8_t,AudioGuestPacketBytes> bytes{};
    const auto packet=[&](float value) {
        std::uint32_t bits;std::memcpy(&bits,&value,4);
        for(std::size_t sample=0;sample<AudioGuestPacketBytes/4;++sample)
            for(unsigned i=0;i<4;++i) bytes[sample*4+i]=static_cast<std::uint8_t>(bits>>(24-8*i));
    };
    for(auto chunk:{1,3,127,255,256}) {
        NativeAudioQueue queue;Writer sink;sink.limit=chunk;
        assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Empty && sink.calls==0);
        assert(!queue.submit(nullptr,bytes.size()));
        // Repeated fill/drain wraps the ring and verifies independent snapshots.
        std::vector<float> expected;
        for(unsigned cycle=0;cycle<3;++cycle) {
            for(unsigned p=0;p<8;++p) {
                const auto value=float(cycle*8+p)/32;packet(value);
                assert(queue.submit(bytes.data(),bytes.size()));
                expected.insert(expected.end(),512,value);
                bytes.fill(0xFF); // Caller storage no longer owns accepted samples.
            }
            assert(!queue.submit(bytes.data(),bytes.size()));
            sink.limit=0;
            const auto before=queue.writtenFrames();
            assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Backpressure);
            assert(queue.pendingPackets()==8 && queue.writtenFrames()==before);
            sink.limit=chunk;
            while(queue.pendingPackets()) assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Progress);
        }
        assert(sink.samples==expected && queue.acceptedPackets()==24 && queue.writtenFrames()==6144);
        assert(queue.clippedSamples()==0 && !queue.failed());
    }
    for(auto failure:{-1,999}) {
        NativeAudioQueue queue;Writer sink;packet(2);
        assert(queue.submit(bytes.data(),bytes.size()) && queue.clippedSamples()==512);
        sink.limit=17;assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Progress);
        sink.limit=failure;assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Failed);
        const auto calls=sink.calls;assert(queue.failed() && queue.writtenFrames()==17 && queue.pendingPackets()==1);
        assert(queue.drainOnce(sink)==NativeAudioQueue::Drain::Failed && sink.calls==calls);
        assert(!queue.submit(bytes.data(),bytes.size()));
    }
    NativeAudioQueue queue;bytes.fill(0xFF);
    assert(!queue.submit(bytes.data(),bytes.size()) && !queue.failed() && queue.acceptedPackets()==0);
    std::puts("NATIVE_AUDIO_QUEUE_V1 partial_write_sizes=5 packets_per_case=24 bounded_packets=8");
}
