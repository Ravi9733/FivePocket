#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::private_trace {
inline bool requestId(const char* value) noexcept {
    if (!value) return false;
    for (unsigned i = 0; i < 32; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return value[32] == '\0';
}

// The binary manifest specification is not included in the public checkout.
// Reject unknown formats instead of interpreting untrusted bytes as argv.
inline bool parseArguments(const std::uint8_t*, std::size_t,
        const std::string&, const char*, std::vector<std::string>&) {
    return false;
}
}
