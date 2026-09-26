#!/usr/bin/env python3
"""Adversarial Unix-socket peers; checks actual C client allocation and deadlines."""
import json, os, socket, struct, subprocess, tempfile, threading, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def frame(obj):
    data = json.dumps(obj, ensure_ascii=True).encode()
    return struct.pack('<II', len(data), 0) + data

def run(reply, *, mode='info', success=False, expected=None, pause=0):
    with tempfile.TemporaryDirectory(dir='/tmp') as home:
        path = home + '/sock'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(path); server.listen()
            def serve():
                conn, _ = server.accept()
                with conn:
                    try:
                        conn.recv(65536)
                        if reply: conn.sendall(reply)
                        if pause: time.sleep(pause)
                    except (BrokenPipeError, ConnectionResetError): pass
            thread = threading.Thread(target=serve, daemon=True); thread.start()
            start = time.monotonic()
            proc = subprocess.run([str(ROOT/'build/test_app_client'), path, mode], capture_output=True, timeout=3)
            assert time.monotonic() - start < 1, 'deadline did not bound I/O'
            assert proc.returncode == (0 if success else 1), (proc.returncode, proc.stdout, proc.stderr)
            assert b'AddressSanitizer' not in proc.stderr and b'runtime error:' not in proc.stderr, proc.stderr
            if expected: assert expected in proc.stdout.decode(), proc.stdout
            thread.join(timeout=1)

run(frame({'ok': True}), success=True)
run(frame(['wrong shape']))
run(struct.pack('<II', 65537, 0))
run(struct.pack('<II', 50, 0) + b'{')
run(frame({'ok':False, 'error':'test refusal'}))
run(b'', pause=.3)
run(b'', mode='cancel', pause=.3)
piece = 'Grüße 🌿\n' + 'x' * 600
run(frame({'ok':True,'piece':piece,'done':False}) + frame({'ok':True,'done':False,'stop':True,'piece':'<|im_end|>'}) + frame({'ok':True,'done':True,'reason':'stop','generated':2,'duration_ns':1500000}), mode='generate', success=True, expected=piece + '\n2 1500000 stop')
print('geistd C client: framing, Unicode/long pieces, refusals, disconnect, deadline and cancellation passed')
