#!/bin/bash
# run_tests.sh — Run a simulator against all test cases and compare outputs
# Usage: ./run_tests.sh <path_to_run.sh>
# Example: ./run_tests.sh ../run.sh

if [ -z "$1" ]; then
    echo "Usage: $0 <path_to_run.sh>"
    echo "  run.sh should accept: ./run.sh input.json output.json"
    exit 1
fi

RUN_CMD="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTS_DIR="$SCRIPT_DIR"

PASS=0
FAIL=0
ERRORS=""

for test_dir in "$TESTS_DIR"/*/; do
    test_name=$(basename "$test_dir")
    input="$test_dir/input.json"
    expected="$test_dir/output.json"
    actual="/tmp/ooo470_test_${test_name}.json"
    desc=$(cat "$test_dir/desc.txt" 2>/dev/null | head -1)

    if [ ! -f "$input" ] || [ ! -f "$expected" ]; then
        continue
    fi

    # Run student simulator
    $RUN_CMD "$input" "$actual" 2>/dev/null
    if [ $? -ne 0 ]; then
        printf "%-40s CRASH  %s\n" "$test_name" "$desc"
        FAIL=$((FAIL+1))
        ERRORS="$ERRORS\n  $test_name: simulator crashed"
        continue
    fi

    # Compare outputs
    diff_result=$(python3 -c "
import json, sys
try:
    ref = json.load(open('$expected'))
    my = json.load(open('$actual'))
    if len(ref) != len(my):
        print(f'CYCLE_COUNT: expected {len(ref)}, got {len(my)}')
        sys.exit(1)
    for i in range(len(ref)):
        for key in ref[i]:
            if ref[i][key] != my[i][key]:
                print(f'CYCLE_{i}_{key}')
                sys.exit(1)
    print('OK')
except Exception as e:
    print(f'ERROR: {e}')
    sys.exit(1)
" 2>&1)

    if [ "$diff_result" = "OK" ]; then
        printf "%-40s PASS   %s\n" "$test_name" "$desc"
        PASS=$((PASS+1))
    else
        printf "%-40s FAIL   %s  (%s)\n" "$test_name" "$desc" "$diff_result"
        FAIL=$((FAIL+1))
        ERRORS="$ERRORS\n  $test_name: $diff_result"
    fi

    rm -f "$actual"
done

echo ""
echo "================================"
echo "Results: $PASS passed, $FAIL failed out of $((PASS+FAIL)) tests"
if [ $FAIL -gt 0 ]; then
    echo -e "\nFailed tests:$ERRORS"
fi
echo "================================"
