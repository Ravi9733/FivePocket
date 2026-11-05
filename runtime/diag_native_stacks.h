#pragma once
// v199: native stacks of every thread in this process, for freeze forensics (no root, no debuggerd).
//
// A process-wide real-time signal handler walks the interrupted thread's frame-pointer chain (x29/x30 on arm64;
// the app, the recompiled PPC code and bionic keep frame records). Every stack word is read through a pipe
// (write(2) returns EFAULT for an unreadable address instead of raising SIGSEGV), so a broken chain ends the
// walk and never faults. The collector signals one thread at a time and waits up to 100 ms for its capture; a
// thread that has the signal blocked is reported as a timeout, and its late handler run finds a different target
// tid and returns. Nothing here runs unless a freeze dump asks for it.
#if defined(__aarch64__) && defined(__linux__)
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

namespace recomp::diag::nativestacks {

constexpr int MaxFrames = 48;

struct Capture {
    std::atomic<int> target{0};
    std::atomic<int> done{0};
    int count = 0;
    std::uintptr_t pcs[MaxFrames]{};
};

inline Capture& capture() { static Capture c; return c; }
inline int* pipeFds() { static int fds[2] = {-1, -1}; return fds; }

// Async-signal-safe: copies 16 bytes at `address` through the pipe, false when the address is unreadable.
inline bool readFrameRecord(std::uintptr_t address, std::uintptr_t out[2]) noexcept {
    const int* fds = pipeFds();
    const auto written = ::write(fds[1], reinterpret_cast<const void*>(address), 16);
    if (written != 16) { // a record straddling an unreadable page writes a prefix: drain it
        if (written > 0) { char junk[16]; (void)!::read(fds[0], junk, static_cast<std::size_t>(written)); }
        return false;
    }
    return ::read(fds[0], out, 16) == 16;
}

// Return addresses saved by pointer-authenticating code (Android system libraries on ARMv8.3+) carry a signature in
// their upper bits: xpaclri (a hint, a no-op without FEAT_PAuth) strips it.
inline std::uintptr_t stripPac(std::uintptr_t value) noexcept {
    register std::uintptr_t lr asm("x30") = value;
    asm("hint #7" : "+r"(lr));
    return lr;
}

inline void walk(std::uintptr_t pc, std::uintptr_t lr, std::uintptr_t fp, std::uintptr_t sp, std::uintptr_t* pcs,
                 int& n) noexcept {
    n = 0;
    pcs[n++] = pc;
    pcs[n++] = stripPac(lr); // the return address when the interrupted function is a leaf that has not pushed a record
    while (n < MaxFrames && fp != 0 && (fp & 7) == 0 && fp >= sp && fp - sp < (std::uintptr_t{256} << 20)) {
        std::uintptr_t record[2];
        if (!readFrameRecord(fp, record) || record[1] == 0) break;
        pcs[n++] = stripPac(record[1]);
        if (record[0] <= fp) break; // frame records move toward older frames at higher addresses
        fp = record[0];
    }
}

inline void handler(int, siginfo_t*, void* context) noexcept {
    auto& c = capture();
    if (c.target.load(std::memory_order_acquire) != static_cast<int>(::gettid())) return;
    const int saved = errno;
    const auto& m = static_cast<ucontext_t*>(context)->uc_mcontext;
    int n = 0;
    walk(m.pc, m.regs[30], m.regs[29], m.sp, c.pcs, n);
    c.count = n;
    c.done.store(1, std::memory_order_release);
    errno = saved;
}

inline int signalNumber() { return SIGRTMIN + 9; }

// Installs the handler once; false when the signal already has a handler (never replaces one).
inline bool install() {
    static const bool ok = [] {
        int* fds = pipeFds();
        if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return false;
        struct sigaction old {};
        if (::sigaction(signalNumber(), nullptr, &old) != 0) return false;
        if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler != SIG_DFL) return false;
        if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction != nullptr) return false;
        struct sigaction action {};
        action.sa_sigaction = handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        return ::sigaction(signalNumber(), &action, nullptr) == 0;
    }();
    return ok;
}

// Captures one thread's stack into pcs; returns the frame count, 0 on timeout, -1 when the thread is gone.
inline int captureThread(int tid, std::uintptr_t* pcs, int maximum) {
    if (!install()) return -1;
    auto& c = capture();
    c.done.store(0, std::memory_order_relaxed);
    c.count = 0;
    c.target.store(tid, std::memory_order_release);
    if (::syscall(SYS_tgkill, ::getpid(), tid, signalNumber()) != 0) {
        c.target.store(0, std::memory_order_release);
        return -1;
    }
    for (int i = 0; i < 100 && !c.done.load(std::memory_order_acquire); ++i) ::usleep(1000);
    const bool done = c.done.load(std::memory_order_acquire);
    c.target.store(0, std::memory_order_release);
    if (!done) return 0;
    const int n = c.count < maximum ? c.count : maximum;
    for (int i = 0; i < n; ++i) pcs[i] = c.pcs[i];
    return n;
}

// "@offset" for this library, "name+offset" for another object, the raw address otherwise.
inline int formatFrame(char* out, std::size_t size, std::uintptr_t pc) {
    static Dl_info self{};
    static const bool haveSelf = ::dladdr(reinterpret_cast<const void*>(&formatFrame), &self) != 0;
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(pc), &info) && info.dli_fname) {
        const auto offset = pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        if (haveSelf && info.dli_fbase == self.dli_fbase) return std::snprintf(out, size, "@%lx", static_cast<unsigned long>(offset));
        const char* base = std::strrchr(info.dli_fname, '/');
        return std::snprintf(out, size, "%s+%lx", base ? base + 1 : info.dli_fname, static_cast<unsigned long>(offset));
    }
    return std::snprintf(out, size, "%lx", static_cast<unsigned long>(pc));
}

// Calls emit(line) once per thread of this process except the caller. Returns the number of threads captured.
template <class Emit>
int dumpAll(Emit&& emit) {
    if (!install()) { emit("RENDER_DIAG_NATIVE_STACK unavailable=1"); return 0; }
    DIR* dir = ::opendir("/proc/self/task");
    if (!dir) return 0;
    const int self = static_cast<int>(::gettid());
    int captured = 0;
    while (dirent* entry = ::readdir(dir)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0 || tid == self) continue;
        char comm[32] = "?";
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        if (const int fd = ::open(path, O_RDONLY | O_CLOEXEC); fd >= 0) {
            const auto got = ::read(fd, comm, sizeof comm - 1);
            comm[got > 0 ? got : 0] = 0;
            if (char* nl = std::strchr(comm, '\n')) *nl = 0;
            for (char* p = comm; *p; ++p) if (*p == ' ') *p = '_';
            ::close(fd);
        }
        std::uintptr_t pcs[MaxFrames];
        const int n = captureThread(tid, pcs, MaxFrames);
        char line[3072];
        int w = std::snprintf(line, sizeof line, "RENDER_DIAG_NATIVE_STACK tid=%d comm=%s n=%d frames=", tid, comm, n);
        for (int i = 0; i < n && w > 0 && w < static_cast<int>(sizeof line) - 48; ++i) {
            if (i) line[w++] = ',';
            w += formatFrame(line + w, sizeof line - w, pcs[i]);
        }
        emit(line);
        if (n > 0) ++captured;
    }
    ::closedir(dir);
    return captured;
}

} // namespace recomp::diag::nativestacks
#endif
