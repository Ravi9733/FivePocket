#include "guest_address_space.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/sharedmem.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <linux/memfd.h>
#elif defined(__APPLE__)
#include <cstdlib>
#include <cstdio>
#endif
#endif

namespace recomp::bootstrap {

GuestAddressSpace::~GuestAddressSpace() {
    release();
}

bool GuestAddressSpace::reserve() noexcept {
    if (base_ != nullptr) {
        return true;
    }

#if defined(_WIN32)
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr,
            static_cast<SIZE_T>(Size),
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE));
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* mapping = mmap(
            nullptr,
            static_cast<std::size_t>(Size),
            PROT_READ | PROT_WRITE,
            flags,
            -1,
            0);
    if (mapping != MAP_FAILED) {
        base_ = static_cast<std::uint8_t*>(mapping);
    }
#endif
    return base_ != nullptr;
}

bool GuestAddressSpace::reserveWithPhysicalAcAlias() noexcept {
    // The old flat reservation made the uncached C/D view unrelated zero RAM.
    // Original guest scalar AND raw SIMD accesses use base+address, so pointer
    // masking in a single memcpy wrapper would not establish coherent views.
    if(base_!=nullptr)return false;
#if defined(__linux__) || defined(__APPLE__)
    constexpr std::size_t PhysicalBytes=0x20000000;
    int fd=-1;
#if defined(__ANDROID__)
    // The private trace app keeps RLIMIT_FSIZE=64KiB. Growing a Linux memfd
    // with ftruncate therefore fails EFBIG even though it is anonymous RAM.
    // Use the platform shared-memory allocator; never raise the file limit.
    fd=ASharedMemory_create("recomp-physical-ac",PhysicalBytes);
    if(fd>=0 && (ASharedMemory_getSize(fd)!=PhysicalBytes ||
            ASharedMemory_setProt(fd,PROT_READ|PROT_WRITE)!=0 ||
            fcntl(fd,F_SETFD,FD_CLOEXEC)!=0)){close(fd);return false;}
#elif defined(__linux__)
    fd=static_cast<int>(syscall(SYS_memfd_create,"recomp-physical-ac",MFD_CLOEXEC));
#else
    // Anonymous after unlink: no persistent guest RAM file or reusable name.
    char name[64];
    std::snprintf(name,sizeof(name),"/recomp-ac-%ld-%08x",static_cast<long>(getpid()),arc4random());
    fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    if(fd>=0 && shm_unlink(name)!=0){close(fd);return false;}
#endif
    if(fd<0)return false;
#if !defined(__ANDROID__)
    if(ftruncate(fd,PhysicalBytes)!=0){close(fd);return false;}
#endif
    if(!reserve()){close(fd);return false;}
    const auto map=[&](std::uint32_t start) {
        void* expected=base_+start;
        return mmap(expected,PhysicalBytes,PROT_READ|PROT_WRITE,
            MAP_SHARED|MAP_FIXED,fd,0)==expected;
    };
    // MAP_FIXED targets only subranges of this new, exclusively owned reserve.
    const bool ok=map(0xA0000000) && map(0xC0000000);
    close(fd);
    if(!ok)release();
    return ok;
#else
    // The ordinary Windows reserve remains available. Its aliased-view backend
    // requires placeholder/file mappings; don't silently offer flat RAM here.
    return false;
#endif
}

void GuestAddressSpace::release() noexcept {
    if (base_ == nullptr) {
        return;
    }

#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, static_cast<std::size_t>(Size));
#endif
    base_ = nullptr;
}

bool GuestAddressSpace::isReserved() const noexcept {
    return base_ != nullptr;
}

std::uint8_t* GuestAddressSpace::base() const noexcept {
    return base_;
}

std::uint8_t* GuestAddressSpace::translate(std::uint32_t guestAddress) const noexcept {
    return base_ == nullptr ? nullptr : base_ + guestAddress;
}

} // namespace recomp::bootstrap
