// HOST TEST STAND-IN: captures __android_log_print lines for format checks.
#include "android/log.h"
#include "host_log_capture.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex& captureLock() { static auto* lock = new std::mutex; return *lock; }
std::vector<recomp::testing::HostLogLine>& captured() {
    static auto* lines = new std::vector<recomp::testing::HostLogLine>;
    return *lines;
}
bool echo = false;
}

extern "C" int __android_log_print(int, const char* tag, const char* format, ...) {
    char text[4096];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    std::lock_guard lock(captureLock());
    captured().push_back({tag ? tag : "", text});
    if (echo) std::fprintf(stderr, "%s: %s\n", tag ? tag : "", text);
    return length;
}

namespace recomp::testing {
std::vector<HostLogLine> hostLogLines() { std::lock_guard lock(captureLock()); return captured(); }
void clearHostLogLines() { std::lock_guard lock(captureLock()); captured().clear(); }
void echoHostLogLines(bool enabled) { std::lock_guard lock(captureLock()); echo = enabled; }
}
