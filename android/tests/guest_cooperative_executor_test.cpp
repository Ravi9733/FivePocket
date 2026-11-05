#include "guest_cooperative_executor.h"
#include "guest_thread_priority.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace recomp::bootstrap;

namespace {
void step(const char* label) { GuestExecutionGuard::checkpoint(label); }

void testParkedEvidenceBeforeSceneInterrupt() {
    GuestCooperativeExecutor executor;
    bool produced=false,release=false,finished=false;unsigned readinessCalls=0;
    assert(!executor.currentTaskToken() && !executor.parkedTask(2));
    const auto result=executor.run(1,[&] {
        assert(executor.currentTaskToken()==1 && !executor.parkedTask(1));
        assert(executor.enqueue(2,[&] {
            assert(executor.currentTaskToken()==2);
            produced=true;
            step("scene-submitted-before-original-worker-park");
            assert(executor.waitUntil([&]{++readinessCalls;return release;}));
            finished=true;
        }));
        assert(!executor.parkedTask(2) && !executor.parkedTask(99));
        step("allow-original-producer");
        assert(produced && !executor.parkedTask(2) && !finished);
        assert(executor.waitUntil([&]{return produced && executor.parkedTask(2);}));
        const auto calls=readinessCalls;
        assert(executor.parkedTask(2) && readinessCalls==calls && !finished);
        release=true;
        assert(executor.parkedTask(2)); // Observation does not consume the signal.
        assert(executor.waitUntil([&]{return finished;}));
        assert(!executor.parkedTask(2));
    },{100,1,3},[](auto){});
    assert(result.execution.completed() && finished && !executor.currentTaskToken() && !executor.parkedTask(2));
    std::puts("NATIVE_SCENE_PARKED_CONSUMER_PASS producer_flag_not_parked=1 actual_wait_registration=1 observation_no_signal_consumption=1 unknown_or_returned_rejected=1 outside_gate_rejected=1 gpu_execution=0");
}

void testSimpleAndValidation() {
    GuestCooperativeExecutor executor;
    std::uint32_t current = 0;
    const auto bind = [&](auto token) { current = token; };
    assert(!executor.enqueue(2, [] {}));
    assert(executor.run(0, [] {}, {}, bind).execution.reason == GuestExecutionStopReason::InvalidEntry);
    assert(executor.run(1, {}, {}, bind).execution.reason == GuestExecutionStopReason::InvalidEntry);
    assert(executor.run(1, [] {}, {}, {}).execution.reason == GuestExecutionStopReason::InvalidEntry);
    for (const auto limits : {GuestCooperativeLimits{0, 1, 2}, {1, 0, 2}, {1, 1, 0}, {1, 1, 66}}) {
        assert(executor.run(1, [] {}, limits, bind).execution.reason == GuestExecutionStopReason::InvalidBudget);
    }
    const auto result = executor.run(1, [&] {
        assert(executor.running() && current == 1);
        assert(executor.run(2, [] {}, {}, bind).execution.reason == GuestExecutionStopReason::NestedRun);
        step("main"); step("main");
    }, {10, 1, 2}, bind);
    assert(result.execution.completed() && result.execution.completedFunctionEntries == 2);
    assert(result.workersCreated == 0 && !executor.running());
    assert(executor.run(1, [] {}, {10, 1, 2}, bind).execution.completed());
}

std::vector<std::string> interleavedRun() {
    GuestCooperativeExecutor executor;
    std::vector<std::string> trace;
    std::uint32_t current = 0;
    bool published = false, entered = false;
    int workerLocalResult = 0;
    const auto result = executor.run(1, [&] {
        step("m0");
        assert(executor.enqueue(2, [&] {
            assert(published && current == 2);
            entered = true;
            int local = 17; // Must survive another task's live host stack.
            step("w0");
            assert(current == 2 && local == 17);
            local += 4;
            step("w1");
            assert(current == 2 && local == 21);
            workerLocalResult = local;
        }));
        assert(!entered); // Enqueue never executes inline or releases the gate.
        published = true;
        step("m1");
        assert(current == 1 && entered);
        step("m2");
        assert(current == 1);
    }, {20, 1, 3}, [&](auto token) { current = token; },
    [&](auto label) { trace.emplace_back(std::to_string(current) + ":" + std::string(label)); });
    assert(result.execution.completed() && result.execution.completedFunctionEntries == 5);
    assert(result.workersCreated == 1 && result.workersEntered == 1);
    assert(result.contextSwitches >= 3 && workerLocalResult == 21);
    assert((trace == std::vector<std::string>{"1:m0", "1:m1", "2:w0", "1:m2", "2:w1"}));
    return trace;
}

void testCancellationAndGlobalBudget() {
    GuestCooperativeExecutor executor;
    std::uint32_t current = 0;
    bool mainContinued = false, workerContinued = false;
    auto result = executor.run(1, [&] {
        assert(executor.enqueue(2, [&] {
            step("worker");
            GuestExecutionGuard::captureImportArguments({0x2200, {{2, 3, 4, 5, 6, 7, 8, 9}}});
            GuestExecutionGuard::stopForUnimplementedImport("__imp__SyntheticWorker");
            workerContinued = true;
        }));
        step("main");
        step("main2");
        mainContinued = true;
    }, {20, 1, 4}, [&](auto t) { current = t; });
    assert(result.execution.reason == GuestExecutionStopReason::UnimplementedImport);
    assert(result.execution.detail == "__imp__SyntheticWorker");
    assert(result.execution.importArguments && result.execution.importArguments->stackPointer == 0x2200
        && result.execution.importArguments->registers[0] == 2);
    assert(!mainContinued && !workerContinued && result.workersEntered == 1);

    result = executor.run(1, [&] {
        assert(executor.enqueue(2, [&] { for (;;) { assert(current == 2); step("w"); } }));
        for (;;) { assert(current == 1); step("m"); }
    }, {17, 1, 3}, [&](auto t) { current = t; });
    assert(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
    assert(result.execution.completedFunctionEntries == 17 && result.workersEntered == 1);
    assert(!result.execution.importArguments);
    assert(!executor.running());
}

void testParentStopsBeforeWorkerRuns() {
    GuestCooperativeExecutor executor;
    bool entered = false;
    const auto result = executor.run(1, [&] {
        assert(executor.enqueue(2, [&] { entered = true; }));
        GuestExecutionGuard::stopForRejectedImport("__imp__SyntheticMain");
    }, {10, 1, 2}, [](auto) {});
    assert(result.execution.reason == GuestExecutionStopReason::RejectedImport);
    assert(result.execution.detail == "__imp__SyntheticMain");
    assert(result.workersCreated == 1 && result.workersEntered == 0 && !entered);
}

void testCapacityAndHostFailure() {
    GuestCooperativeExecutor executor;
    bool workerDone = false;
    const auto result = executor.run(1, [&] {
        assert(!executor.enqueue(0, [] {}));
        assert(!executor.enqueue(1, [] {}));
        assert(!executor.enqueue(2, {}));
        assert(executor.enqueue(2, [&] { workerDone = true; }));
        assert(!executor.enqueue(2, [] {}));
        assert(!executor.enqueue(3, [] {}));
    }, {10, 32, 2}, [](auto) {});
    assert(result.execution.completed() && workerDone); // Parent can return before worker starts.

    GuestCooperativeExecutor failing([](auto) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    const auto failure = failing.run(1, [&] { assert(!failing.enqueue(2, [] {})); step("main"); },
                                    {10, 1, 2}, [](auto) {});
    assert(failure.execution.completed() && failure.workersCreated == 0);
    GuestCooperativeExecutor empty([](auto) { return std::thread{}; });
    assert(empty.run(1, [&] { assert(!empty.enqueue(2, [] {})); }, {10, 1, 2}, [](auto) {}).execution.completed());
}

void testBindingAndHostException() {
    GuestCooperativeExecutor executor;
    const auto result = executor.run(1, [&] {
        assert(executor.enqueue(2, [] { step("must-not-run"); })); step("main");
    }, {10, 1, 2}, [](auto token) { if (token == 2) { throw std::runtime_error("private"); } });
    assert(result.execution.reason == GuestExecutionStopReason::HostException);
    assert(result.execution.detail.find("private") == std::string::npos);
    assert(result.workersEntered == 0);
    assert(executor.run(1, [] { throw std::logic_error("private"); }, {}, [](auto) {}).execution.reason
           == GuestExecutionStopReason::HostException);
}

void testManyTasksAndPlainSharedMemory() {
    GuestCooperativeExecutor executor;
    std::uint32_t current = 0;
    int shared = 0;
    const auto result = executor.run(1, [&] {
        for (std::uint32_t id = 2; id <= 9; ++id) {
            assert(executor.enqueue(id, [&, id] {
                for (int i = 0; i < 20; ++i) {
                    assert(current == id);
                    ++shared; // Deliberately non-atomic; the execution gate must serialize it.
                    step("worker");
                }
            }));
        }
        for (int i = 0; i < 20; ++i) { assert(current == 1); ++shared; step("main"); }
    }, {1000, 3, 10}, [&](auto t) { current = t; });
    assert(result.execution.completed() && shared == 180);
    assert(result.execution.completedFunctionEntries == 180 && result.workersEntered == 8);
}

void testParkWakeAndStall() {
    GuestCooperativeExecutor executor;
    assert(!executor.waitUntil([] { return true; }));
    std::uint32_t current = 0;
    int signal = 0, polls = 0;
    auto result = executor.run(1, [&] {
        int stackLocal = 42;
        assert(executor.enqueue(2, [&] {
            assert(current == 2 && signal == 0);
            signal = 1;
        }));
        assert(executor.waitUntil([&] {
            ++polls;
            if (signal == 0) { return false; }
            --signal;
            return true;
        }));
        assert(current == 1 && stackLocal == 42 && signal == 0);
    }, {10, 32, 2}, [&](auto token) { current = token; });
    assert(result.execution.completed() && result.workersEntered == 1);
    assert(result.waitOperations == 1 && result.contextSwitches == 2 && polls == 2);
    result = executor.run(1, [&] {
        assert(executor.enqueue(2, [] {}));
        (void)executor.waitUntil([] { return false; });
        assert(false && "A stalled run must unwind instead of claiming a wake");
    }, {10, 32, 2}, [](auto) {});
    assert(result.execution.reason == GuestExecutionStopReason::NoRunnableTask);
    polls = 0;
    result = executor.run(1, [&] {
        assert(executor.enqueue(2, [] {}));
        (void)executor.waitUntil([&] {
            if (++polls > 1) { throw std::logic_error("private readiness detail"); }
            return false;
        });
    }, {10, 32, 2}, [](auto) {});
    assert(result.execution.reason == GuestExecutionStopReason::HostException);
    assert(result.execution.detail.find("private") == std::string::npos);
    result = executor.run(1, [&] {
        assert(!executor.waitUntil({}));
        assert(!executor.waitUntil([] { return false; })); // No producer, no busy spin.
        assert(executor.waitUntil([] { return true; }));
        assert(!executor.waitUntil([] { return true; })); // Explicit operation cap.
    }, {2, 32, 2}, [](auto) {});
    assert(result.execution.completed() && result.waitOperations == 2);
}

void testTimedWorkerAfterMainReturns() {
    GuestCooperativeExecutor executor;
    bool woke = false;
    const auto result = executor.run(1, [&] {
        assert(executor.enqueue(2, [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            assert(executor.waitUntil([&] { return std::chrono::steady_clock::now() >= deadline; }, deadline));
            woke = true;
        }));
        step("let_worker_park");
    }, {100, 32, 2}, [](auto) {});
    assert(result.execution.completed() && woke && result.waitOperations == 1);
    auto bad = executor.run(1, [&] {
        (void)executor.waitUntil([] { return false; }, std::chrono::steady_clock::now());
    }, {100, 32, 2}, [](auto) {});
    assert(bad.execution.reason == GuestExecutionStopReason::HostException);
}

void testWeightedPriorities() {
    GuestCooperativeExecutor executor;
    assert(!executor.setPriority(1, 0));
    for (const auto priority : {-16, -2, -1, 0, 1, 2, 16}) {
        std::array<int, 3> counts{};
        const auto result = executor.run(1, [&] {
            assert(!executor.setPriority(99, 0));
            assert(!executor.setPriority(1, 3));
            assert(!executor.setPriority(1, -3));
            assert(!executor.setPriority(1, 17)); // Registry must normalize raw increments.
            assert(!executor.enqueue(2, [] {}, 3));
            assert(executor.setPriority(1, priority));
            assert(executor.enqueue(2, [&] { for (;;) { step("worker"); } }));
            for (;;) {
                assert(executor.setPriority(1, priority)); // Cannot reset slice to avoid yielding.
                step("main");
            }
        }, {4001, 4, 2}, [](auto) {}, [&](auto label) { ++counts[label == "main" ? 1 : 2]; });
        assert(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
        const auto shift = portablePrioritySliceShift(priority);
        const auto slice = shift >= 0 ? 4 << shift : std::max(1, 4 >> -shift);
        const auto error = counts[1] * 4 - counts[2] * slice;
        assert(error >= -160 && error <= 160); // Last partial rotation is bounded.
        assert(counts[1] > 0 && counts[2] > 0); // Lower priority does not starve.
    }
    bool ran = false;
    const auto result = executor.run(1, [&] {
        assert(executor.enqueue(2, [&] {
            assert(executor.setPriority(1, -2)); // May update a waiting peer.
            ran = true;
        }, 2));
        assert(executor.waitUntil([&] { return ran; }));
        assert(!executor.setPriority(2, 0)); // Finished workers cannot be changed.
    }, {100, 4, 2}, [](auto) {});
    assert(result.execution.completed() && ran);
    assert(!executor.setPriority(1, 0));
}

void testWorkerTrapCancelsParkedMain() {
    GuestCooperativeExecutor executor;
    const auto result = executor.run(1, [&] {
        (void)executor.enqueue(2, [] { GuestExecutionGuard::stopForGuestTrap(); });
        (void)executor.waitUntil([] { return false; });
        assert(false && "A guest trap must unwind all waiting tasks");
    }, {100, 32, 2}, [](auto) {});
    assert(result.execution.reason == GuestExecutionStopReason::GuestTrap && result.workersEntered == 1);
}

void testFullCapacityTargetedWakeAndCancellation() {
    GuestCooperativeExecutor executor;
    std::uint32_t current = 0;
    std::array<unsigned, 65> counts{};
    auto result = executor.run(1, [&] {
        for (std::uint32_t token = 2; token <= 65; ++token) {
            assert(executor.enqueue(token, [&, token] {
                for (unsigned i = 0; i < 16; ++i) {
                    assert(current == token);
                    ++counts[token - 1]; step("targeted-worker");
                }
            }));
        }
        for (unsigned i = 0; i < 16; ++i) { ++counts[0]; step("targeted-main"); }
    }, {2000, 1, 65}, [&](auto token) { current = token; });
    assert(result.execution.completed() && result.workersEntered == 64);
    for (auto count : counts) assert(count == 16);
    unsigned unwound = 0;
    struct Unwind { unsigned& count; ~Unwind() { ++count; } };
    result = executor.run(1, [&] {
        for (std::uint32_t token = 2; token <= 65; ++token) {
            assert(executor.enqueue(token, [&] {
                Unwind scope{unwound};
                for (;;) step("cancel-all-workers");
            }));
        }
        for (;;) step("cancel-main");
    }, {200, 1, 65}, [](auto) {});
    assert(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
    assert(result.workersEntered == 64 && unwound == 64);
}
} // namespace

int main() {
    testParkedEvidenceBeforeSceneInterrupt();
    testSimpleAndValidation();
    const auto first = interleavedRun();
    for (int i = 0; i < 10; ++i) { assert(interleavedRun() == first); }
    testCancellationAndGlobalBudget();
    testParentStopsBeforeWorkerRuns();
    testCapacityAndHostFailure();
    testBindingAndHostException();
    testManyTasksAndPlainSharedMemory();
    testParkWakeAndStall();
    testTimedWorkerAfterMainReturns();
    testWorkerTrapCancelsParkedMain();
    testFullCapacityTargetedWakeAndCancellation();
    testWeightedPriorities();
}
