#!/bin/sh
# Runs the milestone's automated checks in groups and reports any group that could not run
# (docs/acceptance.md). CPU checks need nothing else; GPU checks need a Metal device; windowed checks
# also need a logged-in desktop session. Static analysis runs Clang's analyzer on the engine and the
# applications, and UBSan runs the CPU checks again in an UndefinedBehaviorSanitizer build (#1069).
# Checks of R1 and W1 need their fetched samples; without them they are left out and reported unavailable.
# Interactive checks are listed as manual.
#
# Each run keeps its logs in <build>/milestone-logs/<time>/ (#1027): the environment, each group's CTest
# output, and the output of every failed test run again on its own. A test that fails again is failed; one
# that passes alone is flaky, which is never reported as passed.
#
# Usage: tools/check_milestone.sh [build directory]    (default: build)
# Exits 0 when every group that ran passed, 1 when a test failed, 3 when tests were only flaky, and 2
# without a configured build or with an unknown group. MAYA_CHECK_GROUPS names the groups to run, separated
# by spaces or commas, from cpu, analysis, ubsan, gpu, and windowed (default: all of them; the self-test runs
# some alone). MAYA_UBSAN_BUILD is the UBSan build (default: <build>-ubsan), which the script uses but never
# builds.
set -u
build=${1:-build}
build=${build%/}
groups=$(echo "${MAYA_CHECK_GROUPS:-all}" | tr ',' ' ')
for group in $groups; do
    case $group in
        all | cpu | analysis | ubsan | gpu | windowed) ;;
        *) echo "Unknown group in MAYA_CHECK_GROUPS: $group (the groups are cpu, analysis, ubsan, gpu, and windowed)"
           exit 2 ;;
    esac
done
if [ -z "$groups" ]; then
    echo "MAYA_CHECK_GROUPS names no group (the groups are cpu, analysis, ubsan, gpu, and windowed)"
    exit 2
fi
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
wants() { case " $groups " in *" all "* | *" $1 "*) return 0 ;; esac; return 1; }

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

# Runs one group of a build tree's tests into <slug>.log, shown as it runs. Each failed test runs again alone.
run() {
    name=$1
    slug=$2
    tree=$3
    shift 3
    log="$logs/$slug.log"
    echo "== $name (log: $log)"
    rm -f "$tree/Testing/Temporary/LastTestsFailed.log"
    { ctest --test-dir "$tree" --output-on-failure "$@" 2>&1; echo $? > "$log.status"; } | tee "$log"
    code=$(cat "$log.status")
    rm -f "$log.status"
    if [ "$code" -eq 0 ]; then
        note "passed       $name"
        return
    fi
    tests=$(sed -n 's/^[0-9]*://p' "$tree/Testing/Temporary/LastTestsFailed.log" 2>/dev/null)
    if [ -z "$tests" ]; then
        note "FAILED       $name: CTest itself failed (exit $code); see $log"
        failed=1
        return
    fi
    for test in $tests; do
        rerun="$logs/$slug.$test.rerun.log"
        echo "== $test failed; running it again alone (log: $rerun)"
        if ctest --test-dir "$tree" --output-on-failure -R "^$test\$" > "$rerun" 2>&1; then
            note "FLAKY        $name: $test failed, then passed alone; see $log and $rerun"
            flaky=1
        else
            note "FAILED       $name: $test, and again alone; see $log and $rerun"
            failed=1
        fi
    done
}

# R1 and W1 are assembled from fetched samples, never committed: without them their checks (labels r1 and
# w1) are left out of every group and reported unavailable, rather than failing.
cached() { sed -n "s/^$1:PATH=//p" "$build/CMakeCache.txt" 2>/dev/null; }
render_samples=$(cached MAYA_RENDER_SAMPLES)
world_samples=$(cached MAYA_WORLD_SAMPLES)
without=""
if [ ! -d "${render_samples:-$source_dir/build/render-samples}/Models" ]; then
    without="r1|w1"
    note "unavailable  R1 checks (maya_r1_*): no fetched render samples; run tools/fetch_render_samples.sh"
    note "unavailable  W1 checks (maya_w1_*, the S1 and soak smoke runs): no fetched render samples; run tools/fetch_render_samples.sh and tools/fetch_world_samples.sh"
elif [ ! -d "${world_samples:-$source_dir/build/world-samples}/models" ]; then
    without="w1"
    note "unavailable  W1 checks (maya_w1_*, the S1 and soak smoke runs): no fetched world samples; run tools/fetch_world_samples.sh"
fi
leaving() { # the CTest arguments that leave out labels $1 (a regular expression), and those without samples
    excluded="$1${without:+${1:+|}$without}"
    if [ -n "$excluded" ]; then echo "-LE $excluded"; fi
}

if wants cpu; then
    # shellcheck disable=SC2046 # leaving prints separate words
    run "CPU checks (no GPU, no window)" cpu "$build" -L cpu $(leaving "")
fi

# Static analysis: Clang's analyzer on every engine and application source in the build's compile database.
if wants analysis; then
    echo "== Static analysis (log: $logs/analysis.log)"
    if ! command -v python3 > /dev/null 2>&1; then
        note "unavailable  Static analysis: no python3"
    else
        python3 "$source_dir/tools/static_analysis.py" "$build" "$logs/analysis.log"
        code=$?
        sources=$(sed -n 's/^Static analysis: \([0-9]*\) sources.*/\1/p' "$logs/analysis.log" 2>/dev/null)
        case $code in
            0) note "passed       Static analysis ($sources sources)" ;;
            2) note "unavailable  Static analysis: $(tail -n 1 "$logs/analysis.log" 2>/dev/null || echo "it did not run")" ;;
            *) note "FAILED       Static analysis: $(tail -n 1 "$logs/analysis.log" 2>/dev/null); see $logs/analysis.log"
               failed=1 ;;
        esac
    fi
fi

# UBSan: the CPU checks again in an UndefinedBehaviorSanitizer build of the same revision, without R1's and
# W1's data runs, which take too long under it. The script uses that build but never makes it.
if wants ubsan; then
    ubsan=${MAYA_UBSAN_BUILD:-$build-ubsan}
    revision() { sed -n 's/.*return {"\([^"]*\)".*/\1/p' "$1/generated/build_info.cpp" 2>/dev/null; }
    if [ ! -f "$ubsan/CTestTestfile.cmake" ] || ! grep -q '^MAYA_ENABLE_SANITIZERS:BOOL=ON' "$ubsan/CMakeCache.txt" 2>/dev/null ||
        ! grep -q '^MAYA_SANITIZERS:STRING=.*undefined' "$ubsan/CMakeCache.txt" 2>/dev/null; then
        note "unavailable  UBSan: no UBSan build in $ubsan; make one with cmake -S . -B $ubsan -DMAYA_ENABLE_SANITIZERS=ON -DMAYA_SANITIZERS=undefined && cmake --build $ubsan"
    elif [ -n "$(revision "$build")" ] && [ "$(revision "$ubsan")" != "$(revision "$build")" ]; then
        built=$(revision "$ubsan")
        note "unavailable  UBSan: $ubsan was built from ${built:-an unknown revision}, not $(revision "$build"); build it again"
    else
        run "UBSan (CPU checks, without the R1 and W1 data runs)" ubsan "$ubsan" -L cpu -LE "r1|w1"
    fi
fi

if wants gpu || wants windowed; then
    if system_profiler SPDisplaysDataType 2>/dev/null | grep -q "Metal"; then
        # shellcheck disable=SC2046
        if wants gpu; then run "GPU checks (Metal, headless)" gpu "$build" -L gpu $(leaving smoke); fi
        # Windows need a desktop session: launchctl reports Aqua for a logged-in GUI session.
        if ! wants windowed; then
            :
        elif [ "$(launchctl managername 2>/dev/null)" = "Aqua" ]; then
            # shellcheck disable=SC2046
            run "Windowed checks (player, editor, sample, desktop host)" windowed "$build" -L smoke $(leaving "")
        else
            note "unavailable  Windowed checks: no desktop session (launchctl reports $(launchctl managername 2>/dev/null || echo nothing))"
        fi
    else
        if wants gpu; then note "unavailable  GPU checks: no Metal device"; fi
        if wants windowed; then note "unavailable  Windowed checks: no Metal device"; fi
    fi
fi
if wants gpu; then
    note "manual       Interactive checks: docs/acceptance.md, steps 1-9, physics and behavior steps 1-8, rendering and content steps 1-8, world editing steps 1-9, the world workflow steps 1-9, and W1 steps 1-4"
    note "manual       Benchmark baselines: a Release build and benchmarks/*.benchmark, including P1, A1, R1, S1, and the soak (docs/performance.md)"
fi
printf "\nSummary:%s\n\nLogs and environment: %s\n" "$summary" "$logs"
if [ "$failed" -ne 0 ]; then exit 1; fi
if [ "$flaky" -ne 0 ]; then exit 3; fi
exit 0
