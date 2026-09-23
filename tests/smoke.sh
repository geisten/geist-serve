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

# --- install.sh against the local build (file:// base, temp prefix) ---------
D=$(mktemp -d); cp geist-serve "$D/geist-serve-$(uname -s | tr A-Z a-z | sed s/darwin/macos/)-$(uname -m | sed 's/aarch64/aarch64/;s/x86_64/x86_64/')"
(cd "$D" && (sha256sum geist-serve-* 2>/dev/null || shasum -a 256 geist-serve-*) > SHA256SUMS)
out=$(GEIST_SERVE_BASE="file://$D" GEIST_SERVE_PREFIX="$D/prefix" sh install.sh 2>&1)
check "install.sh verifies + installs" 'checksum ok'     "$out"
check "install.sh binary runs"  '^installed '            "$out"
( "$D/prefix/bin/geist-serve" >/dev/null 2>&1; echo "rc=$?" ) | grep -q 'rc=2' && echo "ok   install.sh installed binary exits 2" || { echo "FAIL install.sh installed binary"; fail=1; }
sed -i.bak 's/^\([0-9a-f]\{8\}\)/deadbeef/' "$D/SHA256SUMS"
out=$(GEIST_SERVE_BASE="file://$D" GEIST_SERVE_PREFIX="$D/prefix2" sh install.sh 2>&1 || true)
check "install.sh rejects bad checksum" 'checksum mismatch' "$out"
rm -rf "$D"

# --- own socket + SIGTERM -----------------------------------------------------
PORT=$(( 20000 + $$ % 10000 ))
./geist-serve "$MODEL" --port "$PORT" 2>/tmp/geist-serve.$$.log &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    grep -q listening /tmp/geist-serve.$$.log 2>/dev/null && break; sleep 0.5
done
out=$(curl -s -i "http://127.0.0.1:$PORT/health" | tr -d '\r')
check "tcp health"            '{"status":"ok"}'      "$out"

# --- generation through /v1/completions (issue #3) --------------------------
U="http://127.0.0.1:$PORT/v1/completions"
out=$(curl -s "$U" -d '{"prompt":"The capital of France is","max_tokens":16,"temperature":0}')
check "completion text"       'Paris'                "$out"
out2=$(curl -s "$U" -d '{"prompt":"<|im_start|>user\nWhat is the capital of France? One word.<|im_end|>\n<|im_start|>assistant\n","max_tokens":40,"temperature":0}')
check "completion stop at eos" '"finish_reason":"stop"' "$out2"
check "completion usage"      '"prompt_tokens":[1-9]' "$out"
check "completion model name" '"model":"smollm2'     "$out"
out=$(curl -s "$U" -d '{"prompt":"Count: 1, 2, 3,","max_tokens":3,"temperature":0}')
check "max_tokens honoured"   '"completion_tokens":3' "$out"
check "finish length"         '"finish_reason":"length"' "$out"
out=$(curl -s "$U" -d '{"prompt":"List three colors: red,","max_tokens":40,"temperature":0,"stop":[","]}')
check "stop string cuts"      '"text":" blue"'        "$out"
out=$(curl -sN "$U" -d '{"prompt":"Count: 1, 2,","max_tokens":4,"stream":true,"temperature":0}' | tr -d '\r')
check "sse chunk"             '^data: {"id":"cmpl-'  "$out"
check "sse done"              '^data: \[DONE\]'      "$out"
a=$(curl -s "$U" -d '{"prompt":"Once upon a time","max_tokens":8,"temperature":0.9,"seed":42}')
b=$(curl -s "$U" -d '{"prompt":"Once upon a time","max_tokens":8,"temperature":0.9,"seed":42}')
a=$(printf '%s' "$a" | sed 's/"id":"[^"]*"//;s/"created":[0-9]*//'); b=$(printf '%s' "$b" | sed 's/"id":"[^"]*"//;s/"created":[0-9]*//')
check "seed reproducible"     "^$(printf '%s' "$a" | sed 's/[][\.*^$]/\\&/g')\$" "$b"
out=$(curl -s "$U" -d '{"prompt":')
check "bad json 400"          'not a JSON object'    "$out"
out=$(curl -s "$U" -d '{"max_tokens":3}')
check "missing prompt 400"    'prompt must be a string' "$out"
out=$(python3 -c 'import json; print(json.dumps({"prompt":"word "*6000,"max_tokens":1}))' | curl -s "$U" -d @-)
check "oversize prompt 400"   'does not fit'         "$out"
curl -sN "$U" -d '{"prompt":"Write a long story:","max_tokens":300,"stream":true}' | head -c 100 >/dev/null
out=$(curl -s "http://127.0.0.1:$PORT/health")
check "survives client cancel" '{"status":"ok"}'     "$out"

# --- OpenAI chat + model list (issue #5) -------------------------------------
CH="http://127.0.0.1:$PORT/v1/chat/completions"
out=$(curl -s "http://127.0.0.1:$PORT/v1/models")
check "models lists the gguf"  '"id":"smollm2-360m-instruct-q8_0"' "$out"
out=$(curl -s "$CH" -d '{"model":"smollm2-360m-instruct-q8_0:latest","messages":[{"role":"system","content":"Answer in one word."},{"role":"user","content":"What is the capital of France?"}],"temperature":0,"max_tokens":20}')
check "chat answer"           'Paris'                "$out"
check "chat finish stop"      '"finish_reason":"stop"' "$out"
check "chat :latest accepted" '"object":"chat.completion"' "$out"
out=$(curl -sN "$CH" -d '{"messages":[{"role":"user","content":"Say hello"}],"stream":true,"temperature":0,"max_tokens":6}' | tr -d '\r')
check "chat sse role first"   '"delta":{"role":"assistant"' "$out"
check "chat sse chunk object" '"object":"chat.completion.chunk"' "$out"
check "chat sse done"         '^data: \[DONE\]'      "$out"
out=$(curl -s "$CH" -d '{"messages":[{"role":"user","content":[{"type":"text","text":"What is 2+2?"},{"type":"image_url","image_url":{"url":"x"}}]}],"temperature":0,"max_tokens":4}')
check "content parts"         '"object":"chat.completion"' "$out"
out=$(curl -s "$CH" -d '{"model":"gpt-4","messages":[{"role":"user","content":"hi"}]}')
check "wrong model 404"       'model not found'      "$out"
out=$(curl -s "$CH" -d '{"messages":[]}')
check "empty messages 400"    'non-empty array'      "$out"
out=$(curl -s "$CH" -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":"9"}')
check "chat typed max_tokens 400" 'wrong type'       "$out"
out=$(python3 -c '
import json; m=[{"role":"system","content":"Be terse."}]
for i in range(80): m += [{"role":"user","content":("filler sentence number %d. "%i)*20},{"role":"assistant","content":"ok"}]
m.append({"role":"user","content":"What is the capital of France? One word."}); print(json.dumps({"messages":m,"temperature":0,"max_tokens":8}))' | curl -s "$CH" -d @-)
check "long chat truncated, still answers" 'Paris' "$out"
check "long chat under cap"   '"prompt_tokens":\([1-3][0-9][0-9][0-9]\|40[0-8][0-9]\),' "$out"
out=$(python3 -c 'import json; print(json.dumps({"messages":[{"role":"user","content":"word "*6000}],"max_tokens":1}))' | curl -s "$CH" -d @-)
check "oversize message 400"  'does not fit'         "$out"

# --- Ollama API (issue #6) ---------------------------------------------------
A="http://127.0.0.1:$PORT/api"
out=$(curl -s -I "http://127.0.0.1:$PORT/" | tr -d '\r')
check "HEAD / heartbeat"       'HTTP/1.1 200'         "$out"
out=$(curl -s "$A/tags")
check "tags name"              '"name":"smollm2-360m-instruct-q8_0:latest"' "$out"
check "tags quantization"      '"quantization_level":"Q8_0"' "$out"
check "tags parameter size"    '"parameter_size":"360M"' "$out"
out=$(curl -s "$A/version")
check "version"                '"version":"'          "$out"
out=$(curl -s "$A/ps")
check "ps lists model"         '"model":"smollm2'     "$out"
out=$(curl -s "$A/show" -d '{"name":"smollm2-360m-instruct-q8_0"}')
check "show template"          '<|im_start|>'         "$out"
check "show capabilities"      '"capabilities":\["completion"\]' "$out"
check "show context"           '"llama.context_length":8192' "$out"
out=$(curl -s "$A/generate" -d '{"model":"smollm2-360m-instruct-q8_0","prompt":"What is the capital of France? One word.","stream":false,"options":{"temperature":0,"num_predict":8}}')
check "generate answer"        'Paris'                "$out"
check "generate done stop"     '"done_reason":"stop"' "$out"
check "generate stats"         '"eval_count":[1-9]'   "$out"
out=$(curl -sN "$A/generate" -d '{"prompt":"Say hi","options":{"temperature":0,"num_predict":3}}')
check "generate ndjson chunk"  '"done":false}'        "$out"
check "generate ndjson final"  '"done":true'          "$out"
out=$(curl -s "$A/generate" -d '{"model":"smollm2-360m-instruct-q8_0"}')
check "generate empty = load"  '"done_reason":"load"' "$out"
out=$(curl -s "$A/generate" -d '{"prompt":"The capital of France is","raw":true,"stream":false,"options":{"temperature":0,"num_predict":4}}')
check "generate raw"           'Paris'                "$out"
out=$(curl -s "$A/chat" -d '{"model":"smollm2-360m-instruct-q8_0:latest","messages":[{"role":"user","content":"What is the capital of France? One word."}],"stream":false,"options":{"temperature":0}}')
check "chat answer"            'Paris'                "$out"
out=$(curl -sN "$A/chat" -d '{"messages":[{"role":"user","content":"Say hi"}],"options":{"temperature":0,"num_predict":3}}')
check "chat ndjson chunk"      '"message":{"role":"assistant","content":"[^"]*"},"done":false}' "$out"
check "chat ndjson final"      '"done":true,"done_reason"' "$out"
out=$(curl -s "$A/chat" -d '{"messages":[]}')
check "chat empty = load"      '"done_reason":"load"' "$out"
out=$(curl -s "$A/pull" -d '{"name":"llama3"}')
check "pull 404 flat error"    '^{"error":"no registry' "$out"
out=$(curl -s "$A/generate" -d '{"prompt":"hi","options":{"num_predict":"5"}}')
check "options typed 400"      'wrong type'           "$out"
out=$(curl -s "$A/chat" -d '{"model":"nope","messages":[{"role":"user","content":"hi"}]}')
check "chat wrong model 404"   'model not found'      "$out"
if command -v ollama >/dev/null; then
    out=$(OLLAMA_HOST=127.0.0.1:$PORT ollama run smollm2-360m-instruct-q8_0 "What is the capital of France? Answer in one word." 2>&1 | tr -d '\033' )
    check "real ollama CLI run"  'Paris'               "$out"
    out=$(OLLAMA_HOST=127.0.0.1:$PORT ollama list 2>&1)
    check "real ollama CLI list" 'smollm2-360m-instruct-q8_0:latest' "$out"
fi

# --- wire-level critical path + hostile input (tests/test_http.py) ----------
if command -v python3 >/dev/null; then
    python3 -u tests/test_http.py "$PORT" > /tmp/geist-serve-http.$$.log 2>&1 || fail=1
    grep -v '^ok' /tmp/geist-serve-http.$$.log; rm -f /tmp/geist-serve-http.$$.log
fi
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
