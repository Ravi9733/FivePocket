#include "aaudio_output_stream.h"

namespace recomp::audio {
bool AAudioOutputStream::open() {
    if (stream_) return false;
    AAudioStreamBuilder* builder=nullptr;
    lastError_=AAudio_createStreamBuilder(&builder);
    if (lastError_!=AAUDIO_OK || !builder) return false;
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder,48000);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_NONE);
    lastError_=AAudioStreamBuilder_openStream(builder,&stream_);
    const auto deleted=AAudioStreamBuilder_delete(builder);
    if (lastError_!=AAUDIO_OK || deleted!=AAUDIO_OK || !configuration()) {
        if (lastError_==AAUDIO_OK) lastError_=deleted!=AAUDIO_OK?deleted:AAUDIO_ERROR_INVALID_FORMAT;
        close();return false;
    }
    return true;
}
void AAudioOutputStream::close() noexcept {
    if (stream_) {
        const auto result=AAudioStream_close(stream_);
        stream_=nullptr;
        if (result!=AAUDIO_OK) lastError_=result;
    }
}
bool AAudioOutputStream::requestStart() noexcept {
    if (!stream_ || AAudioStream_getState(stream_)!=AAUDIO_STREAM_STATE_OPEN || !configuration()) return false;
    lastError_=AAudioStream_requestStart(stream_);return lastError_==AAUDIO_OK;
}
bool AAudioOutputStream::started() const noexcept {
    return stream_ && AAudioStream_getState(stream_)==AAUDIO_STREAM_STATE_STARTED;
}
std::int32_t AAudioOutputStream::writeStereo(const float* data,std::uint32_t frames) noexcept {
    if (!data || !frames || frames>AudioFramesPerPacket) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (!configuration()) return AAUDIO_ERROR_INVALID_STATE;
    for(std::size_t i=0;i<frames*2;++i)
        if (!std::isfinite(data[i]) || data[i]<-1 || data[i]>1) return AAUDIO_ERROR_OUT_OF_RANGE;
    // Open streams may be primed. Started streams obey real AAudio buffer
    // backpressure. Never block a serialized guest task on native audio I/O.
    const auto result=AAudioStream_write(stream_,data,static_cast<std::int32_t>(frames),0);
    if (result<0) lastError_=result;
    return result;
}
std::optional<std::uint64_t> AAudioOutputStream::framesWritten() const noexcept {
    if (!configuration()) return std::nullopt;
    const auto frames=AAudioStream_getFramesWritten(stream_);
    return frames<0?std::nullopt:std::optional{static_cast<std::uint64_t>(frames)};
}
std::optional<std::uint64_t> AAudioOutputStream::framesRead() const noexcept {
    if (!configuration()) return std::nullopt;
    const auto frames=AAudioStream_getFramesRead(stream_);
    return frames<0?std::nullopt:std::optional{static_cast<std::uint64_t>(frames)};
}
std::optional<bootstrap::NativeAudioConfiguration> AAudioOutputStream::configuration() const noexcept {
    if (!stream_) return std::nullopt;
    const auto state=AAudioStream_getState(stream_);
    if ((state!=AAUDIO_STREAM_STATE_OPEN && state!=AAUDIO_STREAM_STATE_STARTING && state!=AAUDIO_STREAM_STATE_STARTED) ||
        AAudioStream_getDirection(stream_)!=AAUDIO_DIRECTION_OUTPUT ||
        AAudioStream_getFormat(stream_)!=AAUDIO_FORMAT_PCM_FLOAT) return std::nullopt;
    // No hidden producer or automatic restart. Opening alone never starts the
    // stream; disconnected/stopping/closed streams are not usable evidence.
    const auto rate=AAudioStream_getSampleRate(stream_), channels=AAudioStream_getChannelCount(stream_);
    const auto capacity=AAudioStream_getBufferCapacityInFrames(stream_), burst=AAudioStream_getFramesPerBurst(stream_);
    if (rate<=0 || channels<=0 || capacity<=0 || burst<=0) return std::nullopt;
    bootstrap::NativeAudioConfiguration c{static_cast<std::uint32_t>(rate),static_cast<std::uint32_t>(channels),
        static_cast<std::uint32_t>(capacity),static_cast<std::uint32_t>(burst)};
    return bootstrap::validNativeAudioConfiguration(c)?std::optional{c}:std::nullopt;
}
}
