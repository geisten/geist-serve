#!/usr/bin/env python3
"""The number that justifies geistd: an agent as N separate processes, each
resuming the session and adding a turn, versus the same turns through the
stateless chat API (geist-serve), which re-prefills everything every time.

usage: agent_short_calls.py <geistd-socket> <geist-serve-port> [turns]"""
import json, os, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "clients"))
import geistd

SOCK, PORT = sys.argv[1], int(sys.argv[2])
TURNS = int(sys.argv[3]) if len(sys.argv) > 3 else 6
SYSTEM = ("You are a terse assistant. " + "Rules: answer in one word. " * 60).strip()  # ~700 tokens of "system prompt"
QUESTIONS = ["capital of France", "capital of Italy", "capital of Spain", "capital of Japan", "capital of Peru", "capital of Egypt",
             "capital of Kenya", "capital of Chile"][:TURNS]

def chatml(turns):
    s = f"<|im_start|>system\n{SYSTEM}<|im_end|>\n"
    for q, a in turns:
        s += f"<|im_start|>user\n{q}?<|im_end|>\n<|im_start|>assistant\n"
        if a is not None:
            s += f"{a}<|im_end|>\n"
    return s

# --- geistd: N processes, one resident session -------------------------------
c = geistd.Client(path=SOCK)
sid = c.open(temperature=0).id
turns, t_geistd, reused_total, prefilled_total = [], 0.0, 0, 0
for q in QUESTIONS:
    turns.append((q, None))
    ctx = c.tokenize(chatml(turns))
    t0 = time.time()
    out = subprocess.run([sys.executable, "-c", f"""
import sys, json; sys.path.insert(0, {json.dumps(os.path.dirname(geistd.__file__))}); import geistd
c = geistd.Client(path={json.dumps(SOCK)}); s = c.resume({json.dumps(sid)})
r = s.prefill({json.dumps(ctx)}); a = "".join(s.generate(max=6, stop_strings=["<|im_end|>"])).replace("<|im_end|>", "").strip()
print(json.dumps([r, a]))"""], capture_output=True, text=True)
    t_geistd += time.time() - t0
    r, a = json.loads(out.stdout)
    reused_total += r["reused"]; prefilled_total += r["prefilled"]
    turns[-1] = (q, a)
answers_geistd = [a for _, a in turns]

# --- geist-serve: the same turns, stateless ----------------------------------
turns, t_http = [], 0.0
for q in QUESTIONS:
    turns.append((q, None))
    msgs = [{"role": "system", "content": SYSTEM}]
    for qq, aa in turns:
        msgs.append({"role": "user", "content": qq + "?"})
        if aa is not None:
            msgs.append({"role": "assistant", "content": aa})
    body = json.dumps({"messages": msgs, "max_tokens": 6, "temperature": 0}).encode()
    t0 = time.time()
    resp = json.load(urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/chat/completions", body, {"Content-Type": "application/json"})))
    t_http += time.time() - t0
    turns[-1] = (q, resp["choices"][0]["message"]["content"].strip())
answers_http = [a for _, a in turns]

print(f"turns: {TURNS}, system prompt ≈ {len(c.tokenize(SYSTEM))} tokens, load average {os.getloadavg()[0]:.1f} (numbers only mean something under ~2)")
print(f"geistd  (N processes, resident session): {t_geistd:6.2f} s  prefilled {prefilled_total} tokens, reused {reused_total}")
print(f"chat API (stateless, re-prefill each turn): {t_http:6.2f} s")
print(f"speedup: {t_http / t_geistd:.1f}x   answers geistd={answers_geistd} http={answers_http}")
sys.exit(0 if reused_total > 0 and t_geistd < t_http else 1)
