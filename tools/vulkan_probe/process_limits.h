#pragma once
#include <sys/resource.h>
#include <unistd.h>

namespace recomp::gpu {
// Resource guard, not a sandbox or GPU-driver recovery mechanism.
inline bool initializeProbeProcess() {
    const rlimit noCore{0, 0};
    const rlimit cpu{10, 11};
    const rlimit noOutputFiles{0, 0};
    rlimit descriptors{};
    if (getrlimit(RLIMIT_NOFILE, &descriptors)) return false;
    if (descriptors.rlim_cur > 64) descriptors.rlim_cur = 64;
    if (setrlimit(RLIMIT_CORE, &noCore) || setrlimit(RLIMIT_CPU, &cpu) ||
        setrlimit(RLIMIT_FSIZE, &noOutputFiles) || setrlimit(RLIMIT_NOFILE, &descriptors)) return false;
    alarm(30);
    return true;
}
}
