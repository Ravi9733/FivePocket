#include "guest_execution_guard.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using recomp::bootstrap::GuestExecutionGuard;
using recomp::bootstrap::GuestExecutionResult;
using recomp::bootstrap::GuestExecutionStopReason;

namespace {

bool expect(bool condition) {
    return condition;
}

} // namespace

int main() {
    GuestExecutionGuard guard;
    recomp::bootstrap::GuestImportArguments arguments{0xffff'ffff'7000'1000ULL,
        {{0xffff'ffff'ffff'ffffULL, 2, 3, 4, 5, 6, 7, 8}}};
    GuestExecutionGuard::captureImportArguments(arguments); // Outside a run: ignored.
    const auto captured = guard.run(1, [&] {
        GuestExecutionGuard::captureImportArguments(arguments);
        arguments.registers[0] = 0; // Snapshot owns its values.
        GuestExecutionGuard::stopForUnimplementedImport("__imp__Snapshot");
    });
    if (!expect(captured.importArguments && captured.importArguments->stackPointer == arguments.stackPointer
            && captured.importArguments->registers[0] == 0xffff'ffff'ffff'ffffULL
            && captured.importArguments->registers[7] == 8)) { return 21; }
    const auto rejectedSnapshot = guard.run(1, [&] {
        GuestExecutionGuard::captureImportArguments(arguments);
        GuestExecutionGuard::stopForRejectedImport("__imp__Snapshot");
    });
    if (!expect(rejectedSnapshot.importArguments && rejectedSnapshot.importArguments->registers[0] == 0)) { return 22; }
    const auto discarded = guard.run(1, [&] { GuestExecutionGuard::captureImportArguments(arguments); });
    const auto reset = guard.run(1, [&] { GuestExecutionGuard::stopForUnimplementedImport("__imp__Next"); });
    if (!expect(!discarded.importArguments && !reset.importArguments)) { return 23; }

    if (!expect(!GuestExecutionGuard::isActive())) {
        return 1;
    }
    GuestExecutionGuard::checkpoint();

    const auto empty = guard.run(1, {});
    if (!expect(empty.reason == GuestExecutionStopReason::InvalidEntry)) {
        return 2;
    }

    const auto noBudget = guard.run(0, [] {});
    if (!expect(noBudget.reason == GuestExecutionStopReason::InvalidBudget)) {
        return 3;
    }

    const auto completed = guard.run(2, [] {
        GuestExecutionGuard::checkpoint();
        recomp::bootstrap::recomp_guest_execution_checkpoint();
    });
    if (!expect(completed.completed()
            && completed.completedFunctionEntries == 2
            && completed.detail.empty())) {
        return 4;
    }

    std::vector<std::string> functionTrace;
    const auto traced = guard.run(
            2,
            [] {
                GuestExecutionGuard::checkpoint("first");
                recomp::bootstrap::recomp_guest_execution_checkpoint_named("second");
            },
            [&](std::string_view label) {
                functionTrace.emplace_back(label);
            });
    if (!expect(traced.completed()
            && functionTrace == std::vector<std::string>{"first", "second"})) {
        return 5;
    }

    std::string longLabel(200, 'x');
    functionTrace.clear();
    const auto boundedTrace = guard.run(1, [&] { GuestExecutionGuard::checkpoint(longLabel); },
        [&](std::string_view label) { functionTrace.emplace_back(label); });
    longLabel.assign(200, 'y');
    if (!boundedTrace.completed() || functionTrace != std::vector<std::string>{std::string(128, 'x')}) return 30;

    const auto exhausted = guard.run(2, [] {
        GuestExecutionGuard::checkpoint();
        GuestExecutionGuard::checkpoint();
        GuestExecutionGuard::checkpoint();
    });
    if (!expect(exhausted.reason
                    == GuestExecutionStopReason::FunctionEntryBudgetExceeded
            && exhausted.completedFunctionEntries == 2)) {
        return 6;
    }

    const auto missing = guard.run(4, [] {
        GuestExecutionGuard::checkpoint();
        recomp::bootstrap::recomp_guest_execution_unimplemented_import(
                "__imp__NotImplementedForTest");
    });
    if (!expect(missing.reason == GuestExecutionStopReason::UnimplementedImport
            && missing.completedFunctionEntries == 1
            && missing.detail == "__imp__NotImplementedForTest")) {
        return 7;
    }

    const auto unnamed = guard.run(1, [] {
        recomp::bootstrap::recomp_guest_execution_unimplemented_import(nullptr);
    });
    const auto trap = guard.run(2, [] {
        GuestExecutionGuard::checkpoint();
        recomp::bootstrap::recomp_guest_execution_trap();
    });
    if (!expect(trap.reason == GuestExecutionStopReason::GuestTrap && trap.completedFunctionEntries == 1
            && std::string(recomp::bootstrap::guestExecutionStopReasonName(trap.reason)) == "guest-trap")) {
        return 20;
    }
    if (!expect(unnamed.reason == GuestExecutionStopReason::UnimplementedImport
            && unnamed.detail == "unknown-import")) {
        return 8;
    }

    const std::string longImportName(256, 'x');
    const auto bounded = guard.run(1, [&] {
        recomp::bootstrap::recomp_guest_execution_unimplemented_import(
                longImportName.c_str());
    });
    if (!expect(bounded.reason == GuestExecutionStopReason::UnimplementedImport
            && bounded.detail.size() == 128)) {
        return 9;
    }

    const auto invalidIndirect = guard.run(1, [] {
        GuestExecutionGuard::checkpoint();
        GuestExecutionGuard::stopForInvalidIndirectTarget();
    });
    if (!expect(invalidIndirect.reason == GuestExecutionStopReason::InvalidIndirectTarget
            && invalidIndirect.completedFunctionEntries == 1
            && invalidIndirect.detail == "Generated guest indirect target is not mapped")) {
        return 10;
    }

    GuestExecutionResult nestedResult;
    const auto outer = guard.run(1, [&] {
        nestedResult = guard.run(1, [] {});
        GuestExecutionGuard::checkpoint();
    });
    if (!expect(outer.completed()
            && nestedResult.reason == GuestExecutionStopReason::NestedRun)) {
        return 11;
    }

    const auto hostException = guard.run(1, [] {
        throw std::runtime_error("private diagnostic must not escape");
    });
    // The detail must name the failing boundary, not merely restate the stop.
    if (!expect(hostException.reason == GuestExecutionStopReason::HostException
            && hostException.detail.rfind("Guest entry raised a host C++ exception: ", 0) == 0
            && hostException.detail.find("private diagnostic must not escape")
                    != std::string::npos)) {
        return 12;
    }

    // The private trace protocol is line oriented; an unsanitised what() could
    // forge a protocol key. Control bytes must not survive, and the detail must
    // stay inside the guard's bound however long the message is.
    const auto forged = guard.run(1, [] {
        throw std::runtime_error(std::string("a\nstop_reason=completed\tb\x7f\x01")
                + std::string(4096, 'z'));
    });
    if (!expect(forged.reason == GuestExecutionStopReason::HostException
            && forged.detail.size() == 192
            && forged.detail.find('\n') == std::string::npos
            && forged.detail.find("?stop_reason=completed?b??") != std::string::npos
            && std::all_of(forged.detail.begin(), forged.detail.end(),
                    [](char c) { return c >= 0x20 && c <= 0x7e; }))) {
        return 40;
    }

    // An unknown exception still stops, and still reports something bounded.
    const auto unknown = guard.run(1, [] { throw 42; });
    if (!expect(unknown.reason == GuestExecutionStopReason::HostException
            && unknown.detail == "Guest entry raised an unknown host exception")) {
        return 41;
    }

    if (!expect(!GuestExecutionGuard::isActive())) {
        return 13;
    }

    const auto rejected = guard.run(4, [] {
        GuestExecutionGuard::checkpoint();
        GuestExecutionGuard::stopForRejectedImport("__imp__NtWaitForSingleObjectEx");
    });
    if (!expect(rejected.reason == GuestExecutionStopReason::RejectedImport
            && rejected.completedFunctionEntries == 1
            && rejected.detail == "__imp__NtWaitForSingleObjectEx"
            && std::string(recomp::bootstrap::guestExecutionStopReasonName(rejected.reason))
                    == "rejected-import")) {
        return 14;
    }

    return 0;
}
