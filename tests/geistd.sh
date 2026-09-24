#!/bin/sh
# geistd.sh — the daemon end to end: ops suite over the Unix socket, TCP with
# and without the token, --stdio, and the short-calls timing against
# geist-serve. Needs a GGUF (GEIST_MODEL, else the CI reference); skips otherwise.
set -eu
cd "$(dirname "$0")/.."
MODEL=${GEIST_MODEL:-geistlib/gguf_artifacts/smollm2-360m-instruct-q8_0.gguf}
[ -f "$MODEL" ] || { echo "geistd: no model at $MODEL — skipped"; exit 0; }
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-2}
SOCK=/tmp/geistd-test-$$.sock; LOG=/tmp/geistd-test-$$.log
fail=0
ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fail=1; }
wait_log() { i=0; while [ $i -lt "$2" ]; do grep -q "$1" "$LOG" && return 0; sleep 1; i=$((i+1)); done; return 1; }
trap 'kill ${D1:-} ${D2:-} ${S1:-} 2>/dev/null || true; wait 2>/dev/null || true; rm -f "$SOCK" "$LOG"' EXIT

# --- Unix socket + ops --------------------------------------------------------
./geistd "$MODEL" --socket "$SOCK" --sessions 3 2>"$LOG" & D1=$!
wait_log "listening" 60 || { bad "geistd start: $(tail -2 "$LOG")"; exit 1; }
[ "$(stat -f %Lp "$SOCK" 2>/dev/null || stat -c %a "$SOCK")" = 600 ] && ok "socket mode 0600" || bad "socket mode $(stat -f %Lp "$SOCK" 2>/dev/null || stat -c %a "$SOCK")"
python3 -u tests/geistd_ops.py "$SOCK" || fail=1

# --- --stdio ------------------------------------------------------------------
out=$(python3 -c 'import struct,json,sys; h=json.dumps({"op":"info"}).encode(); sys.stdout.buffer.write(struct.pack("<II",len(h),0)+h)' \
      | ./geistd "$MODEL" --stdio 2>/dev/null | python3 -c 'import sys,struct,json; d=sys.stdin.buffer.read(); hl,bl=struct.unpack("<II",d[:8]); print(json.loads(d[8:8+hl])["model"])')
case "$out" in smollm2*) ok "--stdio answers info" ;; *) bad "--stdio: $out" ;; esac

# --- TCP: off-loopback needs a token; loopback does not -----------------------
PORT=$(( 27000 + $$ % 1000 ))
( ./geistd "$MODEL" --host 0.0.0.0 --port $PORT 2>&1 | grep -q 'set GEISTD_TOKEN' ) && ok "off-loopback without token refused at start" || bad "off-loopback without token"
GEISTD_TOKEN=0123456789abcdef0123456789abcdef ./geistd "$MODEL" --host 0.0.0.0 --port $PORT 2>>"$LOG" & D2=$!
i=0; while ! nc -z 127.0.0.1 $PORT 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done
out=$(python3 - $PORT <<'PY'
import sys, os; sys.path.insert(0, "clients"); import geistd
port = int(sys.argv[1])
try:
    geistd.Client(host="127.0.0.1", port=port).info(); print("no-token:accepted")
except geistd.GeistdError as e: print("no-token:refused")
try:
    geistd.Client(host="127.0.0.1", port=port, token="wrong").info(); print("bad-token:accepted")
except geistd.GeistdError as e: print("bad-token:refused")
print("good-token:" + geistd.Client(host="127.0.0.1", port=port, token="0123456789abcdef0123456789abcdef").info()["model"])
PY
)
printf '%s\n' "$out" | grep -q 'no-token:refused' && ok "tcp without hello refused" || bad "tcp without hello: $out"
printf '%s\n' "$out" | grep -q 'bad-token:refused' && ok "tcp wrong token refused" || bad "tcp wrong token: $out"
printf '%s\n' "$out" | grep -q 'good-token:smollm2' && ok "tcp with token serves" || bad "tcp with token: $out"
kill $D2; wait $D2 2>/dev/null || true

# --- the point: short calls against a resident session vs the chat API -------
SPORT=$(( 28000 + $$ % 1000 ))
./geist-serve "$MODEL" --port $SPORT 2>>"$LOG" & S1=$!
i=0; while ! curl -sf http://127.0.0.1:$SPORT/health >/dev/null 2>&1 && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done
python3 -u tests/agent_short_calls.py "$SOCK" $SPORT 5 && ok "resident session beats stateless re-prefill" || bad "short-calls timing"

[ $fail -eq 0 ] && echo "geistd: all passed"
exit $fail
