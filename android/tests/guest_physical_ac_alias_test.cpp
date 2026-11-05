#include "guest_address_space.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#if defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif
using recomp::bootstrap::GuestAddressSpace;

int main() {
    static_assert(sizeof(GuestAddressSpace)==sizeof(void*)); // No shared-class ABI expansion.
#if defined(__ANDROID__)
    // Reproduce the JNI app policy, not the unconstrained adb shell environment.
    // Old memfd/ftruncate must fail, then the real mapping/copy must succeed.
    struct sigaction action{};action.sa_handler=SIG_IGN;sigemptyset(&action.sa_mask);
    assert(sigaction(SIGXFSZ,&action,nullptr)==0);
    const rlimit fileLimit{65536,65536};assert(setrlimit(RLIMIT_FSIZE,&fileLimit)==0);
    const int oldFd=static_cast<int>(syscall(SYS_memfd_create,"old-ac-counterexample",MFD_CLOEXEC));
    assert(oldFd>=0);errno=0;
    assert(ftruncate(oldFd,0x20000000)==-1 && errno==EFBIG);close(oldFd);
#endif
    GuestAddressSpace flat;
    assert(flat.reserve());
    flat.translate(0xBF6A23A0)[0]=0x53;
    assert(flat.translate(0xDF6A23A0)[0]==0); // Reproduce the actual missing view.
    assert(!flat.reserveWithPhysicalAcAlias()); // Preserve already loaded data.
    assert(flat.translate(0xBF6A23A0)[0]==0x53);
    flat.release();
    GuestAddressSpace coherent;
#if defined(__linux__) || defined(__APPLE__)
    assert(coherent.reserveWithPhysicalAcAlias());
    for(auto offset:{0u,0xFFFFu,0x10000u,0x1F6A23A0u,0x1FFFFFFFu}) {
        auto* a=coherent.base()+0xA0000000+offset;
        auto* c=coherent.base()+0xC0000000+offset;
        *a=0x53;assert(*c==0x53);*c=0xA7;assert(*a==0xA7);
    }
    std::array<unsigned char,240> source{};
    for(unsigned i=0;i<source.size();++i)source[i]=static_cast<unsigned char>(i*73+19);
    constexpr unsigned A=0xBF6A23A0,C=0xDF6A23A0,Dst=0xA791F0B4;
    std::memcpy(coherent.base()+A,source.data(),source.size());
    std::memcpy(coherent.base()+Dst,coherent.base()+C,source.size());
    assert(std::memcmp(coherent.base()+Dst,source.data(),source.size())==0);
    assert(std::memcmp(coherent.base()+Dst+0x20000000,source.data(),source.size())==0);
    assert(!coherent.reserveWithPhysicalAcAlias());
    assert(std::memcmp(coherent.base()+Dst,source.data(),source.size())==0);
    coherent.release();assert(!coherent.isReserved());
    assert(coherent.reserveWithPhysicalAcAlias());
    assert(coherent.translate(A)[0]==0 && coherent.translate(C)[0]==0);
#if defined(__ANDROID__)
    rlimit observed{};assert(getrlimit(RLIMIT_FSIZE,&observed)==0);
    assert(observed.rlim_cur==65536 && observed.rlim_max==65536);
    std::puts("PHYSICAL_AC_APP_LIMIT_PASS old_memfd_rejected=1 shared_alias=1 file_limit=65536");
#endif
    std::puts("PHYSICAL_AC_ALIAS_PASS bidirectional=1 raw_copy=1 boundary_pages=1 fresh_only=1");
#else
    assert(!coherent.reserveWithPhysicalAcAlias() && !coherent.isReserved());
    std::puts("PHYSICAL_AC_ALIAS_UNAVAILABLE explicit=1");
#endif
}
