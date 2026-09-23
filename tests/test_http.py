#!/usr/bin/env python3
"""Critical-path and robustness checks against a running geist-serve.

usage: test_http.py PORT — prints ok/FAIL per check, exit 1 on any FAIL.
Raw sockets on purpose: the point is what goes over the wire (framing,
Content-Length exactness, chunk boundaries, UTF-8), not what a client
library tolerates. Every check ends with the server still answering."""
import json, os, random, socket, sys, time

PORT = int(sys.argv[1])
fails = 0

def check(name, cond, detail=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f": {detail[:300]}"))
    fails += 0 if cond else 1

def raw(data: bytes, timeout=60, hold=None) -> bytes:
    """Send bytes (optionally in two parts with a pause), read until EOF."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    if hold:
        s.sendall(data[:hold[0]]); time.sleep(hold[1]); s.sendall(data[hold[0]:])
    else:
        s.sendall(data)
    out = b""
    try:
        while True:
            b = s.recv(65536)
            if not b: break
            out += b
    except socket.timeout:
        out += b"<<TIMEOUT>>"
    except OSError as e:
        out += f"<<{e}>>".encode()
    s.close()
    return out

def request(method, path, body: bytes = b"", extra=b"") -> bytes:
    hdr = f"{method} {path} HTTP/1.1\r\nHost: t\r\n".encode()
    if body: hdr += f"Content-Length: {len(body)}\r\n".encode()
    return raw(hdr + extra + b"\r\n" + body)

def split(resp: bytes):
    head, _, body = resp.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    headers = {}
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        headers[k.strip().lower().decode()] = v.strip().decode()
    return status, headers, body

def post(path, obj):
    return split(request("POST", path, json.dumps(obj).encode()))

def dechunk(body: bytes) -> bytes:
    """Strict chunked-transfer decoder: raises on any framing error."""
    out, i = b"", 0
    while True:
        j = body.index(b"\r\n", i)
        n = int(body[i:j], 16)
        i = j + 2
        if n == 0:
            assert body[i:i+2] == b"\r\n", "missing final CRLF"
            assert i + 2 == len(body), "trailing bytes after last chunk"
            return out
        out += body[i:i+n]
        assert body[i+n:i+n+2] == b"\r\n", "chunk not CRLF-terminated"
        i += n + 2

def alive():
    st, h, b = split(request("GET", "/health"))
    return st == 200 and b == b'{"status":"ok"}'

C = "/v1/completions"

# --- critical path: framing exactness ---------------------------------------
for path, method, body in [("/health", "GET", b""), ("/nope", "GET", b""),
                           (C, "POST", json.dumps({"prompt": "Hi", "max_tokens": 2}).encode())]:
    st, h, b = split(request(method, path, body))
    check(f"content-length exact {path}", int(h.get("content-length", -1)) == len(b), f"{h} {len(b)}")
    if st == 200 and path != "/health":
        check(f"json body {path}", json.loads(b) is not None)

st, h, b = post(C, {"prompt": "Emojis and umlauts: 😀 ü — ", "max_tokens": 20, "temperature": 0})
check("non-stream utf-8 strict", st == 200 and json.loads(b.decode("utf-8", "strict"))["choices"][0]["text"] is not None, b.decode("utf-8","replace"))

resp = request("POST", C, json.dumps({"prompt": "Emojis: 😀 😀 😀", "max_tokens": 24, "stream": True, "temperature": 0}).encode())
st, h, body = split(resp)
check("sse status 200 + chunked", st == 200 and h.get("transfer-encoding") == "chunked", str(h))
try:
    payload = dechunk(body)
    check("sse chunk framing strict", True)
except Exception as e:
    payload = b""; check("sse chunk framing strict", False, str(e))
events = [e for e in payload.split(b"\n\n") if e.strip()]
datas = [e[6:] for e in events if e.startswith(b"data: ")]
check("sse every event is data:", len(datas) == len(events) and len(datas) >= 2, str(events[:3]))
check("sse ends with [DONE]", datas[-1] == b"[DONE]", str(datas[-1]))
ok_json = True; text = ""
for d in datas[:-1]:
    try:
        o = json.loads(d.decode("utf-8", "strict")); text += o["choices"][0]["text"]
    except Exception as e:
        ok_json = False; break
check("sse every chunk strict utf-8 json", ok_json, str(datas[:3]))
check("sse usage on final", b'"usage"' in datas[-2])
tot = json.loads(datas[-2])["usage"]
check("usage sums", tot["prompt_tokens"] + tot["completion_tokens"] == tot["total_tokens"])

# Chat SSE: every chunk strict JSON, role in the first delta, content
# concatenates to valid text, usage on the final chunk sums.
resp = request("POST", "/v1/chat/completions", json.dumps({"messages": [{"role": "user", "content": "Write one sentence with emojis 😀"}], "max_tokens": 24, "stream": True, "temperature": 0}).encode())
st, h, body = split(resp)
try:
    cdatas = [e[6:] for e in dechunk(body).split(b"\n\n") if e.startswith(b"data: ")]
    objs = [json.loads(d.decode("utf-8", "strict")) for d in cdatas[:-1]]
    first_role = objs[0]["choices"][0]["delta"].get("role") == "assistant"
    content = "".join(o["choices"][0]["delta"].get("content", "") for o in objs)
    fin = objs[-1]["choices"][0]["finish_reason"] in ("stop", "length")
    u = objs[-1]["usage"]
    check("chat sse strict", st == 200 and first_role and fin and cdatas[-1] == b"[DONE]" and len(content) > 0
          and u["prompt_tokens"] + u["completion_tokens"] == u["total_tokens"], str(cdatas[:2]))
except Exception as e:
    check("chat sse strict", False, f"{e} {body[:200]}")

# Ollama NDJSON: every line strict JSON, content concatenates, final line
# carries the stats, chunk framing intact.
resp = request("POST", "/api/chat", json.dumps({"messages": [{"role": "user", "content": "One sentence with emojis 😀"}], "options": {"num_predict": 24, "temperature": 0}}).encode())
st, h, body = split(resp)
try:
    lines = [l for l in dechunk(body).split(b"\n") if l]
    objs = [json.loads(l.decode("utf-8", "strict")) for l in lines]
    content = "".join(o["message"]["content"] for o in objs)
    ok = st == 200 and h.get("content-type") == "application/x-ndjson" and all(not o["done"] for o in objs[:-1]) \
        and objs[-1]["done"] and "eval_count" in objs[-1] and objs[-1]["eval_count"] >= len(objs) - 1 and len(content) > 0
    check("ollama ndjson strict", ok, str(lines[:2]))
except Exception as e:
    check("ollama ndjson strict", False, f"{e} {body[:200]}")
resp = request("POST", "/api/generate", json.dumps({"prompt": "Say OK", "stream": False, "options": {"num_predict": 4, "temperature": 0}}).encode())
st, h, b = split(resp)
check("ollama non-stream exact length", st == 200 and int(h["content-length"]) == len(b) and json.loads(b)["done"], str(h))
for name, body in [("options is array", b'{"prompt":"hi","options":[1]}'),
                   ("messages is string", b'{"messages":"hi"}'),
                   ("generate trailing garbage", b'{"prompt":"hi"}x')]:
    resp = request("POST", "/api/chat" if b"messages" in body else "/api/generate", body)
    check(f"ollama: {name} → 400", resp.startswith(b"HTTP/1.1 400") and alive(), resp[:100].decode("latin1"))

# Hostile messages: wrong shapes must be 400, never a crash or a run.
for name, body in [("messages not array", b'{"messages":{"role":"user"}}'),
                   ("message not object", b'{"messages":["hi"]}'),
                   ("message without role", b'{"messages":[{"content":"hi"}]}'),
                   ("content is object", b'{"messages":[{"role":"user","content":{"a":1}}]}'),
                   ("300 messages", json.dumps({"messages": [{"role": "user", "content": "x"}] * 300, "max_tokens": 1}).encode())]:
    resp = request("POST", "/v1/chat/completions", body)
    check(f"chat: {name} → 400", resp.startswith(b"HTTP/1.1 400") and alive(), resp[:100].decode("latin1"))

# Header-injection attempt through the prompt: CRLF must come back escaped.
st, h, b = post(C, {"prompt": "x\r\nContent-Length: 0\r\n\r\n", "max_tokens": 1})
check("crlf in prompt stays inside json", st == 200 and b.count(b"\r\n") == 0 and json.loads(b) is not None, b[:200].decode("utf-8","replace"))

# Expect: 100-continue (curl's behaviour for bodies > 1 KiB).
body = json.dumps({"prompt": "Hello " * 300, "max_tokens": 1}).encode()
s = socket.create_connection(("127.0.0.1", PORT), timeout=60)
s.sendall(f"POST {C} HTTP/1.1\r\nHost: t\r\nContent-Length: {len(body)}\r\nExpect: 100-continue\r\n\r\n".encode())
first = s.recv(64)
check("100-continue interim response", first.startswith(b"HTTP/1.1 100"), first.decode("latin1"))
s.sendall(body); out = b""
try:
    while True:
        b = s.recv(65536)
        if not b: break
        out += b
except OSError as e:
    out += f"<<{e}>>".encode()
s.close()
check("100-continue final 200", b"HTTP/1.1 200" in out, out[:80].decode("latin1"))

# Slow client: header arrives in two pieces 1.5 s apart — must still be served.
st, h, b = split(raw(b"GET /health HTTP/1.1\r\nHost: t\r\n\r\n", hold=(10, 1.5)))
check("slow header drip served", st == 200)

# Connection: close is honoured — a pipelined second request is never answered.
resp = raw(b"GET /health HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\n")
check("pipelined second request dropped", resp.count(b"HTTP/1.1 200") == 1)

# A non-stream client that hangs up must not keep the model busy: the server
# has to notice the EOF and be back within a few seconds, not after 4096 tokens.
s = socket.create_connection(("127.0.0.1", PORT), timeout=60)
body = json.dumps({"prompt": "Write a very long story:", "max_tokens": 4000, "temperature": 0.9}).encode()
s.sendall(f"POST {C} HTTP/1.1\r\nHost: t\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
time.sleep(0.5); s.close()
t0 = time.time(); ok = alive(); dt = time.time() - t0
check("vanished non-stream client frees the server", ok and dt < 10, f"{dt:.1f}s")

# --- robustness: malformed and hostile input --------------------------------
cases = [
    ("negative content-length", b"POST " + C.encode() + b" HTTP/1.1\r\nContent-Length: -5\r\n\r\n"),
    ("non-numeric content-length", b"POST " + C.encode() + b" HTTP/1.1\r\nContent-Length: abc\r\n\r\n{}"),
    ("content-length beyond body then close", b"POST " + C.encode() + b" HTTP/1.1\r\nContent-Length: 50\r\n\r\n{}"),
    ("10 KB request line", b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\n\r\n"),
    ("400-byte request line", b"GET /" + b"a" * 400 + b" HTTP/1.1\r\n\r\n"),
    ("header without colon", b"GET /health HTTP/1.1\r\nnocolon\r\n\r\n"),
    ("LF-only line endings", b"GET /health HTTP/1.1\nHost: t\n\n"),
    ("binary garbage", bytes(random.Random(1).getrandbits(8) for _ in range(3000))),
    ("empty request", b""),
    ("method too long", b"ABCDEFGHIJKLMNOP /health HTTP/1.1\r\n\r\n"),
    ("null bytes", b"GET /hea\x00lth HTTP/1.1\r\n\r\n"),
]
for name, data in cases:
    resp = raw(data, timeout=40)
    st = resp.split(b" ")[1] if resp.startswith(b"HTTP/1.1 ") else b"none"
    # A junk header line is ignored (200); everything else must be refused.
    ok = resp == b"" or resp.startswith(b"HTTP/1.1 4") or (name == "header without colon" and resp.startswith(b"HTTP/1.1 200"))
    check(f"hostile: {name} → {st.decode()}", ok and alive(), resp[:100].decode("latin1"))

json_cases = [
    ("deeply nested arrays", b'{"prompt":' + b"[" * 6000 + b"]" * 6000 + b"}"),
    ("prompt is array", b'{"prompt":["a","b"]}'),
    ("prompt is number", b'{"prompt":5}'),
    ("prompt null", b'{"prompt":null}'),
    ("top-level array", b'["prompt"]'),
    ("trailing garbage", b'{"prompt":"hi"}xxx'),
    ("unterminated string", b'{"prompt":"hi'),
    ("bad escape", b'{"prompt":"\\x41"}'),
    ("short unicode escape", b'{"prompt":"\\u12"}'),
    ("lone surrogate", b'{"prompt":"\\ud800","max_tokens":1}'),
    ("nul escape", b'{"prompt":"a\\u0000b","max_tokens":1}'),
    ("invalid utf-8 bytes", b'{"prompt":"\xff\xfe\xc0","max_tokens":1}'),
    # max_tokens stays 1: a huge value there is a legitimate 4096-token run.
    ("huge numbers", b'{"prompt":"hi","max_tokens":1,"temperature":1e308,"top_p":-1e308,"top_k":1e20,"seed":-1e300}'),
    ("nan/inf", b'{"prompt":"hi","max_tokens":1,"temperature":-nan,"top_p":inf,"seed":-inf}'),
    ("strings for numbers", b'{"prompt":"hi","max_tokens":"3","temperature":"hot"}'),
    ("bool for number", b'{"prompt":"hi","max_tokens":true}'),
    ("object for stop", b'{"prompt":"hi","max_tokens":1,"stop":{"a":1}}'),
    ("null for stop", b'{"prompt":"hi","max_tokens":1,"stop":null}'),
    ("200-char stop + 20 stops", json.dumps({"prompt": "hi", "max_tokens": 2, "stop": ["x" * 200] + [str(i) for i in range(20)]}).encode()),
    ("empty stop strings", b'{"prompt":"hi","max_tokens":1,"stop":["",""]}'),
    # <= 0 means "unset" = fill the context, so these use a prompt that ends
    # on EOS after a few tokens.
    ("max_tokens 0", b'{"prompt":"<|im_start|>user\\nSay OK<|im_end|>\\n<|im_start|>assistant\\n","max_tokens":0,"temperature":0}'),
    ("max_tokens negative", b'{"prompt":"<|im_start|>user\\nSay OK<|im_end|>\\n<|im_start|>assistant\\n","max_tokens":-7,"temperature":0}'),
    ("empty prompt", b'{"prompt":"","max_tokens":2}'),
    ("whitespace-only body", b'   '),
    ("1 MiB body exactly", b'{"prompt":"' + b"a" * (1024*1024 - 40) + b'","max_tokens":1}'),
]
# The 1 MiB single-word prompt must be refused on byte count, before the
# tokenizer sees it: seconds, not minutes.
t0 = time.time(); resp = request("POST", C, json_cases[-1][1]); dt = time.time() - t0
check("1 MiB prompt refused fast", resp.startswith(b"HTTP/1.1 400") and dt < 5, f"{dt:.1f}s {resp[:60]}")
must_400 = {"strings for numbers", "bool for number", "object for stop", "prompt is array", "prompt is number", "prompt null", "empty prompt"}
for name, data in json_cases:
    resp = request("POST", C, data)
    st = resp.split(b" ")[1] if resp.startswith(b"HTTP/1.1 ") else b"none"
    ok = resp.startswith(b"HTTP/1.1 400") if name in must_400 else (resp.startswith(b"HTTP/1.1 2") or resp.startswith(b"HTTP/1.1 4"))
    check(f"json: {name} → {st.decode()}", ok and alive(), resp[:120].decode("latin1"))

# Fuzz: 30 random mutations of a valid request; the server must answer every
# one. The max_tokens prefix is kept intact so a mutation cannot turn a case
# into a 4096-token generation (a duplicate key later cannot appear by chance).
rnd = random.Random(7)
prefix = b'{"max_tokens":2,'
tail = json.dumps({"prompt": "Hi", "temperature": 0.5, "stop": ["\n"], "stream": False}).encode()[1:]
survived = 0
for i in range(30):
    b = bytearray(tail)
    for _ in range(rnd.randint(1, 6)):
        k = rnd.randrange(len(b))
        op = rnd.random()
        if op < 0.4: b[k] = rnd.randrange(256)
        elif op < 0.7: del b[k]
        else: b.insert(k, rnd.randrange(256))
    resp = request("POST", C, prefix + bytes(b))
    survived += resp.startswith(b"HTTP/1.1 ")
check("fuzz: 30 mutated bodies all answered", survived == 30 and alive(), f"{survived}/30")

print(f"test_http: {'all passed' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
