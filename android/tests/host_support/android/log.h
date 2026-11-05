#pragma once
// HOST TEST STAND-IN for the NDK logging header, so attempt sources that log through
// __android_log_print can be unit-tested on the Mac. Never on a device include path.
// Lines are captured by android_log_capture.cpp (see host_log_capture.h).
#ifdef __cplusplus
extern "C" {
#endif
enum {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT
};
int __android_log_print(int priority, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif
