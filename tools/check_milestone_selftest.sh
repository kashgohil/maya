#!/bin/sh
# Checks check_milestone.sh's reporting (#1027) on a throwaway CTest project: a test that passes, one that
# fails every time, and one that fails once and then passes. The failing one must be failed, the other
# flaky and never passed, the exit codes 1 and then 3, and the logs and environment kept.
# Usage: tools/check_milestone_selftest.sh [scratch folder]    (CTest: maya_check_milestone_selftest)
set -u
here=$(cd "$(dirname "$0")" && pwd)
scratch=${1:-${TMPDIR:-/tmp}/maya-check-selftest-$$}
rm -rf "$scratch"
mkdir -p "$scratch/project"
problems=0
expect() { # description, then a command that must succeed
    what=$1
    shift
    if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; problems=$((problems + 1)); fi
}

project() { # writes and configures a project; $1 is "with-failure" or "flaky-only"
    rm -rf "$scratch/project" "$scratch/build"
    mkdir -p "$scratch/project"
    {
        echo 'cmake_minimum_required(VERSION 3.20)'
        echo 'project(check_selftest NONE)'
        echo 'enable_testing()'
        echo 'add_test(NAME always_passes COMMAND sh -c "exit 0")'
        echo 'add_test(NAME passes_on_retry COMMAND sh -c "if [ -f once ]; then exit 0; fi; touch once; echo first run fails; exit 1")'
        if [ "$1" = with-failure ]; then echo 'add_test(NAME always_fails COMMAND sh -c "echo it fails; exit 1")'; fi
        echo 'set_tests_properties(always_passes passes_on_retry PROPERTIES LABELS cpu)'
        if [ "$1" = with-failure ]; then echo 'set_tests_properties(always_fails PROPERTIES LABELS cpu)'; fi
    } > "$scratch/project/CMakeLists.txt"
    cmake -S "$scratch/project" -B "$scratch/build" > /dev/null
}

# A failing test and a flaky one.
project with-failure
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/first.txt" 2>&1
code=$?
expect "a failure exits 1 (got $code)" test "$code" -eq 1
expect "the failing test is reported failed" grep -q "FAILED       CPU checks (no GPU, no window): always_fails, and again alone" "$scratch/first.txt"
expect "the flaky test is reported flaky" grep -q "FLAKY        CPU checks (no GPU, no window): passes_on_retry failed, then passed alone" "$scratch/first.txt"
expect "the group is not reported passed" sh -c "! grep -q 'passed       CPU checks' '$scratch/first.txt'"
logs=$(sed -n 's/^Logs and environment: //p' "$scratch/first.txt")
expect "the summary names the log folder ($logs)" test -d "$logs"
expect "the group's log holds the failing test's output" grep -q "it fails" "$logs/cpu.log"
expect "each failed test's run alone is kept" test -f "$logs/cpu.always_fails.rerun.log" -a -f "$logs/cpu.passes_on_retry.rerun.log"
expect "the environment records the revision, load, and thermal state" \
    sh -c "grep -q '^revision: ' '$logs/environment.txt' && grep -q '^load: ' '$logs/environment.txt' && grep -q '^thermal:' '$logs/environment.txt'"

# Only a flaky test: still not a pass.
sleep 1 # a new log folder
project flaky-only
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/second.txt" 2>&1
code=$?
expect "flaky alone exits 3 (got $code)" test "$code" -eq 3
expect "and is reported flaky" grep -q "FLAKY        CPU checks (no GPU, no window): passes_on_retry" "$scratch/second.txt"

# Everything passing: 0, and passed.
sleep 1
rm -f "$scratch/build/once"
touch "$scratch/build/once"
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/third.txt" 2>&1
code=$?
expect "all passing exits 0 (got $code)" test "$code" -eq 0
expect "and is reported passed" grep -q "passed       CPU checks (no GPU, no window)" "$scratch/third.txt"

if [ "$problems" -eq 0 ]; then
    rm -rf "$scratch"
    echo "check_milestone.sh self-test passed"
    exit 0
fi
echo "check_milestone.sh self-test: $problems problems; output kept in $scratch"
exit 1
