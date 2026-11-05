#include "guest_audio_frame.h"
#include <cassert>
#include <cstdio>

using namespace recomp::audio;
int main() {
    std::array<std::uint8_t,AudioGuestPacketBytes+2> bytes{};
    const auto store=[&](std::size_t index,std::uint32_t bits) {
        for(unsigned i=0;i<4;++i) bytes[1+index*4+i]=static_cast<std::uint8_t>(bits>>(24-8*i));
    };
    const auto bits=[](float value) {std::uint32_t word;std::memcpy(&word,&value,4);return word;};
    PlanarAudioPacket decoded;decoded.fill(0.25f);const auto before=decoded;
    assert(!decodePlanarAudioPacket(nullptr,AudioGuestPacketBytes,decoded));
    for(auto size:{std::size_t{0},AudioGuestPacketBytes-1,AudioGuestPacketBytes+1})
        assert(!decodePlanarAudioPacket(bytes.data()+1,size,decoded) && decoded==before);
    std::uint32_t checks=0;
    for(std::size_t channel=0;channel<6;++channel) for(std::size_t frame=0;frame<256;++frame)
    for(float amplitude:{1.0f,-1.0f}) {
        bytes.fill(0);bytes.front()=0xA5;bytes.back()=0x5A;
        store(channel*256+frame,bits(amplitude));const auto original=bytes;
        assert(decodePlanarAudioPacket(bytes.data()+1,AudioGuestPacketBytes,decoded));
        for(std::size_t i=0;i<decoded.size();++i) assert(decoded[i]==(i==channel*256+frame?amplitude:0));
        StereoAudioPacket stereo;std::uint32_t clips=999;
        assert(mixNormalizedStereoNoLfe(decoded,stereo,clips) && clips==0);
        constexpr float weights[6][2]={{.4f,0},{0,.4f},{.2f,.2f},{0,0},{.4f,0},{0,.4f}};
        for(std::size_t f=0;f<256;++f) for(unsigned side=0;side<2;++side)
            assert(stereo[f*2+side]==(f==frame?amplitude*weights[channel][side]:0));
        assert(bytes==original);++checks;
    }
    // Every plane/sample position rejects Inf and both quiet/signaling NaNs.
    for(std::size_t index=0;index<decoded.size();++index)
    for(auto invalid:{0x7F800000U,0xFF800000U,0x7FC00001U,0x7F800001U}) {
        bytes.fill(0);store(index,invalid);decoded=before;
        assert(!decodePlanarAudioPacket(bytes.data()+1,AudioGuestPacketBytes,decoded) && decoded==before);
        ++checks;
    }
    for(float amplitude:{1.0f,-1.0f,2.0f,-2.0f,std::numeric_limits<float>::max()}) {
        decoded.fill(amplitude);StereoAudioPacket stereo;std::uint32_t clips=999;
        assert(mixNormalizedStereoNoLfe(decoded,stereo,clips));
        const auto expected=std::clamp(amplitude,-1.0f,1.0f);
        for(float value:stereo) assert(value==expected);
        assert(clips==(std::abs(amplitude)>1?512U:0U));
    }
    decoded.fill(0);decoded[3*256]=std::numeric_limits<float>::infinity();
    StereoAudioPacket stereo;stereo.fill(.125f);const auto unchanged=stereo;std::uint32_t clips=123;
    assert(!mixNormalizedStereoNoLfe(decoded,stereo,clips) && stereo==unchanged && clips==123);
    bytes.fill(0);store(0,0x80000000);store(1,1);store(2,0x00800000);
    assert(decodePlanarAudioPacket(bytes.data()+1,AudioGuestPacketBytes,decoded));
    assert(bits(decoded[0])==0x80000000 && bits(decoded[1])==1 && bits(decoded[2])==0x00800000);
    std::printf("AUDIO_FRAME_V1 impulse_and_invalid_checks=%u layout=planar6x256 stereo_policy=normalized-no-lfe\n",checks);
}
