#!/bin/sh
# Checks check_milestone.sh's reporting (#1027) on a throwaway CTest project: a test that passes, one that
# fails every time, and one that fails once and then passes. The failing one must be failed, the other
# flaky and never passed, the exit codes 1 and then 3, and the logs and environment kept. Then its UBSan
# and static analysis groups (#1069): unavailable without their build or compile database, run in a UBSan
# build, and refused for one built from another revision; checks needing fetched samples left out without
# them; and group names, separated by spaces or commas, an unknown one refused.
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
        if [ "$1" = samples ]; then # stands for W1's checks, which fail without the samples they need
            echo 'add_test(NAME needs_samples COMMAND sh -c "echo no samples; exit 1")'
            echo 'set_tests_properties(needs_samples PROPERTIES LABELS "cpu;acceptance;w1")'
        fi
        echo 'set_tests_properties(always_passes passes_on_retry PROPERTIES LABELS cpu)'
        if [ "$1" = with-failure ]; then echo 'set_tests_properties(always_fails PROPERTIES LABELS cpu)'; fi
    } > "$scratch/project/CMakeLists.txt"
    shift
    cmake --no-warn-unused-cli -S "$scratch/project" -B "$scratch/build" "$@" > /dev/null
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

# Without the fetched samples, the checks that need them are left out and reported unavailable, not failed:
# without either, R1's and W1's; with the render samples alone, W1's.
sleep 1
project samples -DMAYA_RENDER_SAMPLES:PATH="$scratch/render" -DMAYA_WORLD_SAMPLES:PATH="$scratch/world"
touch "$scratch/build/once"
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/samples.txt" 2>&1
code=$?
expect "checks without their samples are left out, and the rest pass (got $code)" test "$code" -eq 0
expect "and they are reported unavailable" sh -c "grep -q 'unavailable  R1 checks' '$scratch/samples.txt' && grep -q 'unavailable  W1 checks' '$scratch/samples.txt'"
expect "and not run" sh -c "! grep -q 'needs_samples' '$scratch/samples.txt'"
mkdir -p "$scratch/render/Models"
sleep 1
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/samples2.txt" 2>&1
expect "with the render samples alone, only W1's are unavailable" \
    sh -c "grep -q 'unavailable  W1 checks.*no fetched world samples' '$scratch/samples2.txt' && ! grep -q 'unavailable  R1 checks' '$scratch/samples2.txt'"
mkdir -p "$scratch/world/models"
sleep 1
MAYA_CHECK_GROUPS=cpu sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/samples3.txt" 2>&1
code=$?
expect "with both, W1's checks run (here, and fail: got $code)" test "$code" -eq 1
expect "and nothing is unavailable" sh -c "! grep -q 'unavailable' '$scratch/samples3.txt'"
project flaky-only
touch "$scratch/build/once"

# UBSan: unavailable without a UBSan build, saying how to make one.
MAYA_CHECK_GROUPS=ubsan MAYA_UBSAN_BUILD="$scratch/no-ubsan" sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/fourth.txt" 2>&1
code=$?
expect "no UBSan build is not a failure (got $code)" test "$code" -eq 0
expect "and is reported unavailable, with how to make one" grep -q "unavailable  UBSan: no UBSan build in $scratch/no-ubsan; make one with cmake" "$scratch/fourth.txt"
# In a UBSan build (the same project, configured as one), its CPU checks run there.
cmake --no-warn-unused-cli -S "$scratch/project" -B "$scratch/ubsan" -DMAYA_ENABLE_SANITIZERS:BOOL=ON -DMAYA_SANITIZERS:STRING=undefined > /dev/null
touch "$scratch/ubsan/once"
MAYA_CHECK_GROUPS=ubsan MAYA_UBSAN_BUILD="$scratch/ubsan" sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/fifth.txt" 2>&1
code=$?
expect "a passing UBSan build exits 0 (got $code)" test "$code" -eq 0
expect "and is reported passed" grep -q "passed       UBSan (CPU checks, without the R1 and W1 data runs)" "$scratch/fifth.txt"
logs=$(sed -n 's/^Logs and environment: //p' "$scratch/fifth.txt")
expect "its log is kept" test -f "$logs/ubsan.log"
# A UBSan build of another revision than the build checked is not used.
mkdir -p "$scratch/build/generated" "$scratch/ubsan/generated"
echo '    return {"aaaaaaaaaaaa", "unspecified", "none", "AppleClang"};' > "$scratch/build/generated/build_info.cpp"
echo '    return {"bbbbbbbbbbbb", "unspecified", "undefined", "AppleClang"};' > "$scratch/ubsan/generated/build_info.cpp"
MAYA_CHECK_GROUPS=ubsan MAYA_UBSAN_BUILD="$scratch/ubsan" sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/sixth.txt" 2>&1
expect "a UBSan build of another revision is reported unavailable" \
    grep -q "unavailable  UBSan: $scratch/ubsan was built from bbbbbbbbbbbb, not aaaaaaaaaaaa; build it again" "$scratch/sixth.txt"
expect "and not run" sh -c "! grep -q 'passed       UBSan' '$scratch/sixth.txt'"

# Static analysis: unavailable without a compile database.
MAYA_CHECK_GROUPS=analysis sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/seventh.txt" 2>&1
code=$?
expect "no compile database is not a failure (got $code)" test "$code" -eq 0
expect "and static analysis is reported unavailable" grep -q "unavailable  Static analysis: no compile database in $scratch/build" "$scratch/seventh.txt"

# Groups: commas separate them as spaces do; a name that is not a group runs nothing and exits 2.
MAYA_CHECK_GROUPS=cpu,analysis sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/eighth.txt" 2>&1
expect "commas separate groups" sh -c "grep -q 'passed       CPU checks' '$scratch/eighth.txt' && grep -q 'Static analysis' '$scratch/eighth.txt'"
MAYA_CHECK_GROUPS=ubSan sh "$here/check_milestone.sh" "$scratch/build" > "$scratch/ninth.txt" 2>&1
code=$?
expect "an unknown group exits 2 (got $code)" test "$code" -eq 2
expect "and names it" grep -q "Unknown group in MAYA_CHECK_GROUPS: ubSan" "$scratch/ninth.txt"

if [ "$problems" -eq 0 ]; then
    rm -rf "$scratch"
    echo "check_milestone.sh self-test passed"
    exit 0
fi
echo "check_milestone.sh self-test: $problems problems; output kept in $scratch"
exit 1
