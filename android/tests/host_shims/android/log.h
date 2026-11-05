#pragma once
// Host-only shim for Mac/Linux unit tests of Android runtime sources that log
// through liblog (for example the diagnostic attempt copy of
// guest_import_registry.cpp). Never on the Android include path: the NDK's real
// <android/log.h> must win there. Output is discarded unless
// RECOMP_HOST_ANDROID_LOG is set in the environment, so diagnostic logging in the
// code under test cannot change test timing or flood test output.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT,
};

inline int __android_log_print(int priority, const char* tag, const char* format, ...) {
    static const bool enabled = std::getenv("RECOMP_HOST_ANDROID_LOG") != nullptr;
    if (!enabled) return 0;
    std::fprintf(stderr, "[%d %s] ", priority, tag ? tag : "");
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    return written;
}
