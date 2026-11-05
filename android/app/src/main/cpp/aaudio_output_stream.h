#pragma once

#include "guest_audio_backend.h"
#include "native_audio_render_client.h"
#include <aaudio/AAudio.h>

namespace recomp::audio {
// Serialized ownership: open/query/close must not race. No input permission,
// host data callback, or guest callback is installed by this configuration slice.
class AAudioOutputStream final : public NativeAudioPlaybackBackend {
public:
    AAudioOutputStream() = default;
    ~AAudioOutputStream() override { close(); }
    AAudioOutputStream(const AAudioOutputStream&) = delete;
    AAudioOutputStream& operator=(const AAudioOutputStream&) = delete;
    bool open();
    bool requestStart() noexcept override;
    bool started() const noexcept override;
    std::int32_t writeStereo(const float* interleaved,std::uint32_t frames) noexcept override;
    // Android service counters, not proof of audible physical playback.
    std::optional<std::uint64_t> framesWritten() const noexcept override;
    std::optional<std::uint64_t> framesRead() const noexcept override;
    void close() noexcept;
    std::optional<bootstrap::NativeAudioConfiguration> configuration() const noexcept override;
    aaudio_result_t lastError() const noexcept { return lastError_; }
private:
    AAudioStream* stream_{nullptr};
    aaudio_result_t lastError_{AAUDIO_OK};
};
}
