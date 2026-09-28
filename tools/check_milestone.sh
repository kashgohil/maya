#!/bin/sh
# Runs the milestone's automated checks in three groups and reports any group that could not run
# (docs/acceptance.md). CPU checks need nothing else; GPU checks need a Metal device; windowed checks
# also need a logged-in desktop session. Interactive checks are listed as manual.
# Usage: tools/check_milestone.sh [build directory]    (default: build)
set -u
build=${1:-build}
if [ ! -f "$build/CTestTestfile.cmake" ]; then
    echo "No configured build in $build: run cmake -S . -B $build && cmake --build $build first."
    exit 2
fi
summary=""
status=0
note() { summary="$summary
  $1"; }
run() {
    name=$1
    shift
    echo "== $name"
    if ctest --test-dir "$build" --output-on-failure "$@"; then note "passed       $name"; else note "FAILED       $name"; status=1; fi
}

run "CPU checks (no GPU, no window)" -L cpu

if system_profiler SPDisplaysDataType 2>/dev/null | grep -q "Metal"; then
    run "GPU checks (Metal, headless)" -L gpu -LE smoke
    # Windows need a desktop session: launchctl reports Aqua for a logged-in GUI session.
    if [ "$(launchctl managername 2>/dev/null)" = "Aqua" ]; then
        run "Windowed checks (player, editor, sample, desktop host)" -L smoke
    else
        note "unavailable  Windowed checks: no desktop session (launchctl reports $(launchctl managername 2>/dev/null || echo nothing))"
    fi
else
    note "unavailable  GPU checks: no Metal device"
    note "unavailable  Windowed checks: no Metal device"
fi
note "manual       Interactive checks: docs/acceptance.md, steps 1-9"
note "manual       Benchmark baselines: a Release build and benchmarks/*.benchmark (docs/performance.md)"
printf "\nSummary:%s\n" "$summary"
exit $status
