#!/usr/bin/env bash
# WP4 host tests (multicore plan, local-analysis/parallel-design/plan.md). Host only: no adb, no
# Android build, nothing installed. Run from anywhere:
#   android/tests/host_support/run_wp4_host_tests.sh [OUT_DIR]
# Env: CXX (clang++), RUNS (50: runs of each fast stress test), SEEDS (32), STEPS (20000),
#      NEGATIVE (1: also run the concurrency workloads against the previous code, expecting failure).
#
# Under test: local-analysis/attempts/v12/src/{native_xma_context_pool.cpp,guest_execution_guard.cpp,
# diagnostic_thread_labels.h} and android/app/src/main/cpp/native_audio_{render_client,queue}.h.
# 1. Concurrency tests, each built plain, with UBSan and with TSan (-std=c++20 -g -O1
#    -fsanitize=thread); the XMA and guard tests in RECOMP_GUEST_THREADS=0 (cooperative) and 2 (lanes).
# 2. Existing tests against the WP4 sources (XMA with the host codec stand-in: no FFmpeg here).
# 3. Cooperative XMA equivalence against the previous pool (identical output for every seed).
# 4. Negative controls: the previous code must fail the concurrency workloads.
# TSan is probed first with a trivial threaded program. Where its runtime cannot run (macOS 27.0
# beta: it faults during dyld initialisation) the TSan binaries are still compiled and linked and
# their runs are reported as SKIP, never as PASS. Every compile command is kept in
# OUT_DIR/commands.txt and the source hashes in OUT_DIR/sources.sha256.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd) || exit 2
cd "$ROOT" || exit 2
OUT=${1:-${TMPDIR:-/tmp}/wp4-host-tests}
mkdir -p "$OUT" || exit 2
: > "$OUT/commands.txt"
CXX=${CXX:-clang++}
RUNS=${RUNS:-50}; SEEDS=${SEEDS:-32}; STEPS=${STEPS:-20000}; NEGATIVE=${NEGATIVE:-1}
N=android/app/src/main/cpp; H=android/tests/host_support; T=tools/host_guest_trace
V=local-analysis/attempts/v12/src
POOL=$V/native_xma_context_pool.cpp; GUARD=$V/guest_execution_guard.cpp
XMA_HOST="$H/fake_xma_frame_decoder.cpp $H/android_log_capture.cpp"
PLAIN="-std=c++20 -g -O1 -UNDEBUG -Wall -Wextra"
UBSAN="-std=c++20 -g -O1 -UNDEBUG -fsanitize=undefined -fno-sanitize-recover=all"
TSAN="-std=c++20 -g -O1 -fsanitize=thread -UNDEBUG"
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1 second_deadlock_stack=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
pass=0; fail=0; skip=0; controls=0; triggered=0

say() { printf '%s\n' "$*"; }
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
# limited SECONDS COMMAND...: SIGALRM ends the command after SECONDS (exit status 142).
limited() { local seconds=$1; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$seconds" "$@"; }
# build NAME COMPILER_ARGS...: one compile and link, logged in OUT/NAME.build.log.
build() {
    local name=$1; shift
    printf '%s %s -o %s\n' "$CXX" "$*" "$OUT/$name" >> "$OUT/commands.txt"
    if $CXX "$@" -o "$OUT/$name" > "$OUT/$name.build.log" 2>&1; then
        if grep -q 'warning:' "$OUT/$name.build.log"; then say "  note: $name built with warnings ($OUT/$name.build.log)"; fi
        return 0
    fi
    fail=$((fail + 1)); say "FAIL $name: build (see $OUT/$name.build.log)"; tail -5 "$OUT/$name.build.log"; return 1
}
# runs NAME COUNT SECONDS [ARGS...]: every run of OUT/NAME must exit 0 within SECONDS.
runs() {
    local name=$1 count=$2 seconds=$3; shift 3
    local failures=0 i rc started=$SECONDS log="$OUT/$name.run.log"
    for ((i = 1; i <= count; ++i)); do
        limited "$seconds" "$OUT/$name" "$@" > "$log" 2>&1; rc=$?
        if [ $rc -ne 0 ]; then failures=$((failures + 1)); cp "$log" "$OUT/$name.failed-run-$i.log"; fi
    done
    if [ $failures -eq 0 ]; then
        pass=$((pass + 1)); say "PASS $name runs=$count seconds=$((SECONDS - started)) $(tail -1 "$log")"
    else
        fail=$((fail + 1)); say "FAIL $name failures=$failures/$count (logs $OUT/$name.failed-run-*.log)"; tail -2 "$log"
    fi
}
single() { local name=$1 seconds=$2; shift 2; build "$name" "$@" && runs "$name" 1 "$seconds"; }

say "WP4 host tests: $($CXX --version 2>&1 | head -1); out=$OUT"
shasum -a 256 $POOL $GUARD $V/diagnostic_thread_labels.h $V/trace_runner.cpp $N/native_xma_context_pool.h \
    $N/native_audio_render_client.h $N/native_audio_queue.h $N/guest_execution_guard.h $N/guest_scalar_memory.h \
    android/tests/native_xma_concurrency_test.cpp android/tests/native_xma_equivalence_driver.cpp \
    android/tests/native_audio_concurrency_test.cpp android/tests/trace_diagnostics_concurrency_test.cpp \
    android/tests/guest_execution_guard_concurrency_test.cpp $XMA_HOST $H/compare_xma_equivalence.py \
    > "$OUT/sources.sha256"

# ---- 0. Can a TSan program run on this host at all? ----
cat > "$OUT/tsan_probe.cpp" <<'EOF'
#include <thread>
int main() { int x = 0; std::thread t([&] { x = 1; }); t.join(); return x == 1 ? 0 : 1; }
EOF
TSAN_USABLE=0
if $CXX $TSAN "$OUT/tsan_probe.cpp" -o "$OUT/tsan_probe" > "$OUT/tsan_probe.build.log" 2>&1; then
    if limited 30 "$OUT/tsan_probe" > "$OUT/tsan_probe.run.log" 2>&1; then
        TSAN_USABLE=1; say "TSan runtime: usable"
    else
        say "TSan runtime: UNUSABLE here (a trivial threaded TSan program exits $?; $OUT/tsan_probe.run.log)."
        say "  TSan tests are compiled and linked; their runs are SKIP. Run this script on Linux or an older macOS."
    fi
else
    say "TSan: this compiler cannot build -fsanitize=thread ($OUT/tsan_probe.build.log)"
fi

# ---- 1. Concurrency tests: plain (repeated), UBSan and TSan ----
concurrency() { # concurrency NAME RUNS SECONDS COMPILER_ARGS...
    local name=$1 count=$2 seconds=$3; shift 3
    build "$name" $PLAIN "$@" && runs "$name" "$count" "$seconds"
    build "$name.ubsan" $UBSAN "$@" && runs "$name.ubsan" 3 $((seconds * 4))
    if build "$name.tsan" $TSAN "$@"; then
        if [ $TSAN_USABLE = 1 ]; then runs "$name.tsan" 3 $((seconds * 30))
        else skip=$((skip + 1)); say "SKIP $name.tsan: compiled and linked; TSan runtime unusable on this host"; fi
    fi
}
say "== 1. Concurrency tests"
for mode in 0 2; do
    concurrency native_xma_concurrency.threads$mode "$RUNS" 150 -DRECOMP_GUEST_THREADS=$mode -I$N -I$H \
        android/tests/native_xma_concurrency_test.cpp $POOL $XMA_HOST
    concurrency guest_execution_guard_concurrency.threads$mode "$RUNS" 60 -DRECOMP_GUEST_THREADS=$mode -I$N \
        android/tests/guest_execution_guard_concurrency_test.cpp $GUARD
done
concurrency native_audio_concurrency "$RUNS" 60 -I$N android/tests/native_audio_concurrency_test.cpp
concurrency trace_diagnostics_concurrency $(( (RUNS + 4) / 5 )) 120 -I$V -I$H \
    android/tests/trace_diagnostics_concurrency_test.cpp $H/android_log_capture.cpp

# ---- 2. Existing tests against the WP4 sources ----
say "== 2. Existing tests"
# The pool tests pin the cooperative contract (failure() describes the latest access): default build.
for t in native_xma_context_pool native_xma_loop; do
    single $t 60 $PLAIN -fno-strict-aliasing -fwrapv -I$N -I$H android/tests/${t}_test.cpp $POOL $XMA_HOST $GUARD
done
for mode in 0 2; do
    for t in guest_execution_guard guest_scalar_memory guest_experimental_gpu_clock guest_uptime_data; do
        single $t.threads$mode 60 $PLAIN -DRECOMP_GUEST_THREADS=$mode -I$N -I$T android/tests/${t}_test.cpp $GUARD
    done
done
for model in DEFAULT STRONG ATOMIC; do
    flag=; [ $model = DEFAULT ] || flag=-DRECOMP_GUEST_MEMORY_$model=1
    single guest_memory_model.$model 600 -std=c++20 -O2 -pthread $flag -I$N -I$T $GUARD android/tests/guest_memory_model_test.cpp
done
for std in 17 20; do
    for t in guest_audio_frame native_audio_queue native_audio_render_client; do
        single $t.cxx$std 30 -std=c++$std -g -O1 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -I$N android/tests/${t}_test.cpp
    done
done
# Registry-level audio tests: the qualified runtime objects compiled with the WP4 audio headers.
RT="-std=c++20 -g -O1 -UNDEBUG -Wall -Wextra -Wpedantic -Werror"
mkdir -p "$OUT/rt"; rt_ok=1; pids=
for f in guest_address_space guest_xex_header guest_thread_environment guest_thread_storage_pool guest_import_registry \
         guest_native_initialization xenos_command_stream guest_cooperative_executor guest_execution_guard; do
    printf '%s %s -I%s -c %s -o %s\n' "$CXX" "$RT" "$N" "$N/$f.cpp" "$OUT/rt/$f.o" >> "$OUT/commands.txt"
    $CXX $RT -I$N -c $N/$f.cpp -o "$OUT/rt/$f.o" > "$OUT/rt/$f.log" 2>&1 &
    pids="$pids $!"
done
for p in $pids; do wait $p || rt_ok=0; done
if [ $rt_ok = 1 ]; then
    for t in guest_audio_configuration guest_audio_render_registration guest_native_callback_worker; do
        single $t.qualified_runtime 120 $RT -I$N android/tests/${t}_test.cpp "$OUT"/rt/*.o
    done
    # The WP4 pool behind the qualified registry, whose dispatch holds R across
    # XMACreateContext/XMAReleaseContext (R > L_xma) and whose mapNativeAudioPhysical takes R.
    for mode in 0 2; do
        single guest_native_xma_registry.threads$mode 120 $PLAIN -DRECOMP_GUEST_THREADS=$mode -I$N -I$H \
            android/tests/guest_native_xma_registry_test.cpp $POOL $XMA_HOST "$OUT"/rt/*.o
    done
else
    fail=$((fail + 1)); say "FAIL qualified runtime objects did not build (logs $OUT/rt/*.log)"
fi

# ---- 3. Cooperative equivalence of the XMA pool ----
say "== 3. Cooperative XMA equivalence"
PREVIOUS_POOL=${PREVIOUS_POOL:-local-analysis/attempts/v11/src/native_xma_context_pool.cpp}
PREVIOUS_POOL_SHA256=ab259646c89b25f169183c6910c33ffcdbbd44f0dbcaf3eb59d33c80711d7ef2
if [ "$(sha "$PREVIOUS_POOL")" = $PREVIOUS_POOL_SHA256 ]; then
    EQ="-std=c++20 -g -O1 -w -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG -I$N -I$H android/tests/native_xma_equivalence_driver.cpp"
    if build xma_equivalence.previous $EQ "$PREVIOUS_POOL" $XMA_HOST && build xma_equivalence.wp4 $EQ $POOL $XMA_HOST; then
        started=$SECONDS
        if python3 $H/compare_xma_equivalence.py "$OUT/xma_equivalence.previous" "$OUT/xma_equivalence.wp4" \
                "$STEPS" "$SEEDS" > "$OUT/xma_equivalence.log" 2>&1; then
            pass=$((pass + 1)); say "PASS xma_equivalence seconds=$((SECONDS - started)) $(tail -1 "$OUT/xma_equivalence.log")"
        else
            fail=$((fail + 1)); say "FAIL xma_equivalence (see $OUT/xma_equivalence.log)"; tail -3 "$OUT/xma_equivalence.log"
        fi
    fi
else
    skip=$((skip + 1)); say "SKIP xma_equivalence: $PREVIOUS_POOL is missing or not sha256 $PREVIOUS_POOL_SHA256"
fi

# ---- 4. Negative controls ----
if [ "$NEGATIVE" = 1 ]; then
    say "== 4. Negative controls (the previous code must fail these workloads)"
    control() { # control NAME RUNS SECONDS COMPILER_ARGS...: at least one run must fail
        local name=$1 count=$2 seconds=$3; shift 3
        local failures=0 i
        printf '%s %s -o %s\n' "$CXX" "-std=c++20 -g -O1 -w $*" "$OUT/$name" >> "$OUT/commands.txt"
        if ! $CXX -std=c++20 -g -O1 -w "$@" -o "$OUT/$name" > "$OUT/$name.build.log" 2>&1; then
            skip=$((skip + 1)); say "SKIP $name: build failed ($OUT/$name.build.log)"; return
        fi
        controls=$((controls + 1))
        for ((i = 1; i <= count; ++i)); do limited "$seconds" "$OUT/$name" > "$OUT/$name.run.log" 2>&1 || failures=$((failures + 1)); done
        if [ $failures -gt 0 ]; then triggered=$((triggered + 1)); say "EXPECTED_FAILURE $name failures=$failures/$count"
        else say "CONTROL_NOT_TRIGGERED $name: the previous code passed $count runs (weaker evidence on this host)"; fi
    }
    if [ "$(sha "$PREVIOUS_POOL")" = $PREVIOUS_POOL_SHA256 ]; then
        printf 'namespace recomp::audio { unsigned nativeXmaLocksHeldForTest() noexcept { return 0; } }\n' > "$OUT/locks_stub.cpp"
        control native_xma_concurrency.previous 10 30 -I$N -I$H android/tests/native_xma_concurrency_test.cpp \
            "$PREVIOUS_POOL" "$OUT/locks_stub.cpp" $XMA_HOST
    fi
    PREVIOUS_GUARD=${PREVIOUS_GUARD:-local-analysis/attempts/v11/src/guest_execution_guard.cpp}
    if [ "$(sha "$PREVIOUS_GUARD")" = 34eed77e28af2be598ff815a7fe56951700cf5c910809ede4beb7753739778f7 ]; then
        control guest_execution_guard_concurrency.previous 10 30 -I$N android/tests/guest_execution_guard_concurrency_test.cpp "$PREVIOUS_GUARD"
    else skip=$((skip + 1)); say "SKIP guard control: $PREVIOUS_GUARD missing or changed"; fi
    # The previous audio headers, from the git objects they had before WP4.
    mkdir -p "$OUT/previous_audio"
    if git cat-file blob b209d18ae602098b0d06a9bd3841603a83654c87 > "$OUT/previous_audio/native_audio_render_client.h" 2>/dev/null &&
       git cat-file blob 3c54a415bced3d8ebb3787b250dc885560f81166 > "$OUT/previous_audio/native_audio_queue.h" 2>/dev/null; then
        control native_audio_concurrency.previous 10 30 -I"$OUT/previous_audio" -I$N android/tests/native_audio_concurrency_test.cpp
    else skip=$((skip + 1)); say "SKIP audio control: previous header objects unavailable"; fi
    control trace_diagnostics_concurrency.previous 5 60 -DTRACE_DIAGNOSTICS_LEGACY_CONCURRENCY=1 -I$V -I$H \
        android/tests/trace_diagnostics_concurrency_test.cpp $H/android_log_capture.cpp
fi

tsan=unusable; [ $TSAN_USABLE = 1 ] && tsan=usable
say "WP4_HOST_TESTS pass=$pass fail=$fail skip=$skip negative_controls_triggered=$triggered/$controls tsan_runtime=$tsan seconds=$SECONDS out=$OUT"
[ $fail -eq 0 ]
