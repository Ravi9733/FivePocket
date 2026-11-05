// HOST TEST STAND-IN for native_xma_frame_decoder.cpp. No build of the Xenia FFmpeg XMAFRAMES
// codec exists for the Mac (the pinned archives are Android ELF objects), so host runs of the
// XMA pool tests link this instead. It validates the frame envelope exactly like the real
// NativeXmaFrameDecoder::decode (first bit, bit count range, available bytes, declared 15-bit
// length) and returns a silent 512-sample frame. It proves packet traversal, loops, locking and
// guest-word publication; it proves nothing about codec output. The codec-backed variants are
// still built by tools/host_guest_trace/test_xma_runtime.sh against the pinned FFmpeg build.
// The stand-in never touches the FFmpeg handles that the real decoder owns.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#include "native_xma_frame_decoder.h"
#pragma clang diagnostic pop

namespace recomp::audio {
NativeXmaFrameDecoder::NativeXmaFrameDecoder() = default;
NativeXmaFrameDecoder::~NativeXmaFrameDecoder() = default;
bool NativeXmaFrameDecoder::available() const noexcept { return true; }
bool NativeXmaFrameDecoder::configure(std::uint32_t rate, std::uint32_t channels) {
    if (channels < 1 || channels > 2 || (rate != 24000 && rate != 32000 && rate != 44100 && rate != 48000))
        return false;
    rate_ = rate;
    channels_ = channels;
    return true;
}
void NativeXmaFrameDecoder::reset() noexcept {}
NativeXmaFrameDecoder::Result NativeXmaFrameDecoder::decode(const std::uint8_t* data, std::size_t bytes,
        std::uint32_t firstBit, std::uint32_t bitCount, Frame& output) {
    if (!rate_) return Result::CodecFailure;
    if (!data || firstBit > 7 || bitCount < 15 || bitCount >= 0x7FFF) return Result::InvalidInput;
    const auto payloadBytes = (firstBit + bitCount + 7) / 8;
    if (bytes < payloadBytes) return Result::InvalidInput;
    std::uint32_t declaredBits = 0;
    for (unsigned bit = firstBit; bit < firstBit + 15; ++bit)
        declaredBits = (declaredBits << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1U);
    if (declaredBits != bitCount) return Result::InvalidInput;
    Frame staged;
    staged.channels = channels_;
    staged.sampleRate = rate_;
    staged.samples = 512;
    output = staged;
    return Result::Decoded;
}
} // namespace recomp::audio
