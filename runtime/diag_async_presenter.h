#pragma once
#if defined(__ANDROID__)
#include <unistd.h>
extern "C" void recomp_register_stop_yield_thread(int tid); // v241 (guest_cooperative_executor.cpp)
#endif
// Attempt v43, DIAGNOSTIC BUILDS ONLY: hands finished generic-renderer frames to the debug app on a
// dedicated host thread, so the render thread (guest 6408e000) no longer waits inside its lock gate for
// the scan-out mapping and the app's own Vulkan upload/readback/present of every frame.
//
// Semantics that change against the synchronous path, all diagnostic-only:
//  - submit() returning true means "queued", not "displayed": the caller counts the frame as presented
//    before the app has accepted it. A display failure is recorded when it happens (failed(), log) and
//    every later submit() refuses, so the caller is back on the synchronous path from then on (where the
//    app's own sequence refuses further frames exactly as before).
//  - A mailbox of one: a frame queued while the previous one is still being displayed replaces an
//    undelivered earlier one (counted as replaced). The app never sees a gap in its own sequence numbers:
//    they are assigned when a frame is actually delivered.
//  - Every display call (queued or synchronous) runs under one mutex, and a synchronous call first waits
//    until every queued frame was delivered, so frames reach the app in submission order and the app's
//    single-caller callback is never entered twice at once.
// The thread starts at the first submit() and is joined by the destructor, which first delivers a frame
// still queued; no display call runs after the destructor returns.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif

namespace recomp::diag {
class AsyncFramePresenter {
public:
    // The synchronous display of one frame (bytes, width, height); true when the consumer accepted it.
    using Display=std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)>;
    using Report=std::function<void(const char* what,std::uint64_t sequence)>;
    struct Counters {std::uint64_t submitted=0,delivered=0,replaced=0,failed=0,syncCalls=0,refused=0,pairs=0,midsDropped=0;};

    explicit AsyncFramePresenter(Display display,Report report={}):display_(std::move(display)),report_(std::move(report)){}
    AsyncFramePresenter(const AsyncFramePresenter&)=delete;
    AsyncFramePresenter& operator=(const AsyncFramePresenter&)=delete;
    ~AsyncFramePresenter() {
        {std::lock_guard lock(mutex_);stop_=true;}
        wake_.notify_all();
        if(thread_.joinable())thread_.join();
    }
    // Copies the frame into an owned buffer (recycled between frames) and queues it. false: not queued
    // (an earlier queued frame failed to display, the presenter is stopping, or the thread could not be
    // started); the caller then displays synchronously.
    bool submit(const std::uint8_t* rgba,std::size_t bytes,unsigned width,unsigned height) {
        std::vector<std::uint8_t> buffer;
        {
            std::lock_guard lock(mutex_);
            if(failed_ || stop_ || !display_ || !rgba){++counters_.refused;return false;}
            if(!thread_.joinable()) {
                try {thread_=std::thread([this]{run();});}
                catch(...) {++counters_.refused;return false;}
            }
            if(!spare_.empty()){buffer=std::move(spare_.back());spare_.pop_back();}
        }
        try {buffer.assign(rgba,rgba+bytes);} // outside the lock: the thread may be displaying meanwhile
        catch(...) {std::lock_guard lock(mutex_);++counters_.refused;return false;}
        std::lock_guard lock(mutex_);
        if(failed_ || stop_){++counters_.refused;spare_.push_back(std::move(buffer));return false;}
        // v43 mailbox: a newer frame replaces every undelivered one (v158: queued mids and a waiting real frame too).
        for(auto& q:queue_){++counters_.replaced;spare_.push_back(std::move(q.frame.bytes));}
        queue_.clear();
        queue_.push_back({Frame{std::move(buffer),width,height,++sequence_},Clock::now(),false});
        ++counters_.submitted;
        wake_.notify_one();
        return true;
    }
    // v158 (frame interpolation, diagnostic): `mid` now, then `rgba` `delay` later. A real frame still waiting from the
    // previous pair is delivered at once first (frames stay in order); an undelivered mid is dropped.
    bool submitPair(const std::uint8_t* mid,const std::uint8_t* rgba,std::size_t bytes,unsigned width,unsigned height,
                    std::chrono::microseconds delay) {
        std::vector<std::uint8_t> a,b;
        {
            std::lock_guard lock(mutex_);
            if(failed_ || stop_ || !display_ || !rgba || !mid){++counters_.refused;return false;}
            if(!thread_.joinable()) {
                try {thread_=std::thread([this]{run();});}
                catch(...) {++counters_.refused;return false;}
            }
            if(!spare_.empty()){a=std::move(spare_.back());spare_.pop_back();}
            if(!spare_.empty()){b=std::move(spare_.back());spare_.pop_back();}
        }
        try {a.assign(mid,mid+bytes);b.assign(rgba,rgba+bytes);}
        catch(...) {std::lock_guard lock(mutex_);++counters_.refused;return false;}
        std::lock_guard lock(mutex_);
        if(failed_ || stop_){++counters_.refused;spare_.push_back(std::move(a));spare_.push_back(std::move(b));return false;}
        const auto now=Clock::now();
        std::deque<Entry> kept;
        for(auto& q:queue_) {
            if(q.interpolated){++counters_.midsDropped;spare_.push_back(std::move(q.frame.bytes));}
            else {q.at=now;kept.push_back(std::move(q));}
        }
        queue_=std::move(kept);
        queue_.push_back({Frame{std::move(a),width,height,++sequence_},now,true});
        queue_.push_back({Frame{std::move(b),width,height,++sequence_},now+delay,false});
        counters_.submitted+=2;++counters_.pairs;
        wake_.notify_one();
        return true;
    }
    // A synchronous display call, after every queued frame was delivered, serialised with the thread.
    bool runSync(const std::function<bool()>& call) {
        drain();
        std::lock_guard display(displayMutex_);
        {std::lock_guard lock(mutex_);++counters_.syncCalls;}
        return call();
    }
    // Waits until nothing is queued or being displayed.
    void drain() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock,[&]{return queue_.empty() && !busy_;});
    }
    bool failed()const{std::lock_guard lock(mutex_);return failed_;}
    Counters counters()const{std::lock_guard lock(mutex_);return counters_;}
private:
    struct Frame {std::vector<std::uint8_t> bytes;unsigned width=0,height=0;std::uint64_t sequence=0;};
    using Clock=std::chrono::steady_clock;
    struct Entry {Frame frame;Clock::time_point at;bool interpolated=false;};
    void run() {
#if defined(__ANDROID__) || defined(__linux__)
        pthread_setname_np(pthread_self(),"recomp-present");
#endif
#if defined(__ANDROID__)
        recomp_register_stop_yield_thread(int(gettid())); // v241
#endif
        std::unique_lock lock(mutex_);
        for(;;) {
            wake_.wait(lock,[&]{return stop_ || !queue_.empty();});
            if(queue_.empty())return; // stopping with nothing queued
            // v158: an entry is displayed at its time (a paired real frame waits half a frame interval), or at once when
            // stopping; a newer submission re-evaluates the queue.
            if(!stop_ && queue_.front().at>Clock::now()) {
                const auto at=queue_.front().at;
                wake_.wait_until(lock,at,[&]{return stop_ || queue_.empty() || queue_.front().at!=at || Clock::now()>=at;});
                continue;
            }
            Frame frame=std::move(queue_.front().frame);queue_.pop_front();busy_=true;
            lock.unlock();
            bool ok=false;
            {
                std::lock_guard display(displayMutex_);
                try {ok=display_(frame.bytes,frame.width,frame.height);}
                catch(...) {ok=false;}
            }
            if(!ok && report_)report_("display-failed",frame.sequence);
            lock.lock();
            busy_=false;
            if(ok)++counters_.delivered;
            else {++counters_.failed;failed_=true;}
            spare_.push_back(std::move(frame.bytes));
            if(spare_.size()>4)spare_.resize(4);
            idle_.notify_all();
        }
    }
    Display display_;
    Report report_;
    mutable std::mutex mutex_;
    std::mutex displayMutex_;
    std::condition_variable wake_,idle_;
    std::deque<Entry> queue_; // v158 (v43: std::optional<Frame> pending_, a mailbox of one)
    std::vector<std::vector<std::uint8_t>> spare_;
    bool busy_=false,failed_=false,stop_=false;
    std::uint64_t sequence_=0;
    Counters counters_;
    std::thread thread_;
};
}
