#!/bin/sh
# Runs the milestone's automated checks in three groups and reports any group that could not run
# (docs/acceptance.md). CPU checks need nothing else; GPU checks need a Metal device; windowed checks
# also need a logged-in desktop session. Interactive checks are listed as manual.
#
# Each run keeps its logs in <build>/milestone-logs/<time>/ (#1027): the environment, each group's CTest
# output, and the output of every failed test run again on its own. A test that fails again is failed; one
# that passes alone is flaky, which is never reported as passed.
#
# Usage: tools/check_milestone.sh [build directory]    (default: build)
# Exits 0 when every group that ran passed, 1 when a test failed, 3 when tests were only flaky, and 2
# without a configured build. MAYA_CHECK_GROUPS=cpu runs the CPU group alone (for the self-test).
set -u
build=${1:-build}
if [ ! -f "$build/CTestTestfile.cmake" ]; then
    echo "No configured build in $build: run cmake -S . -B $build && cmake --build $build first."
    exit 2
fi
source_dir=$(cd "$(dirname "$0")/.." && pwd)
logs="$build/milestone-logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$logs"
summary=""
failed=0
flaky=0
note() { summary="$summary
  $1"; }

# The environment the run had: what makes a failure reproducible, or not.
{
    echo "date: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "revision: $(git -C "$source_dir" describe --always --dirty 2>/dev/null || echo unknown)"
    echo "build: $build ($(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$build/CMakeCache.txt" 2>/dev/null | sed 's/^$/no build type/'))"
    echo "machine: $(sysctl -n hw.model 2>/dev/null || echo unknown), macOS $(sw_vers -productVersion 2>/dev/null || echo unknown)"
    echo "load: $(sysctl -n vm.loadavg 2>/dev/null || uptime)"
    echo "thermal:"
    pmset -g therm 2>/dev/null | sed 's/^/  /'
} > "$logs/environment.txt"

# Runs one group into <slug>.log, shown as it runs. Each failed test runs again alone.
run() {
    name=$1
    slug=$2
    shift 2
    log="$logs/$slug.log"
    echo "== $name (log: $log)"
    rm -f "$build/Testing/Temporary/LastTestsFailed.log"
    { ctest --test-dir "$build" --output-on-failure "$@" 2>&1; echo $? > "$log.status"; } | tee "$log"
    code=$(cat "$log.status")
    rm -f "$log.status"
    if [ "$code" -eq 0 ]; then
        note "passed       $name"
        return
    fi
    tests=$(sed -n 's/^[0-9]*://p' "$build/Testing/Temporary/LastTestsFailed.log" 2>/dev/null)
    if [ -z "$tests" ]; then
        note "FAILED       $name: CTest itself failed (exit $code); see $log"
        failed=1
        return
    fi
    for test in $tests; do
        rerun="$logs/$slug.$test.rerun.log"
        echo "== $test failed; running it again alone (log: $rerun)"
        if ctest --test-dir "$build" --output-on-failure -R "^$test\$" > "$rerun" 2>&1; then
            note "FLAKY        $name: $test failed, then passed alone; see $log and $rerun"
            flaky=1
        else
            note "FAILED       $name: $test, and again alone; see $log and $rerun"
            failed=1
        fi
    done
}

run "CPU checks (no GPU, no window)" cpu -L cpu

if [ "${MAYA_CHECK_GROUPS:-all}" = "cpu" ]; then
    :
elif system_profiler SPDisplaysDataType 2>/dev/null | grep -q "Metal"; then
    run "GPU checks (Metal, headless)" gpu -L gpu -LE smoke
    # Windows need a desktop session: launchctl reports Aqua for a logged-in GUI session.
    if [ "$(launchctl managername 2>/dev/null)" = "Aqua" ]; then
        run "Windowed checks (player, editor, sample, desktop host)" windowed -L smoke
    else
        note "unavailable  Windowed checks: no desktop session (launchctl reports $(launchctl managername 2>/dev/null || echo nothing))"
    fi
else
    note "unavailable  GPU checks: no Metal device"
    note "unavailable  Windowed checks: no Metal device"
fi
if [ "${MAYA_CHECK_GROUPS:-all}" != "cpu" ]; then
    if [ ! -f "$build/r1/r1.scene" ]; then
        note "unavailable  R1 checks (maya_r1_*, R1 packaging): no fetched samples; run tools/fetch_render_samples.sh"
    fi
    note "manual       Interactive checks: docs/acceptance.md, steps 1-9, physics and behavior steps 1-8, and rendering and content steps 1-8"
    note "manual       Benchmark baselines: a Release build and benchmarks/*.benchmark, including P1, A1, and R1 (docs/performance.md)"
fi
printf "\nSummary:%s\n\nLogs and environment: %s\n" "$summary" "$logs"
if [ "$failed" -ne 0 ]; then exit 1; fi
if [ "$flaky" -ne 0 ]; then exit 3; fi
exit 0
