#pragma once
// Host tests: every __android_log_print line (tag and formatted text), thread-safe.
#include <string>
#include <vector>

namespace recomp::testing {
struct HostLogLine { std::string tag, text; };
std::vector<HostLogLine> hostLogLines();
void clearHostLogLines();
// Also echo captured lines to stderr (default off).
void echoHostLogLines(bool enabled);
}
