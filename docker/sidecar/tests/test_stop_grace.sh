#!/usr/bin/env bash
#
# test_stop_grace.sh — the sidecar entrypoint's forwarded shutdown is BOUNDED.
#
# `docker stop` sends SIGTERM to entrypoint.sh (PID 1) and SIGKILLs the
# container 10 s later by default. The entrypoint forwards the signal and
# then waits for the daemons' own clean exits through wait_daemons_bounded,
# which gives them STOP_GRACE_SECS and then ends whatever is left — so a
# daemon that cannot finish (a wedged broker link once hung the bridge's
# last publish until the SIGKILL, exit 137) never costs the container a
# non-zero exit for a stop that was asked for.
#
# This lifts wait_daemons_bounded from entrypoint.sh (the way
# privacy_witness_kernel/tests/test_config_render.sh lifts its siblings) and
# runs it against real background children: two that honor SIGTERM, then one
# that ignores it. It also pins the default grace under Docker's 10 s and
# checks both of run()'s exit paths go through the bounded wait.
#
# Usage: bash docker/sidecar/tests/test_stop_grace.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTRYPOINT_SH="${TEST_ENTRYPOINT_SH:-$HERE/../entrypoint.sh}"

FAILED=0
ok() { echo "ok   - $*"; }
fail() { echo "FAIL - $*" >&2; FAILED=1; }

# shell_function FILE NAME — the source of the top-level `NAME() {` ... `}`.
shell_function() {
    awk -v name="$2" '
        $0 == name "() {" { grab = 1 }
        grab { print }
        grab && /^}$/ { grab = 0; done = 1 }
        END { if (!done) exit 1 }
    ' "$1"
}

if ! lifted=$(shell_function "$ENTRYPOINT_SH" wait_daemons_bounded); then
    fail "entrypoint.sh: wait_daemons_bounded is not a top-level function (nothing to test)"
    exit 1
fi
ok "entrypoint.sh: wait_daemons_bounded is a top-level function (lifted)"

# The function's two dependencies, as entrypoint.sh defines them.
LOG_LINES=()
log() { LOG_LINES+=("$*"); }
DAEMON_PIDS=()
eval "$lifted"

now_ms() { date +%s%3N; }

# --- 1. Daemons that honor SIGTERM: all exit on their own, quickly, return 0.
sleep 30 & DAEMON_PIDS+=($!)
sleep 30 & DAEMON_PIDS+=($!)
kill -TERM "${DAEMON_PIDS[@]}"
t0=$(now_ms)
if wait_daemons_bounded 5; then
    elapsed=$(( $(now_ms) - t0 ))
    if [ "$elapsed" -lt 2000 ]; then
        ok "daemons that exit on SIGTERM: returns 0 at once (${elapsed} ms, grace unused)"
    else
        fail "daemons that exit on SIGTERM: took ${elapsed} ms, expected well under the 5 s grace"
    fi
else
    fail "daemons that exit on SIGTERM: returned 1 (reported a forced end that did not happen)"
fi
for pid in "${DAEMON_PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then fail "pid $pid still alive after the wait"; fi
done
if [ "${#LOG_LINES[@]}" -eq 0 ]; then
    ok "nothing logged for a clean exit"
else
    fail "logged on a clean exit: ${LOG_LINES[*]}"
fi

# --- 2. One daemon ignores SIGTERM: the wait ends at the grace, kills it,
#        names it, returns 1 — and the one that honored SIGTERM is not blamed.
DAEMON_PIDS=()
LOG_LINES=()
bash -c 'trap "" TERM; sleep 30' & stubborn=$!; DAEMON_PIDS+=("$stubborn")
sleep 30 & DAEMON_PIDS+=($!)
sleep 0.2   # let the stubborn child install its trap before the TERM lands
kill -TERM "${DAEMON_PIDS[@]}"
t0=$(now_ms)
if wait_daemons_bounded 1; then
    fail "a daemon that ignores SIGTERM: returned 0 (should report the forced end)"
else
    elapsed=$(( $(now_ms) - t0 ))
    if [ "$elapsed" -ge 900 ] && [ "$elapsed" -lt 4000 ]; then
        ok "a daemon that ignores SIGTERM: returns 1 after the 1 s grace (${elapsed} ms)"
    else
        fail "a daemon that ignores SIGTERM: returned after ${elapsed} ms, expected about the 1 s grace"
    fi
fi
if kill -0 "$stubborn" 2>/dev/null; then
    fail "the SIGTERM-ignoring daemon ($stubborn) survived the bounded wait"
    kill -KILL "$stubborn" 2>/dev/null || true
else
    ok "the SIGTERM-ignoring daemon was ended"
fi
case "${LOG_LINES[*]:-}" in
    *"did not exit within 1s"*"$stubborn"*|*"$stubborn"*"did not exit within 1s"*)
        ok "the forced end is logged with the pid and the grace: ${LOG_LINES[*]}" ;;
    *)  fail "no 'did not exit within 1s' line naming pid $stubborn; logged: ${LOG_LINES[*]:-<nothing>}" ;;
esac
case "${LOG_LINES[*]:-}" in
    *"${DAEMON_PIDS[1]}"*) fail "the daemon that honored SIGTERM (${DAEMON_PIDS[1]}) was named as a straggler" ;;
    *) ok "the daemon that honored SIGTERM is not named" ;;
esac

# --- 3. The grace stays under Docker's default 10 s stop timeout, and both
#        of run()'s exit paths wait through the bounded function.
default_grace=$(sed -n -E 's/^STOP_GRACE_SECS="\$\{SECURACV_STOP_GRACE_SECS:-([0-9]+)\}"$/\1/p' "$ENTRYPOINT_SH" | head -n1)
if [ -n "$default_grace" ] && [ "$default_grace" -lt 10 ]; then
    ok "entrypoint.sh: the default grace (${default_grace} s, SECURACV_STOP_GRACE_SECS) is under Docker's 10 s stop timeout"
else
    fail "entrypoint.sh: STOP_GRACE_SECS default is '${default_grace:-unset}', expected a number under 10"
fi
calls=$(grep -cF "wait_daemons_bounded \"\$STOP_GRACE_SECS\"" "$ENTRYPOINT_SH" || true)
if [ "$calls" -ge 2 ]; then
    ok "entrypoint.sh: both exit paths (requested stop, service exit) wait through wait_daemons_bounded (${calls} calls)"
else
    fail "entrypoint.sh: expected both exit paths to call wait_daemons_bounded \"\$STOP_GRACE_SECS\", found ${calls}"
fi
if grep -qE '^[[:space:]]+wait \|\| true$' "$ENTRYPOINT_SH"; then
    fail "entrypoint.sh: an unbounded 'wait || true' remains in the run() exit paths"
else
    ok "entrypoint.sh: no unbounded wait remains on the exit paths"
fi

if [ "$FAILED" -ne 0 ]; then
    echo "❌ test_stop_grace: failures above" >&2
    exit 1
fi
echo "✅ test_stop_grace: the forwarded shutdown is bounded"
