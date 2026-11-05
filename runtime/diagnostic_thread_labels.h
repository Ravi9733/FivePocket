#pragma once
// DIAGNOSTIC (attempt v12, multicore plan WP4): the trace sink's per-thread function-label records
// (RENDER_DIAG_THREAD / RENDER_DIAG_PROFILE) and its most-recent function-label ring
// (RENDER_DIAG_LABELS and the final function_label= lines). Safe for many concurrent guest threads:
// - ThreadLabelBook: one record per guest token, claimed by a CAS on the token. A claimed slot is
//   never released, so a token keeps its slot and each host thread caches it. The owner updates
//   its record under the record's spin lock; a dump copies it under the same lock and formats and
//   logs outside it.
// - FunctionLabelRing: a global sequence number and atomic slots. Labels are views of
//   NUL-terminated static strings (__func__ or literals); readers bound each one with strnlen, so a
//   slot overwritten during a mid-run dump can never be over-read.
// With one guest thread at a time (the cooperative executor) the records, dump lines and the final
// ring are the same as the former plain arrays; trace_diagnostics_concurrency_test compares both.
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace diagnostic_skip {

// Bounded spin, then yield. Held for a few stores (owner) or one record copy (dump).
class DiagnosticSpinLock final {
public:
    void lock() noexcept {
        unsigned spins = 0;
        while (flag_.exchange(true, std::memory_order_acquire))
            while (flag_.load(std::memory_order_relaxed))
                if (++spins > 64) std::this_thread::yield();
    }
    void unlock() noexcept { flag_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> flag_{false};
};

class ThreadLabelBook final {
public:
    static constexpr unsigned Slots = 128, RecentLabels = 12;
    // Attempt v9: sample 1 entry in 8 per host thread; entry counts stay in units of real entries.
    static bool sample() noexcept {
        static thread_local unsigned sampled = 0;
        return !(++sampled & 7);
    }
    void record(std::uint32_t token, std::string_view label) {
        auto* t = find(token);
        if (!t) return;
        const bool skipAnchor = anchorFiltered(label);
        std::lock_guard guard(t->lock);
        t->entries += 8;
        t->labels[t->next] = label;
        t->next = (t->next + 1) % RecentLabels;
        // Attempt v2: where a busy thread spends its entries, every 256th entry, per dump interval.
        if (!(t->entries & 255) && label.data()) {
            auto& slot = t->profile[label.data()];
            slot.first = label;
            ++slot.second;
        }
        if (!skipAnchor && t->anchor.data() != label.data()) { t->anchorPrevious = t->anchor; t->anchor = label; } // DIAGNOSTIC v699
    }
    // One RENDER_DIAG_THREAD line per record, in slot order, plus RENDER_DIAG_PROFILE for a record
    // with more than 2^20 entries since the previous dump. Restarts every record's profile.
    void dump() {
        for (auto& t : records_) {
            const auto token = t.token.load(std::memory_order_acquire);
            if (!token) continue;
            unsigned long long entries = 0, reported = 0;
            unsigned next = 0;
            std::array<std::string_view, RecentLabels> labels{};
            std::string_view anchor, previous;
            std::vector<std::pair<unsigned, std::string_view>> top;
            unsigned samples = 0;
            bool profiled = false;
            {
                std::lock_guard guard(t.lock);
                entries = t.entries; reported = t.reported; next = t.next; labels = t.labels;
                anchor = t.anchor; previous = t.anchorPrevious;
                if (entries - reported > (1u << 20) && !t.profile.empty()) {
                    profiled = true;
                    for (const auto& [key, entry] : t.profile) { (void)key; top.emplace_back(entry.second, entry.first); samples += entry.second; }
                }
                t.profile.clear();
                t.reported = entries;
            }
            std::string recent;
            for (unsigned k = 0; k < RecentLabels; ++k) { recent += labels[(next + k) % RecentLabels]; recent += ' '; }
            const std::string anchorText(anchor), previousText(previous);
            __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_THREAD token=%08x new=%llu total=%llu anchor=%s<-%s recent=%s",
                token, entries - reported, entries, anchorText.c_str(), previousText.c_str(), recent.c_str());
            if (!profiled) continue;
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string hot;
            for (std::size_t k = 0; k < top.size() && k < 10; ++k) { hot += top[k].second; hot += ':'; hot += std::to_string(top[k].first); hot += ' '; }
            __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_PROFILE token=%08x samples=%u top=%s", token, samples, hot.c_str());
        }
    }
private:
    struct alignas(64) Record {
        std::atomic<std::uint32_t> token{0};
        DiagnosticSpinLock lock;
        unsigned long long entries = 0, reported = 0;
        unsigned next = 0;
        std::array<std::string_view, RecentLabels> labels{};
        std::string_view anchor, anchorPrevious;
        std::unordered_map<const char*, std::pair<std::string_view, unsigned>> profile{};
    };
    Record* find(std::uint32_t token) noexcept {
        struct Cache { std::uint64_t book = 0; std::uint32_t token = 0; Record* record = nullptr; };
        static thread_local Cache cache;
        if (cache.book == id_ && cache.token == token) return cache.record;
        unsigned i = (token >> 12) & (Slots - 1);
        for (unsigned probe = 0; probe < Slots; ++probe, i = (i + 1) & (Slots - 1)) {
            auto& t = records_[i];
            auto current = t.token.load(std::memory_order_acquire);
            if (!current && t.token.compare_exchange_strong(current, token, std::memory_order_acq_rel, std::memory_order_acquire))
                current = token;
            if (current == token) { cache = {id_, token, &t}; return &t; }
        }
        return nullptr; // All slots taken: not recorded, as before.
    }
    // Attempt v3: cheap anchor filter. Register save/restore helpers are "__imp____..." (a '_'
    // right after "__imp__"); the three watchdog helpers are matched once by text, then by the
    // address of their static label.
    bool anchorFiltered(std::string_view label) noexcept {
        if (label.size() > 8 && label[7] == '_') return true;
        if (label.size() != 19 || label[7] != 's') return false;
        for (const auto& w : watchdog_) if (w.load(std::memory_order_relaxed) == label.data()) return true;
        if (label != "__imp__sub_82220140" && label != "__imp__sub_82220130" && label != "__imp__sub_8251E820") return false;
        for (auto& w : watchdog_) {
            const char* expected = nullptr;
            if (w.compare_exchange_strong(expected, label.data(), std::memory_order_relaxed) || expected == label.data()) break;
        }
        return true;
    }
    static std::uint64_t nextId() noexcept {
        static std::atomic<std::uint64_t> id{0};
        return id.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    const std::uint64_t id_ = nextId();
    std::array<Record, Slots> records_{};
    std::array<std::atomic<const char*>, 3> watchdog_{};
};

class FunctionLabelRing final {
public:
    // cadence: the RENDER_DIAG_LABELS trigger fires every `cadence` dropped labels (a power of two).
    explicit FunctionLabelRing(std::size_t limit, std::uint64_t cadence = std::uint64_t{1} << 26)
        : limit_(limit), cadenceMask_(cadence - 1),
          slots_(limit ? std::make_unique<Slot[]>(limit) : nullptr) {}
    // Records one label. Returns the dropped count when this label overwrote the oldest one and
    // that count is a multiple of the cadence, else 0.
    std::uint64_t record(std::string_view label) noexcept {
        if (!limit_) return 0;
        const auto n = total_.fetch_add(1, std::memory_order_relaxed);
        auto& slot = slots_[n % limit_];
        slot.size.store(static_cast<std::uint32_t>(label.size()), std::memory_order_relaxed);
        slot.data.store(label.data(), std::memory_order_relaxed);
        if (n < limit_) return 0;
        const auto dropped = n - limit_ + 1;
        return (dropped & cadenceMask_) ? 0 : dropped;
    }
    std::uint64_t dropped() const noexcept {
        const auto total = total_.load(std::memory_order_relaxed);
        return total > limit_ ? total - limit_ : 0;
    }
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(std::min<std::uint64_t>(total_.load(std::memory_order_relaxed), limit_));
    }
    // The i-th oldest label. Exact once every writer has joined.
    std::string_view at(std::size_t i) const noexcept {
        const auto total = total_.load(std::memory_order_relaxed);
        const auto first = total > limit_ ? total % limit_ : 0;
        return label(slots_[(first + i) % limit_]);
    }
    // The `count` labels ending with the one whose record() returned `dropped`, oldest first, each
    // followed by ' '. Empty when the ring has fewer than `count` slots, as before.
    std::string recentAtDrop(std::uint64_t dropped, unsigned count) const {
        std::string text;
        if (!limit_ || limit_ < count) return text;
        const auto newest = dropped + limit_ - 1;
        for (unsigned k = 0; k < count; ++k) { text += label(slots_[(newest - (count - 1) + k) % limit_]); text += ' '; }
        return text;
    }
private:
    struct Slot {
        std::atomic<const char*> data{nullptr};
        std::atomic<std::uint32_t> size{0};
    };
    static std::string_view label(const Slot& slot) noexcept {
        const auto* data = slot.data.load(std::memory_order_relaxed);
        const auto size = slot.size.load(std::memory_order_relaxed);
        return data ? std::string_view(data, strnlen(data, size)) : std::string_view{};
    }
    const std::uint64_t limit_;
    const std::uint64_t cadenceMask_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::uint64_t> total_{0};
};

} // namespace diagnostic_skip
