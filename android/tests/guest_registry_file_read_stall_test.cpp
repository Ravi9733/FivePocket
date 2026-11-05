// Multicore plan WP2: NtReadFile performs its host read without the registry
// mutex when RECOMP_GUEST_THREADS > 0 (open call). A slow source makes the window
// observable: an unrelated import from another host thread must complete during
// the read. The cooperative build keeps today's behaviour, which this test pins as
// the documented stall. Parallel-only cases cover the publish-under-lock rules:
// a handle closed during the read, a concurrent seek of an implicit-position read,
// and a destination freed during the read.
#include "guest_import_registry.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>

using namespace recomp::bootstrap;
using namespace std::chrono_literals;
namespace {
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS > 0
constexpr bool Parallel = true;
#else
constexpr bool Parallel = false;
#endif
constexpr std::uint32_t Meta = 0x30000000, Name = Meta + 32, Text = Meta + 64, Out = Meta + 128,
    Iosb = Meta + 144, Offset = Meta + 160, Query = Meta + 176, Seek = Meta + 192, SeekIosb = Meta + 208,
    Stack = Meta + 4096;
constexpr std::uint32_t Bytes = 256 * 1024;
constexpr auto SlowRead = 150ms;

class SlowSource final : public GuestReadOnlyFile {
public:
    std::uint64_t size() const noexcept override { return 4 * 1024 * 1024; }
    bool read(std::uint64_t offset, std::uint8_t* out, std::size_t bytes) const noexcept override {
        {
            std::lock_guard l(m_); ++calls_; lastOffset_ = offset;
            if (slow_) { reading_ = true; cv_.notify_all(); }
        }
        for (std::size_t i = 0; i < bytes; ++i) out[i] = static_cast<std::uint8_t>((offset + i) % 251);
        if (slow_) std::this_thread::sleep_for(SlowRead);
        return !fail_;
    }
    void configure(bool slow, bool fail) { std::lock_guard l(m_); slow_ = slow; fail_ = fail; reading_ = false; calls_ = 0; }
    void awaitReading() const { std::unique_lock l(m_); cv_.wait(l, [&] { return reading_; }); }
    unsigned calls() const { std::lock_guard l(m_); return calls_; }
    std::uint64_t lastOffset() const { std::lock_guard l(m_); return lastOffset_; }
private:
    mutable std::mutex m_; mutable std::condition_variable cv_;
    mutable bool reading_{false}; mutable unsigned calls_{0}; mutable std::uint64_t lastOffset_{0};
    bool slow_{false}, fail_{false};
};

void put(GuestAddressSpace& s, std::uint32_t a, std::uint64_t v, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i) s.base()[a + i] = static_cast<std::uint8_t>(v >> ((size - i - 1) * 8));
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t a) {
    const auto* p = s.base() + a;
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
struct Result { GuestImportDispatchState state; std::uint64_t value; };
Result call(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args, std::uint32_t sp = 0) {
    GuestImportCall c;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    c.stackPointer = sp; c.returnValue = 0xABCDEF;
    const auto result = r.dispatch(name, c);
    return {result.state, c.returnValue};
}
std::uint64_t invoke(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args, std::uint32_t sp = 0) {
    const auto result = call(r, name, args, sp);
    assert(result.state == GuestImportDispatchState::Handled);
    return result.value;
}

struct Fixture {
    GuestAddressSpace& s;
    GuestImportRegistry r;
    std::shared_ptr<SlowSource> source = std::make_shared<SlowSource>();
    std::uint32_t handle{0}, buffer{0};
    explicit Fixture(GuestAddressSpace& space) : s(space) {
        assert(r.initialize(s, 0x10000000, 0x01000000));
        assert(r.bindFileMetadataQuery([this](auto) {
            return GuestFileQueryResult{GuestFileQueryState::Found, {{0, 0, 0, 0, source->size(), source->size()}, 1}};
        }));
        assert(r.bindReadOnlyFiles([this](auto) -> std::shared_ptr<const GuestReadOnlyFile> { return source; }));
        open();
        put(s, Query, 0); put(s, Query + 4, Bytes);
        assert(invoke(r, "__imp__NtAllocateVirtualMemory", {Query, Query + 4, 0x3000, 4, 0}) == 0);
        buffer = get(s, Query);
    }
    void open() {
        constexpr std::string_view path = "game:\\slow.bin";
        put(s, Meta, 0); put(s, Meta + 4, Name); put(s, Meta + 8, 0x40);
        put(s, Name, path.size(), 2); put(s, Name + 2, path.size(), 2); put(s, Name + 4, Text);
        std::memcpy(s.base() + Text, path.data(), path.size()); put(s, Stack + 0x54, 0x60);
        assert(invoke(r, "__imp__NtCreateFile", {Out, 0x80100080, Meta, Iosb, 0, 0x80, 1, 1}, Stack) == 0);
        handle = get(s, Out); assert(handle);
    }
    // offset < 0: implicit position (FILE_USE_FILE_POINTER_POSITION).
    Result read(std::int64_t offset, std::uint32_t length = Bytes) {
        put(s, Offset, offset < 0 ? 0xFFFFFFFFFFFFFFFEULL : static_cast<std::uint64_t>(offset), 8);
        return call(r, "__imp__NtReadFile", {handle, 0, 0, Iosb, Iosb, buffer, length, Offset});
    }
    std::uint64_t position() {
        assert(invoke(r, "__imp__NtQueryInformationFile", {handle, SeekIosb, Query, 8, 14}) == 0);
        return (std::uint64_t{get(s, Query)} << 32) | get(s, Query + 4);
    }
    bool bytesFrom(std::uint64_t offset, std::uint32_t length = Bytes) {
        for (std::uint32_t i = 0; i < length; ++i) if (s.base()[buffer + i] != (offset + i) % 251) return false;
        return true;
    }
};

// Another host thread's unrelated import during a slow read.
std::chrono::milliseconds stall(GuestAddressSpace& s) {
    Fixture f(s);
    f.source->configure(true, false);
    Result read{};
    std::thread reader([&] { read = f.read(4096); });
    f.source->awaitReading();
    const auto start = std::chrono::steady_clock::now();
    assert(invoke(f.r, "__imp__KeGetCurrentProcessType", {}) == 1);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    reader.join();
    assert(read.state == GuestImportDispatchState::Handled && read.value == 0);
    assert(get(s, Iosb) == 0 && get(s, Iosb + 4) == Bytes && f.bytesFrom(4096) && f.position() == 4096 + Bytes);
    return latency;
}

// A failed chunk still preserves destination, IOSB and cursor (both builds).
void failedChunkPreserves(GuestAddressSpace& s) {
    Fixture f(s);
    f.source->configure(false, false);
    assert(f.read(0, 64).value == 0 && f.position() == 64);
    std::fill_n(s.base() + f.buffer, Bytes, 0xA5); std::fill_n(s.base() + Iosb, 8, 0xCC);
    f.source->configure(false, true);
    assert(f.read(-1).state == GuestImportDispatchState::Rejected);
    assert(std::all_of(s.base() + f.buffer, s.base() + f.buffer + Bytes, [](auto b) { return b == 0xA5; }));
    assert(get(s, Iosb) == 0xCCCCCCCC && f.position() == 64);
}

// Parallel only: the handle is closed during the read. The I/O completes (as NT
// completes I/O already in progress) without a cursor; the handle stays closed.
void closedDuringRead(GuestAddressSpace& s) {
    Fixture f(s);
    f.source->configure(true, false);
    Result read{};
    std::thread reader([&] { read = f.read(8192); });
    f.source->awaitReading();
    assert(invoke(f.r, "__imp__NtClose", {f.handle}) == 0);
    reader.join();
    assert(read.state == GuestImportDispatchState::Handled && read.value == 0 && f.bytesFrom(8192));
    f.source->configure(false, false);
    assert(f.read(0).value == 0xC0000008); // STATUS_INVALID_HANDLE after the close.
}

// Parallel only: a seek during an implicit-position read re-serializes the read
// after the seek, so it reads from, and advances past, the new cursor.
void seekDuringImplicitRead(GuestAddressSpace& s) {
    Fixture f(s);
    f.source->configure(false, false);
    assert(f.read(0, 64).value == 0 && f.position() == 64);
    f.source->configure(true, false);
    Result read{};
    std::thread reader([&] { read = f.read(-1); });
    f.source->awaitReading();
    put(s, Seek, 1 << 20, 8);
    assert(invoke(f.r, "__imp__NtSetInformationFile", {f.handle, SeekIosb, Seek, 8, 14}) == 0);
    reader.join();
    assert(read.state == GuestImportDispatchState::Handled && read.value == 0);
    assert(f.source->lastOffset() >= (1u << 20) && f.bytesFrom(1 << 20) && f.position() == (1u << 20) + Bytes);
}

// Parallel only: the destination is freed during the read. Publication under the
// lock revalidates it and fails closed; IOSB and cursor are preserved.
void destinationFreedDuringRead(GuestAddressSpace& s) {
    Fixture f(s);
    f.source->configure(true, false);
    std::fill_n(s.base() + Iosb, 8, 0xCC);
    Result read{};
    std::thread reader([&] { read = f.read(0); });
    f.source->awaitReading();
    put(s, Query, f.buffer); put(s, Query + 4, 0);
    assert(invoke(f.r, "__imp__NtFreeVirtualMemory", {Query, Query + 4, 0x8000, 0}) == 0);
    reader.join();
    assert(read.state == GuestImportDispatchState::GuestMemoryFault && read.value == 0xABCDEF);
    assert(get(s, Iosb) == 0xCCCCCCCC);
    f.source->configure(false, false);
    assert(f.position() == 0);
}
} // namespace

int main() {
    GuestAddressSpace s; assert(s.reserve());
    const auto latency = stall(s);
    if (Parallel) assert(latency < SlowRead / 2);  // Import completed while the host read ran.
    else assert(latency >= SlowRead * 2 / 3);        // Documented cooperative stall behind the registry mutex.
    failedChunkPreserves(s);
    if (Parallel) { closedDuringRead(s); seekDuringImplicitRead(s); destinationFreedDuringRead(s); }
    std::printf("REGISTRY_FILE_READ_STALL_V1 mode=%s unrelated_import_latency_ms=%lld slow_read_ms=%lld"
        " failed_chunk_preserves=1 closed_during_read=%s seek_reserialized=%s freed_destination_fails_closed=%s\n",
        Parallel ? "parallel" : "cooperative", static_cast<long long>(latency.count()),
        static_cast<long long>(SlowRead.count()), Parallel ? "1" : "n/a", Parallel ? "1" : "n/a", Parallel ? "1" : "n/a");
}
