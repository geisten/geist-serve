#!/usr/bin/env python3
"""Every geistd op against a running daemon, plus the properties that make
it worth having: sessions survive processes, prefill pays only for the
diff, logits are reachable, bad frames do not kill it.

usage: geistd_ops.py <socket-path>"""
import json, os, socket, struct, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "clients"))
import geistd

SOCK = sys.argv[1]
fails = 0
def check(name, cond, detail=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f": {str(detail)[:300]}"))
    fails += 0 if cond else 1

c = geistd.Client(path=SOCK)
info = c.info()
check("info fields", all(k in info for k in ("model", "arch", "eos", "ctx", "vocab", "template", "add_bos", "bos")), info)
check("vocab known", info["vocab"] > 1000, info["vocab"])

prompt = "<|im_start|>user\nWhat is the capital of France? One word.<|im_end|>\n<|im_start|>assistant\n"
ids = c.tokenize(prompt)
check("tokenize", 10 < len(ids) < 40, len(ids))
s = c.open(temperature=0)
check("open gives 16-hex id", len(s.id) == 16 and all(ch in "0123456789abcdef" for ch in s.id), s.id)
r = s.prefill(ids)
check("first prefill pays everything", r["prefilled"] == len(ids) and r["reused"] == 0, r)
r = s.prefill(ids)
check("identical prefill is free", r["prefilled"] == 0 and r["reused"] == len(ids), r)
check("str roundtrip", "".join(p or "" for p in s.strs(ids)) == prompt, s.strs(ids))

paris, london = c.tokenize("Paris")[0], c.tokenize("London")[0]
pk = s.peek(ids=[paris, london], topk=5)
check("peek logprobs for candidates", pk["logprobs"][0] > pk["logprobs"][1], pk)
check("peek topk sorted", pk["top"] == sorted(pk["top"], key=lambda t: -t[1]) and len(pk["top"]) == 5, pk["top"])
check("peek logprobs ≤ 0", all(lp <= 0 for _, lp in pk["top"]), pk["top"])
full = s.peek(full=True)
check("peek full vector", len(full["full"]) == info["vocab"], len(full["full"]))
pk2 = s.peek(ids=[-1, 10**8])
check("peek out-of-range ids are null", pk2["logits"] == [None, None], pk2)

# constrained decoding: pick among candidates by logprob, feed it back
best = paris if pk["logprobs"][0] >= pk["logprobs"][1] else london
r = s.prefill(ids + [best])
check("feed chosen token = 1 prefilled", r["prefilled"] == 1 and r["reused"] == len(ids), r)

st = s.step(topk=3)
check("step returns token+piece+top", "token" in st and "piece" in st and len(st["top"]) == 3, st)
out = "".join(s.generate(max=16))
check("generate stops at eos", s.last["reason"] == "stop", s.last)

# session survives a process boundary; the next process pays only the diff
sid = s.id
ctx = ids + [best, st["token"]] + c.tokenize(out) + c.tokenize("<|im_start|>user\nWhat is the capital of Italy? One word.<|im_end|>\n<|im_start|>assistant\n")
child = subprocess.run([sys.executable, "-c", f"""
import sys; sys.path.insert(0, {json.dumps(os.path.dirname(geistd.__file__))}); import geistd, json
c = geistd.Client(path={json.dumps(SOCK)}); s = c.resume({json.dumps(sid)})
r = s.prefill({json.dumps(ctx)}); out = "".join(s.generate(max=8)); print(json.dumps([r, out]))
"""], capture_output=True, text=True)
try:
    r, out2 = json.loads(child.stdout)
    check("second process resumes the session", r["reused"] > 0 and r["prefilled"] < len(ctx), r)
    check("resumed session answers", len(out2.replace("<|im_end|>", "").strip()) > 0, out2)  # wording differs per backend
except Exception as e:
    check("second process resumes the session", False, child.stdout + child.stderr)

r = s.prefill(c.tokenize("completely different"))
check("divergent prefill resets", r["reused"] == 0, r)
s.reset()
try:
    s.step(); check("step after reset refused", False)
except geistd.GeistdError as e:
    check("step after reset refused", "prefill first" in str(e), e)

# errors never end the daemon
for name, hdr, body in [("unknown op", {"op": "nope"}, b""), ("no op", {"x": 1}, b""),
                        ("unknown session", {"op": "step", "session": "0000000000000000"}, b""),
                        ("prefill odd body", {"op": "prefill", "session": s.id}, b"\x01\x02\x03"),
                        ("prefill bad id", {"op": "prefill", "session": s.id}, struct.pack("<i", 10**8)),
                        ("prefill overflow", {"op": "prefill", "session": s.id}, b"\x01\x00\x00\x00" * 5000),
                        ("open typed", {"op": "open", "temperature": "hot"}, b"")]:
    try:
        c._call(hdr, body); check(f"error: {name}", False, "no error raised")
    except geistd.GeistdError as e:
        check(f"error: {name}", True)
raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); raw.connect(SOCK)
raw.sendall(b"\xff\xff\xff\xff\x00\x00\x00\x00"); reply = raw.recv(300); raw.close()
check("oversize frame refused + closed", b"malformed" in reply, reply)
raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); raw.connect(SOCK)
raw.sendall(struct.pack("<II", 5, 0) + b"{{{{{"); reply = raw.recv(300); raw.close()
check("bad json header refused", b"not a JSON" in reply, reply)
check("daemon alive after all that", c.info()["ok"])

# eviction: open more than the table holds; the oldest goes
live = c.info()["max_sessions"]
keep = [c.open() for _ in range(live)]
try:
    s.step(); check("LRU evicted after overflow", False, "old session still answers")
except geistd.GeistdError as e:
    check("LRU evicted after overflow", "unknown session" in str(e), e)
for k in keep: k.close()
check("close frees", c.info()["sessions"] == 0, c.info()["sessions"])

print(f"geistd_ops: {'all passed' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
