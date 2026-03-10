#!/bin/bash
# Auto-discovers and runs all test_*.c files in this directory.
# Convention: test_foo.c is compiled with ../main/foo.c

set -euo pipefail
cd "$(dirname "$0")"

PASS=0
FAIL=0
ERRORS=()

for test_src in test_*.c; do
    module="${test_src#test_}"       # strip leading "test_"
    module="${module%.c}"            # strip trailing ".c"
    main_src="../main/${module}.c"
    binary="./run_${module}"

    echo "========================================"
    echo "  Compiling: $test_src"
    echo "========================================"

    if gcc -I. -I.. -o "$binary" "$test_src" "$main_src" 2>&1; then
        if "$binary"; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            ERRORS+=("$test_src (test failures)")
        fi
        rm -f "$binary"
    else
        FAIL=$((FAIL + 1))
        ERRORS+=("$test_src (compile error)")
    fi
done

echo ""
echo "========================================"
echo "  Overall: $PASS suite(s) passed, $FAIL failed"
if [ ${#ERRORS[@]} -gt 0 ]; then
    for e in "${ERRORS[@]}"; do
        echo "  FAILED: $e"
    done
fi
echo "========================================"

[ $FAIL -eq 0 ]
