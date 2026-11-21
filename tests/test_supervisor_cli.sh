#!/bin/sh
set -eu

CHRONO=./build/chrono
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-supervisor-cli-XXXXXX") || exit 1
state_home=$test_dir/state
first_id=
second_id=
cleanup() {
    if [ -n "$first_id" ]; then XDG_STATE_HOME=$state_home "$CHRONO" stop "$first_id" >/dev/null 2>&1 || :; fi
    if [ -n "$second_id" ]; then XDG_STATE_HOME=$state_home "$CHRONO" stop "$second_id" >/dev/null 2>&1 || :; fi
    rm -rf -- "$test_dir"
}
on_signal() { signal=$1; trap - EXIT HUP INT TERM; cleanup; exit "$signal"; }
trap cleanup EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

expect_failure() {
    if XDG_STATE_HOME=$state_home "$@" >"$test_dir/output" 2>&1; then
        cat "$test_dir/output" >&2
        echo "test_supervisor_cli: command unexpectedly succeeded: $*" >&2
        exit 1
    fi
}

"$CHRONO" --help >"$test_dir/help" 2>&1 || :
if ! grep -F 'protect -- PROGRAM' "$test_dir/help" >/dev/null; then
    echo "test_supervisor_cli: usage does not document protect" >&2
    cat "$test_dir/help" >&2
    exit 1
fi

expect_failure "$CHRONO" protect
expect_failure "$CHRONO" protect -- /definitely/not/a/chronoos-program

XDG_STATE_HOME=$state_home "$CHRONO" protect -- /bin/sleep 60 >"$test_dir/first" 2>&1
first_id=$(sed -n 's/^ChronoOS Process ID: //p' "$test_dir/first")
[ -n "$first_id" ] || { cat "$test_dir/first" >&2; echo "test_supervisor_cli: first registration ID missing" >&2; exit 1; }
grep -Fx 'State: RUNNING' "$test_dir/first" >/dev/null

XDG_STATE_HOME=$state_home "$CHRONO" protect -- /bin/sleep 60 >"$test_dir/second" 2>&1
second_id=$(sed -n 's/^ChronoOS Process ID: //p' "$test_dir/second")
[ -n "$second_id" ] && [ "$first_id" != "$second_id" ] || { echo "test_supervisor_cli: registrations did not receive unique IDs" >&2; exit 1; }

XDG_STATE_HOME=$state_home "$CHRONO" list >"$test_dir/list"
grep -F "$first_id" "$test_dir/list" >/dev/null
grep -F "$second_id" "$test_dir/list" >/dev/null
XDG_STATE_HOME=$state_home "$CHRONO" status "$first_id" >"$test_dir/status"
grep -Fx "ChronoOS Process ID: $first_id" "$test_dir/status" >/dev/null
XDG_STATE_HOME=$state_home "$CHRONO" stop "$first_id" >"$test_dir/stopped"
grep -F 'state=EXITED' "$test_dir/stopped" >/dev/null
XDG_STATE_HOME=$state_home "$CHRONO" status "$second_id" >"$test_dir/status-second"
grep -Fx 'State: RUNNING' "$test_dir/status-second" >/dev/null
XDG_STATE_HOME=$state_home "$CHRONO" stop "$second_id" >"$test_dir/stopped-second"
grep -F 'state=EXITED' "$test_dir/stopped-second" >/dev/null

expect_failure "$CHRONO" status cp-999999
echo "supervisor CLI registration tests passed"
