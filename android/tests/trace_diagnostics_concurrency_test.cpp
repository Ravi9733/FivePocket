// Tests for the trace sink's diagnostics (local-analysis/attempts/v12/src/diagnostic_thread_labels.h).
// 1. Cooperative equivalence: the former plain-array code (copied below as the reference) and the
//    new ThreadLabelBook/FunctionLabelRing see one deterministic interleaving of guest tokens and
//    labels on one host thread, with dumps in between. Every RENDER_DIAG_THREAD / _PROFILE line,
//    every RENDER_DIAG_LABELS trigger and text, and the final ring must be byte-identical.
// 2. Concurrency: 16 guest-like threads run the sink (sampled records plus the ring) while another
//    thread dumps. Totals, anchors and the ring are checked after join. Build with ThreadSanitizer
//    (see host_support/run_wp4_host_tests.sh).
// -DTRACE_DIAGNOSTICS_LEGACY_CONCURRENCY=1 builds a negative control instead: part 2's workload
// against the former code, which is expected to fail (lost entries or a crash).
#include "diagnostic_thread_labels.h"
#include "host_log_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
[[noreturn]] void failTest(const char* what, unsigned line) {
    std::fprintf(stderr, "TRACE_DIAGNOSTICS_FAIL line=%u %s\n", line, what);
    std::abort();
}
#define CHECK(condition) do { if (!(condition)) failTest(#condition, __LINE__); } while (0)

// Static labels, as the generated prologue passes __func__: ordinary functions, register helpers
// (filtered from the anchor), the three watchdog helpers, and an unnamed checkpoint.
const char* const Labels[] = {
    "__imp__sub_823BC370", "__imp__sub_823BA300", "__imp__sub_83642BA0", "__imp__sub_823C7A68",
    "__imp__sub_836BE1C8", "__imp__sub_836B1768", "__imp____savegprlr_29", "__imp____restgprlr_28",
    "__imp__sub_82220140", "__imp__sub_82220130", "__imp__sub_8251E820", "native_audio_callback_demand",
    "__imp__sub_83670A18", "__imp__sub_8364E530", nullptr,
};
constexpr unsigned LabelCount = sizeof(Labels) / sizeof(Labels[0]);
std::string_view label(unsigned i) {
    const char* text = Labels[i % LabelCount];
    return text ? std::string_view(text) : std::string_view{};
}

// ---- Reference: the previous trace_runner.cpp code, verbatim except for the explicit token ----
namespace legacy {
struct ThreadLabels {std::uint32_t token=0;unsigned long long entries=0,reported=0;unsigned next=0;std::array<std::string_view,12> labels{};std::string_view anchor,anchorPrevious;
    std::unordered_map<const char*,std::pair<std::string_view,unsigned>> profile{}; // every 256th entry, per dump interval
};
std::array<ThreadLabels,128> threadLabels{};
void recordThreadLabel(std::uint32_t token,std::string_view label) {
    static thread_local unsigned sampled=0;
    if(++sampled&7)return;
    if(!token)return;
    unsigned i=(token>>12)&127;
    for(unsigned probe=0;probe<128;++probe,i=(i+1)&127) {
        auto& t=threadLabels[i];
        if(t.token==token || !t.token){t.token=token;t.entries+=8;t.labels[t.next]=label;t.next=(t.next+1)%12;
            if(!(t.entries&255) && label.data()){auto& slot=t.profile[label.data()];slot.first=label;++slot.second;}
            static const char* watchdog[3]{};
            const bool helper=label.size()>8 && label[7]=='_';
            bool skip=helper;
            if(!skip && label.size()==19 && label[7]=='s') {
                for(const char* w:watchdog)skip|=w==label.data();
                if(!skip && (label=="__imp__sub_82220140" || label=="__imp__sub_82220130" || label=="__imp__sub_8251E820")) {
                    for(auto& w:watchdog)if(!w){w=label.data();break;}
                    skip=true;
                }
            }
            if(!skip && t.anchor.data()!=label.data()){t.anchorPrevious=t.anchor;t.anchor=label;}
            return;}
    }
}
void logThreads() {
    for(auto& t:threadLabels)if(t.token) {
        std::string recent;for(unsigned k=0;k<12;++k){recent+=t.labels[(t.next+k)%12];recent+=' ';}
        const std::string anchor(t.anchor),previous(t.anchorPrevious);
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_THREAD token=%08x new=%llu total=%llu anchor=%s<-%s recent=%s",
            t.token,t.entries-t.reported,t.entries,anchor.c_str(),previous.c_str(),recent.c_str());
        if(t.entries-t.reported>(1u<<20) && !t.profile.empty()) {
            std::vector<std::pair<unsigned,std::string_view>> top;unsigned samples=0;
            for(const auto& [key,entry]:t.profile){(void)key;top.emplace_back(entry.second,entry.first);samples+=entry.second;}
            std::sort(top.begin(),top.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            std::string hot;
            for(std::size_t k=0;k<top.size() && k<10;++k){hot+=top[k].second;hot+=':';hot+=std::to_string(top[k].first);hot+=' ';}
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PROFILE token=%08x samples=%u top=%s",t.token,samples,hot.c_str());
        }
        t.profile.clear();
        t.reported=t.entries;
    }
}
// The former sink's ring, with the 2^26 trigger replaced by `cadence` for the test.
struct Ring {
    Ring(std::size_t l,std::uint64_t c):limit(l),cadence(c){}
    std::size_t limit;std::uint64_t cadence;
    std::vector<std::string_view> functionLabels;std::size_t nextFunctionLabel=0;std::uint64_t droppedFunctionLabels=0;
    void record(std::string_view label) {
        if (functionLabels.size() < limit) {
            functionLabels.emplace_back(label);
        } else {
            functionLabels[nextFunctionLabel] = label;
            nextFunctionLabel = (nextFunctionLabel + 1) % functionLabels.size();
            ++droppedFunctionLabels;
            if(!(droppedFunctionLabels & (cadence-1))) {
                std::string recent;
                for(std::size_t i=functionLabels.size()-16;i<functionLabels.size();++i)
                    {recent+=functionLabels[(nextFunctionLabel+i)%functionLabels.size()];recent+=' ';}
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_LABELS entries=%llu recent=%s",
                    static_cast<unsigned long long>(droppedFunctionLabels),recent.c_str());
            }
        }
    }
};
}

std::vector<std::string> takeLines() {
    std::vector<std::string> lines;
    for (const auto& line : recomp::testing::hostLogLines()) lines.push_back(line.text);
    recomp::testing::clearHostLogLines();
    return lines;
}

// One deterministic cooperative schedule: tokens take turns in slices, with periodic dumps.
template<class Record, class Dump, class RingRecord>
std::vector<std::string> run(Record record, Dump dump, RingRecord ring, std::uint64_t entries) {
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    const auto next = [&](std::uint64_t bound) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state % bound; };
    constexpr std::array<std::uint32_t, 6> Tokens{0x63001000, 0x6408e000, 0x64141000, 0x640b1000, 0x64184000, 0x64339000};
    std::vector<std::string> lines;
    bool lateDump = false;
    for (std::uint64_t e = 0; e < entries;) {
        // One dominant thread, like the audio engine thread (64-72% of guest entries on device).
        const auto token = next(2) ? Tokens[2] : Tokens[next(Tokens.size())];
        const auto slice = 1 + next(700);
        for (std::uint64_t k = 0; k < slice && e < entries; ++k, ++e) {
            const auto l = label(static_cast<unsigned>(next(LabelCount * 3) % LabelCount));
            record(token, l);
            ring(l);
        }
        // Many dumps early (interleavings), then one late dump after > 2^20 entries (PROFILE lines).
        const bool early = e < 500'000;
        if ((early && !next(40)) || (!early && !lateDump && e >= 6'000'000)) {
            lateDump |= !early;
            dump();
            for (auto& line : takeLines()) lines.push_back(std::move(line));
        }
    }
    dump();
    for (auto& line : takeLines()) lines.push_back(std::move(line));
    return lines;
}
}

#if defined(TRACE_DIAGNOSTICS_LEGACY_CONCURRENCY) && TRACE_DIAGNOSTICS_LEGACY_CONCURRENCY
int main() {
    constexpr unsigned Threads = 16;
    constexpr std::uint64_t PerThread = 1 << 20;
    std::atomic<bool> stop{false};
    std::thread dumper([&] { while (!stop.load()) { legacy::logThreads(); recomp::testing::clearHostLogLines(); } });
    std::vector<std::thread> sinks;
    for (unsigned t = 0; t < Threads; ++t) sinks.emplace_back([t] {
        for (std::uint64_t e = 0; e < PerThread; ++e)
            legacy::recordThreadLabel(0x64001000U + t * 0x13000U, label(static_cast<unsigned>((e * 7 + t) % LabelCount)));
    });
    for (auto& s : sinks) s.join();
    stop = true;
    dumper.join();
    unsigned long long total = 0;
    for (const auto& record : legacy::threadLabels) total += record.entries;
    CHECK(total == Threads * PerThread);
    std::printf("TRACE_DIAGNOSTICS_LEGACY_NO_FAILURE_OBSERVED total=%llu\n", total);
}
#else
int main() {
    // ---- 1. Cooperative equivalence (single host thread, like one guest running at a time) ----
    constexpr std::size_t Limit = 1000;
    constexpr std::uint64_t Cadence = 1 << 12;
    constexpr std::uint64_t Entries = 12'000'000; // Enough for PROFILE lines (> 2^20 per token).
    legacy::Ring legacyRing{Limit, Cadence};
    const auto reference = run([](auto token, auto l) { legacy::recordThreadLabel(token, l); },
        [] { legacy::logThreads(); }, [&](auto l) { legacyRing.record(l); }, Entries);
    diagnostic_skip::ThreadLabelBook book;
    diagnostic_skip::FunctionLabelRing ring(Limit, Cadence);
    const auto current = run([&](auto token, auto l) { if (diagnostic_skip::ThreadLabelBook::sample()) book.record(token, l); },
        [&] { book.dump(); }, [&](auto l) {
            if (const auto dropped = ring.record(l)) {
                const auto recent = ring.recentAtDrop(dropped, 16);
                __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_LABELS entries=%llu recent=%s",
                    static_cast<unsigned long long>(dropped), recent.c_str());
            }
        }, Entries);
    CHECK(reference.size() == current.size());
    for (std::size_t i = 0; i < reference.size(); ++i) if (reference[i] != current[i]) {
        std::fprintf(stderr, "reference: %s\ncurrent:   %s\n", reference[i].c_str(), current[i].c_str());
        failTest("dump line differs from the former implementation", __LINE__);
    }
    std::map<std::string, unsigned> kinds;
    for (const auto& line : current) ++kinds[line.substr(0, line.find(' '))];
    CHECK(kinds["RENDER_DIAG_THREAD"] > 0 && kinds["RENDER_DIAG_PROFILE"] > 0 && kinds["RENDER_DIAG_LABELS"] > 0);
    CHECK(ring.size() == legacyRing.functionLabels.size() && ring.dropped() == legacyRing.droppedFunctionLabels);
    for (std::size_t i = 0; i < ring.size(); ++i)
        CHECK(ring.at(i) == legacyRing.functionLabels[(legacyRing.nextFunctionLabel + i) % legacyRing.functionLabels.size()]);
    // Tiny rings: the former loop printed nothing when the ring held fewer than 16 labels.
    for (std::size_t limit : {1u, 7u, 16u, 17u}) {
        legacy::Ring small{limit, 1}; diagnostic_skip::FunctionLabelRing tiny(limit, 1);
        for (unsigned i = 0; i < 300; ++i) {
            small.record(label(i));
            if (const auto dropped = tiny.record(label(i)))
                __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_LABELS entries=%llu recent=%s",
                    static_cast<unsigned long long>(dropped), tiny.recentAtDrop(dropped, 16).c_str());
        }
        const auto lines = takeLines();
        CHECK(lines.size() % 2 == 0);
        for (std::size_t i = 0; i < lines.size(); i += 2) CHECK(lines[i] == lines[i + 1]);
    }

    // ---- 2. Concurrency: 16 sink threads and a dumper ----
    constexpr unsigned Threads = 16;
    constexpr std::uint64_t PerThread = 1 << 20; // a multiple of 8: exactly PerThread/8 samples each
    diagnostic_skip::ThreadLabelBook shared;
    diagnostic_skip::FunctionLabelRing sharedRing(Limit, 1 << 10);
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> triggers{0}, dumps{0};
    std::thread dumper([&] { while (!stop.load()) { shared.dump(); ++dumps; recomp::testing::clearHostLogLines(); std::this_thread::yield(); } });
    std::vector<std::thread> sinks;
    for (unsigned t = 0; t < Threads; ++t) sinks.emplace_back([&, t] {
        const std::uint32_t token = 0x64001000U + t * 0x13000U;
        for (std::uint64_t e = 0; e < PerThread; ++e) {
            const auto l = label(static_cast<unsigned>((e * 7 + t) % LabelCount));
            if (diagnostic_skip::ThreadLabelBook::sample()) shared.record(token, l);
            if (const auto dropped = sharedRing.record(l)) {
                const auto text = sharedRing.recentAtDrop(dropped, 16);
                CHECK(std::count(text.begin(), text.end(), ' ') == 16);
                ++triggers;
            }
        }
    });
    for (auto& s : sinks) s.join();
    stop = true;
    dumper.join();
    recomp::testing::clearHostLogLines();
    shared.dump();
    unsigned threadLines = 0;
    for (const auto& line : takeLines()) {
        if (line.rfind("RENDER_DIAG_THREAD ", 0) != 0) continue;
        ++threadLines;
        unsigned token = 0; unsigned long long fresh = 0, total = 0;
        CHECK(std::sscanf(line.c_str(), "RENDER_DIAG_THREAD token=%x new=%llu total=%llu", &token, &fresh, &total) == 3);
        CHECK(total == PerThread && (token - 0x64001000U) % 0x13000U == 0);
        const auto anchorAt = line.find(" anchor=") + 8, arrow = line.find("<-", anchorAt);
        const auto anchor = line.substr(anchorAt, arrow - anchorAt);
        CHECK(anchor.rfind("__imp____", 0) != 0 && anchor != "__imp__sub_82220140" &&
              anchor != "__imp__sub_82220130" && anchor != "__imp__sub_8251E820");
    }
    CHECK(threadLines == Threads);
    CHECK(sharedRing.size() == Limit && sharedRing.dropped() == Threads * PerThread - Limit);
    CHECK(triggers.load() == sharedRing.dropped() / (1 << 10));
    for (std::size_t i = 0; i < sharedRing.size(); ++i) {
        const auto l = sharedRing.at(i);
        bool known = false;
        for (unsigned k = 0; k < LabelCount; ++k) known |= l == label(k);
        CHECK(known);
    }
    std::printf("TRACE_DIAGNOSTICS_PASS equivalence_lines=%zu thread=%u profile=%u labels=%u; concurrent_threads=%u "
                "entries=%llu dumps=%llu ring_triggers=%llu\n",
        current.size(), kinds["RENDER_DIAG_THREAD"], kinds["RENDER_DIAG_PROFILE"], kinds["RENDER_DIAG_LABELS"],
        Threads, static_cast<unsigned long long>(Threads * PerThread), dumps.load(), triggers.load());
}
#endif
