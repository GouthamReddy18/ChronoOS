#!/bin/sh
set -eu

CHRONO=./build/chrono
test_root=$(mktemp -d ./build/demo-test-XXXXXX) || {
    echo "test_demo: could not create a private test directory" >&2
    exit 1
}
test_root=$(cd "$test_root" && pwd -P)
chmod 700 "$test_root"
mkdir "$test_root/unavailable" "$test_root/interrupted" "$test_root/manual-eof" "$test_root/manual-interrupted" "$test_root/manual-fake"
chmod 700 "$test_root/unavailable" "$test_root/interrupted"
demo_pid=
cleanup() {
    if [ -n "$demo_pid" ]; then
        kill -INT "$demo_pid" 2>/dev/null || true
        wait "$demo_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

"$CHRONO" demo --help >"$test_root/help.log" 2>&1
grep -F 'Usage: chrono demo' "$test_root/help.log" >/dev/null
"$CHRONO" demo-interactive --help >"$test_root/interactive-help.log" 2>&1
grep -F 'demo-interactive' "$test_root/interactive-help.log" >/dev/null
"$CHRONO" demo-manual --help >"$test_root/manual-help.log" 2>&1
grep -F 'Usage: chrono demo-manual' "$test_root/manual-help.log" >/dev/null
"$CHRONO" dashboard --help >"$test_root/dashboard-help.log" 2>&1
grep -F 'Usage: chrono dashboard' "$test_root/dashboard-help.log" >/dev/null
if "$CHRONO" demo unexpected >"$test_root/usage.log" 2>&1; then
    echo "test_demo: invalid demo arguments were accepted" >&2
    exit 1
fi
grep -F 'demo' "$test_root/usage.log" >/dev/null
if "$CHRONO" demo-interactive unexpected >"$test_root/interactive-usage.log" 2>&1; then
    echo "test_demo: invalid interactive demo arguments were accepted" >&2
    exit 1
fi
grep -F 'demo-interactive' "$test_root/interactive-usage.log" >/dev/null
if "$CHRONO" demo-manual unexpected >"$test_root/manual-usage.log" 2>&1; then
    echo "test_demo: invalid manual demo arguments were accepted" >&2
    exit 1
fi
grep -F 'demo-manual' "$test_root/manual-usage.log" >/dev/null

set +e
TMPDIR="$test_root/unavailable" CHRONO_CRIU_PATH="$test_root/missing-criu" \
    CHRONO_CRIU_UNPRIVILEGED=1 "$CHRONO" demo >"$test_root/unavailable.log" 2>&1
unavailable_status=$?
set -e
if [ "$unavailable_status" -ne 1 ]; then
    cat "$test_root/unavailable.log" >&2
    echo "test_demo: unavailable CRIU demo exited $unavailable_status, expected 1" >&2
    exit 1
fi
grep -F 'CRIU preflight: BLOCKED' "$test_root/unavailable.log" >/dev/null
grep -F 'Checkpoint metadata state: FAILED' "$test_root/unavailable.log" >/dev/null
grep -F 'Checkpoint creation:  FAIL' "$test_root/unavailable.log" >/dev/null
grep -F 'Restore:              NOT ATTEMPTED' "$test_root/unavailable.log" >/dev/null
grep -F 'DEMO INCOMPLETE' "$test_root/unavailable.log" >/dev/null
if grep -F 'Stage 6/8' "$test_root/unavailable.log" >/dev/null; then
    echo "test_demo: restore stage ran after checkpoint failure" >&2
    exit 1
fi
identity=$(sed -n 's/.*Launched managed workload PID \([0-9][0-9]*\), STARTTIME \([0-9][0-9]*\)\..*/\1 \2/p' "$test_root/unavailable.log" | head -n 1)
set -- $identity
if [ "$#" -ne 2 ] || "$CHRONO" status "$1" "$2" >"$test_root/status.log" 2>&1; then
    echo "test_demo: original managed workload was not confirmed stopped" >&2
    exit 1
fi
grep -F 'Diagnostic log:' "$test_root/unavailable.log" >/dev/null

# EOF at the live-monitor prompt aborts before checkpoint, but must safely stop
# the exact managed workload created by this run. Missing CRIU makes this
# independent of host CRIU availability and exercises only the failure path.
set +e
printf 'l\n' | TMPDIR="$test_root/manual-eof" CHRONO_CRIU_PATH="$test_root/missing-criu" \
    CHRONO_CRIU_UNPRIVILEGED=1 "$CHRONO" demo-manual >"$test_root/manual-eof.log" 2>&1
manual_eof_status=$?
set -e
if [ "$manual_eof_status" -ne 1 ]; then
    cat "$test_root/manual-eof.log" >&2
    echo "test_demo: manual EOF run exited $manual_eof_status, expected 1" >&2
    exit 1
fi
grep -F 'MANAGED WORKLOAD' "$test_root/manual-eof.log" >/dev/null
grep -F 'Checkpoint creation:  NOT ATTEMPTED' "$test_root/manual-eof.log" >/dev/null
grep -F 'Restore:              NOT ATTEMPTED' "$test_root/manual-eof.log" >/dev/null
grep -F 'Cleanup:              PASS' "$test_root/manual-eof.log" >/dev/null
manual_identity=$(sed -n 's/.*Managed workload: PID=\([0-9][0-9]*\).*STARTTIME=\([0-9][0-9]*\).*/\1 \2/p' "$test_root/manual-eof.log" | head -n 1)
set -- $manual_identity
if [ "$#" -ne 2 ] || "$CHRONO" status "$1" "$2" >"$test_root/manual-eof-status.log" 2>&1; then
    echo "test_demo: manual EOF cleanup did not stop its managed workload" >&2
    exit 1
fi

# Exercise the manual workflow's real adapter path with a deterministic fake
# that passes preflight and fails dump. It must mark metadata failed, avoid
# restore, and clean up only its managed workload.
: >"$test_root/fake-criu.log"
set +e
printf 'l\nc\n' | TMPDIR="$test_root/manual-eof" \
    CHRONO_CRIU_PATH="$(pwd)/build/fake_criu" CHRONO_CRIU_UNPRIVILEGED=1 \
    CHRONO_TEST_ALLOW_FAKE_CRIU=1 \
    CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED=1 CHRONO_FAKE_CRIU_FAIL=dump \
    CHRONO_FAKE_CRIU_LOG="$test_root/fake-criu.log" \
    "$CHRONO" demo-manual >"$test_root/manual-dump-failure.log" 2>&1
manual_dump_status=$?
set -e
if [ "$manual_dump_status" -ne 1 ]; then
    cat "$test_root/manual-dump-failure.log" >&2
    echo "test_demo: fake dump failure run exited $manual_dump_status, expected 1" >&2
    exit 1
fi
grep -F 'CRIU preflight passed' "$test_root/manual-dump-failure.log" >/dev/null
grep -F 'Checkpoint metadata state: FAILED' "$test_root/manual-dump-failure.log" >/dev/null
grep -F 'Restore:              NOT ATTEMPTED' "$test_root/manual-dump-failure.log" >/dev/null
grep -F 'Cleanup:              PASS' "$test_root/manual-dump-failure.log" >/dev/null
grep '^check' "$test_root/fake-criu.log" >/dev/null
grep '^dump' "$test_root/fake-criu.log" >/dev/null
if grep '^restore' "$test_root/fake-criu.log" >/dev/null; then
    echo "test_demo: restore ran after fake checkpoint failure" >&2
    exit 1
fi

# Fake CRIU can exercise dashboard orchestration, but its fabricated PID must
# not be adopted as a restored workload. Drive each destructive control and
# explicit SIGTERM confirmation through the dashboard input channel.
mkfifo "$test_root/manual-fake/input"
exec 4<>"$test_root/manual-fake/input"
CHRONO_CRIU_PATH="$(pwd)/build/fake_criu" CHRONO_CRIU_UNPRIVILEGED=1 \
    CHRONO_TEST_ALLOW_FAKE_CRIU=1 \
    CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED=1 \
    CHRONO_DASHBOARD_PROCESS_LIMIT=2 \
    CHRONO_FAKE_CRIU_LOG="$test_root/manual-fake/fake.log" \
    TMPDIR="$test_root/manual-fake" "$CHRONO" demo-manual <&4 4>&- \
    >"$test_root/manual-fake/run.log" 2>&1 &
demo_pid=$!
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F '[l]aunch sample' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
if ! grep -F '[l]aunch sample' "$test_root/manual-fake/run.log" >/dev/null; then
    cat "$test_root/manual-fake/run.log" >&2
    echo "test_demo: fake manual demo did not reach checkpoint input" >&2
    exit 1
fi
printf 'p\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F 'CHRONOOS PROCESS BROWSER' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F 'CHRONOOS PROCESS BROWSER' "$test_root/manual-fake/run.log" >/dev/null
printf '1\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F 'READ-ONLY MONITOR' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F 'READ-ONLY MONITOR' "$test_root/manual-fake/run.log" >/dev/null
printf 'b\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && [ "$(grep -cF 'CHRONOOS PROCESS BROWSER' "$test_root/manual-fake/run.log" || true)" -lt 2 ]; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
printf 'x\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F '[INPUT] Received: x - CLOSE' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F '[INPUT] Received: x - CLOSE' "$test_root/manual-fake/run.log" >/dev/null
printf 'l\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F '[SUCCESS] Managed workload launched; process identity and counter verified.' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
if ! grep -F '[SUCCESS] Managed workload launched; process identity and counter verified.' "$test_root/manual-fake/run.log" >/dev/null; then
    cat "$test_root/manual-fake/run.log" >&2
    echo "test_demo: managed workload launch was not visibly acknowledged" >&2
    exit 1
fi
if grep '^restore' "$test_root/manual-fake/fake.log" >/dev/null; then
    echo "test_demo: fake restore ran before user confirmation" >&2
    exit 1
fi
printf 'c\n' >&4
attempt=0
while [ "$attempt" -lt 120 ] && ! grep -F 'CHECKPOINT VALIDATED; ORIGINAL LIVE' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F 'CHECKPOINT VALIDATED; ORIGINAL LIVE' "$test_root/manual-fake/run.log" >/dev/null
printf 't\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F '[WAITING] Press y to confirm termination of the original process, or n to cancel.' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F '[WAITING] Press y to confirm termination of the original process, or n to cancel.' "$test_root/manual-fake/run.log" >/dev/null
printf 'n\n' >&4
identity=$(sed -n 's/.*Managed workload: PID=\([0-9][0-9]*\).*STARTTIME=\([0-9][0-9]*\).*/\1 \2/p' "$test_root/manual-fake/run.log" | head -n 1)
set -- $identity
if [ "$#" -ne 2 ] || ! "$CHRONO" status "$1" "$2" >"$test_root/manual-fake/cancel-status.log" 2>&1; then
    echo "test_demo: declining termination confirmation stopped the original workload" >&2
    exit 1
fi
grep -F '[INFO] Termination cancelled; the original process remains running.' "$test_root/manual-fake/run.log" >/dev/null
attempt=0
while [ "$attempt" -lt 80 ] && [ "$(grep -cF 'CHECKPOINT VALIDATED; ORIGINAL LIVE' "$test_root/manual-fake/run.log" || true)" -lt 2 ]; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
[ "$(grep -cF 'CHECKPOINT VALIDATED; ORIGINAL LIVE' "$test_root/manual-fake/run.log" || true)" -ge 2 ]
printf 't\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && [ "$(grep -cF '[WAITING] Press y to confirm termination of the original process, or n to cancel.' "$test_root/manual-fake/run.log" || true)" -lt 2 ]; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
[ "$(grep -cF '[WAITING] Press y to confirm termination of the original process, or n to cancel.' "$test_root/manual-fake/run.log" || true)" -ge 2 ]
printf 'y\n' >&4
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F 'ORIGINAL TERMINATED AND REAPED' "$test_root/manual-fake/run.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1; attempt=$((attempt + 1))
done
grep -F 'ORIGINAL TERMINATED AND REAPED' "$test_root/manual-fake/run.log" >/dev/null
printf 'r\n' >&4
set +e
wait "$demo_pid"
manual_fake_status=$?
set -e
demo_pid=
exec 4>&-
if [ "$manual_fake_status" -ne 1 ]; then
    cat "$test_root/manual-fake/run.log" >&2
    echo "test_demo: fake restore fixture exited $manual_fake_status, expected identity verification failure" >&2
    exit 1
fi
grep -F 'PASS configured backend leave-running operation' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'Checkpoint metadata state: COMPLETE; artifacts: valid.' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'PASS original PID=' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'SYSTEM STATUS' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'p opens read-only browser' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'MANAGED WORKLOAD' "$test_root/manual-fake/run.log" >/dev/null
grep -E 'PID=[0-9]+ UID=[0-9]+ STATE=[A-Z] START=[0-9]+' "$test_root/manual-fake/run.log" >/dev/null
grep -E 'Live in-memory counter: [0-9]+' "$test_root/manual-fake/run.log" >/dev/null
grep -E '/proc comm \(display only\): [0-9]+' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'Validation: VALIDATED' "$test_root/manual-fake/run.log" >/dev/null
metadata_path=$(sed -n 's/^  META //p' "$test_root/manual-fake/run.log" | head -n 1)
image_path=$(sed -n 's/^  IMAGES //p' "$test_root/manual-fake/run.log" | head -n 1)
[ -n "$metadata_path" ] && [ -f "$metadata_path" ]
[ -n "$image_path" ] && [ -d "$image_path" ]
grep -F 'Live in-memory counter:' "$test_root/manual-fake/run.log" >/dev/null
grep -F '/proc comm (display only):' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'EVENT HISTORY (latest events; session only)' "$test_root/manual-fake/run.log" >/dev/null
grep -F '[INPUT] Received: c - CHECKPOINT' "$test_root/manual-fake/run.log" >/dev/null
grep -F '[INPUT] Received: t - STOP REQUEST' "$test_root/manual-fake/run.log" >/dev/null
grep -F '[INPUT] Received: y - CONFIRM' "$test_root/manual-fake/run.log" >/dev/null
grep -F '[INPUT] Received: r - RESTORE / REFRESH' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'FAIL CRIU restore adapter' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'test-only backend restore output cannot be adopted as a restored process' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'Recovery history:' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'Restore: failed; no restored process adopted' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'Final status: DEMO INCOMPLETE' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'CHRONOOS DEMONSTRATION INCOMPLETE' "$test_root/manual-fake/run.log" >/dev/null
grep -F 'FINAL RECOVERY HISTORY' "$test_root/manual-fake/run.log" >/dev/null
grep '^restore' "$test_root/manual-fake/fake.log" >/dev/null
echo "SIMULATION: fake CRIU exercised manual orchestration; fake restore PID was rejected by identity verification"

mkfifo "$test_root/manual-interrupted/input"
exec 3<>"$test_root/manual-interrupted/input"
TMPDIR="$test_root/manual-interrupted" CHRONO_CRIU_PATH="$test_root/missing-criu" \
    CHRONO_CRIU_UNPRIVILEGED=1 "$CHRONO" demo-manual <&3 >"$test_root/manual-interrupted.log" 2>&1 &
demo_pid=$!
attempt=0
while [ "$attempt" -lt 60 ] && ! grep -F '[l]aunch sample' "$test_root/manual-interrupted.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1
    attempt=$((attempt + 1))
done
if ! grep -F '[l]aunch sample' "$test_root/manual-interrupted.log" >/dev/null; then
    cat "$test_root/manual-interrupted.log" >&2
    echo "test_demo: manual demo did not reach its interactive monitoring prompt" >&2
    exit 1
fi
printf 'l\n' >&3
attempt=0
while [ "$attempt" -lt 80 ] && ! grep -F '[SUCCESS] Managed workload launched; process identity and counter verified.' "$test_root/manual-interrupted.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1
    attempt=$((attempt + 1))
done
grep -F '[SUCCESS] Managed workload launched; process identity and counter verified.' "$test_root/manual-interrupted.log" >/dev/null
kill -INT "$demo_pid"
set +e
wait "$demo_pid"
manual_interrupt_status=$?
set -e
demo_pid=
exec 3>&-
if [ "$manual_interrupt_status" -ne 130 ]; then
    cat "$test_root/manual-interrupted.log" >&2
    echo "test_demo: interrupted manual demo exited $manual_interrupt_status, expected 130" >&2
    exit 1
fi
grep -F 'Interrupted. Remaining stages are not reported as passed.' "$test_root/manual-interrupted.log" >/dev/null
grep -F 'Cleanup:              PASS' "$test_root/manual-interrupted.log" >/dev/null

TMPDIR="$test_root/interrupted" CHRONO_CRIU_PATH="$test_root/missing-criu" \
    CHRONO_CRIU_UNPRIVILEGED=1 "$CHRONO" demo >"$test_root/interrupted.log" 2>&1 &
demo_pid=$!
attempt=0
while [ "$attempt" -lt 50 ] && ! grep -F 'Stage 3/8' "$test_root/interrupted.log" >/dev/null; do
    if ! kill -0 "$demo_pid" 2>/dev/null; then break; fi
    sleep 0.1
    attempt=$((attempt + 1))
done
if ! grep -F 'Stage 3/8' "$test_root/interrupted.log" >/dev/null; then
    cat "$test_root/interrupted.log" >&2
    echo "test_demo: demo did not reach monitoring for interruption test" >&2
    exit 1
fi
kill -INT "$demo_pid"
set +e
wait "$demo_pid"
interrupt_status=$?
set -e
demo_pid=
if [ "$interrupt_status" -ne 130 ]; then
    cat "$test_root/interrupted.log" >&2
    echo "test_demo: interrupted demo exited $interrupt_status, expected 130" >&2
    exit 1
fi
grep -F 'Run interrupted' "$test_root/interrupted.log" >/dev/null
grep -F 'Cleanup:              PASS' "$test_root/interrupted.log" >/dev/null

echo "demo CLI, blocked-checkpoint, EOF, cleanup, no-restore, and interruption tests passed"
echo "test artifacts retained at $test_root"
