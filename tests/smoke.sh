#!/bin/sh
# smoke.sh — transport contract through --stdio, over TCP, and with an
# inherited listener (LISTEN_FDS). Needs a GGUF: GEIST_MODEL, else the CI
# reference model inside the engine checkout; skips (exit 0) without one.
set -eu
cd "$(dirname "$0")/.."

MODEL=${GEIST_MODEL:-geistlib/gguf_artifacts/smollm2-360m-instruct-q8_0.gguf}
if [ ! -f "$MODEL" ]; then
    echo "smoke: no model at $MODEL — skipped (set GEIST_MODEL)"
    exit 0
fi
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-2}

fail=0
check() { # name expected-substring actual
    if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -5; fail=1; fi
}
stdio() { printf "$1" | ./geist-serve "$MODEL" --stdio 2>/dev/null | tr -d '\r'; }

# --- --stdio -----------------------------------------------------------------
out=$(stdio 'GET /health HTTP/1.1\r\nHost: x\r\n\r\n')
check "health 200"            'HTTP/1.1 200 OK'      "$out"
check "health body"           '{"status":"ok"}'      "$out"
check "health content-length" 'Content-Length: 15'   "$out"
check "cors header"           'Access-Control-Allow-Origin: \*' "$out"

out=$(stdio 'GET /health?probe=1 HTTP/1.1\r\n\r\n')
check "query string stripped" 'HTTP/1.1 200'         "$out"

out=$(stdio 'GET / HTTP/1.1\r\n\r\n')
check "root ollama probe"     'Ollama is running'    "$out"

out=$(stdio 'OPTIONS /v1/chat/completions HTTP/1.1\r\n\r\n')
check "preflight 204"         'HTTP/1.1 204'         "$out"

out=$(stdio 'GET /nope HTTP/1.1\r\n\r\n')
check "unknown 404"           'HTTP/1.1 404'         "$out"
check "404 json error"        '"type":"invalid_request_error"' "$out"

out=$(stdio 'POST /x HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n')
check "oversize body 413"     'HTTP/1.1 413'         "$out"

out=$(stdio 'POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n')
check "chunked request 411"   'HTTP/1.1 411'         "$out"

big=$(head -c 9000 /dev/zero | tr '\0' 'a')
out=$(stdio "GET /health HTTP/1.1\r\nX-Pad: $big\r\n\r\n")
check "oversize headers 431"  'HTTP/1.1 431'         "$out"

out=$(stdio 'garbage\r\n\r\n')
check "malformed 400"         'HTTP/1.1 400'         "$out"

out=$(stdio 'POST /health HTTP/1.1\r\nContent-Length: 5\r\n\r\nab')
check "truncated body 400"    'HTTP/1.1 400'         "$out"

# --- own socket + SIGTERM -----------------------------------------------------
PORT=$(( 20000 + $$ % 10000 ))
./geist-serve "$MODEL" --port "$PORT" 2>/tmp/geist-serve.$$.log &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    grep -q listening /tmp/geist-serve.$$.log 2>/dev/null && break; sleep 0.5
done
out=$(curl -s -i "http://127.0.0.1:$PORT/health" | tr -d '\r')
check "tcp health"            '{"status":"ok"}'      "$out"
kill -TERM $pid
wait $pid; rc=$?
check "sigterm exit 0"        '^0$'                  "$rc"
check "sigterm log"           'stopped'              "$(cat /tmp/geist-serve.$$.log)"
rm -f /tmp/geist-serve.$$.log

# --- inherited listener (systemd LISTEN_FDS protocol) -----------------------
if command -v python3 >/dev/null; then
out=$(python3 - "$MODEL" "$PORT" <<'PY'
import os, socket, subprocess, sys, time, urllib.request
model, port = sys.argv[1], int(sys.argv[2])
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", port)); ls.listen(4); os.set_inheritable(ls.fileno(), True)
env = dict(os.environ, LISTEN_FDS="1", LISTEN_PID="0")
# close_fds runs after preexec_fn, so fd 3 must be in pass_fds too.
p = subprocess.Popen(["./geist-serve", model], env=env, pass_fds=(ls.fileno(), 3),
                     stderr=subprocess.PIPE, text=True,
                     preexec_fn=lambda: os.dup2(ls.fileno(), 3))
for line in p.stderr:
    if "listening" in line: break
else:
    print("server exited before listening"); sys.exit(0)
body = urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=10).read().decode()
p.terminate(); p.wait(10)
print(body, "listener-fd-ok" if p.returncode == 0 else f"rc={p.returncode}")
PY
)
check "LISTEN_FDS health"     '{"status":"ok"}'      "$out"
check "LISTEN_FDS clean exit" 'listener-fd-ok'       "$out"
fi

[ $fail -eq 0 ] && echo "smoke: all passed"
exit $fail
