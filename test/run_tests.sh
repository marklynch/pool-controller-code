#!/bin/bash
# Auto-discovers and runs all test_*.c files in this directory.
# Convention: test_foo.c is compiled with ../main/foo.c (plus log_capture.c).
# test_replay.c is handled separately at the end — it replays sample logs
# under ./samples/ through message_decoder.

set -euo pipefail
cd "$(dirname "$0")"

SHARED=(log_capture.c unknown_buffer_stub.c nvs_stub.c)
PASS=0
FAIL=0
ERRORS=()
SKIPPED=()

# Tests that are currently broken from upstream API drift. Re-enable each
# entry once the test has been updated to match the current decoder/state
# interfaces. Keep this list small — entries here are tech debt, not
# permanent exclusions.
SKIP_LIST=(
)

is_skipped() {
    local needle="$1"
    if [ ${#SKIP_LIST[@]} -eq 0 ]; then return 1; fi
    for s in "${SKIP_LIST[@]}"; do
        if [ "$s" = "$needle" ]; then return 0; fi
    done
    return 1
}

for test_src in test_*.c; do
    if [ "$test_src" = "test_replay.c" ] || [ "$test_src" = "test_framing.c" ]; then
        continue
    fi
    if is_skipped "$test_src"; then
        SKIPPED+=("$test_src")
        continue
    fi

    module="${test_src#test_}"
    module="${module%.c}"
    main_src="../main/${module}.c"
    binary="./run_${module}"

    # message_decoder.c shares its data/discovery packet classification
    # with framing.c - link it in for that one module. The lighting color
    # name table lives in lighting_colors.c, and the channel state name table
    # in channel_states.c; both are needed by message_decoder and
    # mqtt_commands. message_decoder also feeds filter_pump_type.c.
    extra_srcs=()
    if [ "$module" = "message_decoder" ]; then
        extra_srcs=(../main/framing.c ../main/lighting_colors.c ../main/channel_states.c ../main/filter_pump_type.c)
    elif [ "$module" = "mqtt_commands" ]; then
        extra_srcs=(../main/lighting_colors.c ../main/channel_states.c)
    fi

    echo "========================================"
    echo "  Compiling: $test_src"
    echo "========================================"

    if gcc -fno-common -I. -I.. -o "$binary" "$test_src" "$main_src" ${extra_srcs[@]+"${extra_srcs[@]}"} "${SHARED[@]}" 2>&1; then
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

# Run framing tests.
if [ -f test_framing.c ]; then
    echo "========================================"
    echo "  Compiling: test_framing.c"
    echo "========================================"

    if gcc -fno-common -I. -I.. -o ./run_framing test_framing.c "${SHARED[@]}" 2>&1; then
        if ./run_framing; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            ERRORS+=("test_framing.c (framing failures)")
        fi
        rm -f ./run_framing
    else
        FAIL=$((FAIL + 1))
        ERRORS+=("test_framing.c (compile error)")
    fi
fi

# Replay sample logs through the real decoder.
if [ -f test_replay.c ]; then
    echo "========================================"
    echo "  Compiling: test_replay.c"
    echo "========================================"

    if gcc -fno-common -I. -I.. -o ./run_replay test_replay.c ../main/message_decoder.c ../main/framing.c ../main/lighting_colors.c ../main/channel_states.c ../main/filter_pump_type.c "${SHARED[@]}" 2>&1; then
        if ./run_replay; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            ERRORS+=("test_replay.c (replay mismatches)")
        fi
        rm -f ./run_replay
    else
        FAIL=$((FAIL + 1))
        ERRORS+=("test_replay.c (compile error)")
    fi
fi

echo ""
echo "========================================"
echo "  Overall: $PASS suite(s) passed, $FAIL failed"
if [ ${#ERRORS[@]} -gt 0 ]; then
    for e in "${ERRORS[@]}"; do
        echo "  FAILED: $e"
    done
fi
if [ ${#SKIPPED[@]} -gt 0 ]; then
    for s in "${SKIPPED[@]}"; do
        echo "  SKIPPED: $s (see SKIP_LIST in run_tests.sh)"
    done
fi
echo "========================================"

[ $FAIL -eq 0 ]
