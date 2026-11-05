#include "native_xma_frame_decoder.h"
#include <cassert>
#include <cstdio>
using recomp::audio::NativeXmaFrameDecoder;
int main() {
    NativeXmaFrameDecoder decoder;
    assert(decoder.available());
    NativeXmaFrameDecoder::Frame output;
    output.samples = 123;
    const std::uint8_t bad[4]{};
    assert(decoder.decode(bad, 4, 0, 24, output) == NativeXmaFrameDecoder::Result::CodecFailure);
    for (auto rate : {24000U, 32000U, 44100U, 48000U}) {
        for (auto channels : {1U, 2U}) {
            assert(decoder.configure(rate, channels));
            assert(decoder.configure(rate, channels));
            for (const auto first : {0U, 7U, 8U})
                assert(decoder.decode(bad, 4, first, 24, output) == NativeXmaFrameDecoder::Result::InvalidInput);
            assert(output.samples == 123);
            decoder.reset();
        }
    }
    assert(!decoder.configure(48000, 0) && !decoder.configure(48000, 3) && !decoder.configure(22050, 1));
    assert(decoder.decode(nullptr, 4, 0, 24, output) == NativeXmaFrameDecoder::Result::InvalidInput);
    assert(output.samples == 123);
    std::puts("NATIVE_XMA_FRAME_DECODER: 8 codec configurations/reopen/reset and invalid-bit-frame guards PASS; valid game frames not yet tested");
}
