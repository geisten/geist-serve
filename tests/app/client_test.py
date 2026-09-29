#!/usr/bin/env python3
"""Adversarial Unix-socket peers; checks actual C client allocation and deadlines."""
import json, os, re, socket, struct, subprocess, tempfile, threading, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def frame(obj):
    data = json.dumps(obj, ensure_ascii=True).encode()
    return struct.pack('<II', len(data), 0) + data

def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        # Force fragmented reads: a stream socket does not preserve send boundaries.
        piece = conn.recv(min(3, size - len(data)))
        if not piece:
            raise ConnectionResetError('request closed before the frame was complete')
        data.extend(piece)
    return bytes(data)

def run(reply, *, mode='info', success=False, expected=None, pause=0):
    with tempfile.TemporaryDirectory(dir='/tmp') as home:
        path = home + '/sock'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(path); server.listen()
            def serve():
                conn, _ = server.accept()
                with conn:
                    try:
                        # Drain the whole request before replying/closing. Closing with
                        # unread bytes can reset a Unix socket on Linux and discard the
                        # reply, turning a valid-client test into a peer-induced failure.
                        header_size, body_size = struct.unpack('<II', recv_exact(conn, 8))
                        assert 0 < header_size <= 65536 and body_size <= 16 << 20
                        request = json.loads(recv_exact(conn, header_size))
                        assert request['op'] == ('generate' if mode.startswith('generate') else 'info')
                        if body_size: recv_exact(conn, body_size)
                        if isinstance(reply,list):
                            for part in reply:
                                conn.sendall(part);time.sleep(.08)
                        elif reply: conn.sendall(reply)
                        if pause: time.sleep(pause)
                    except (BrokenPipeError, ConnectionResetError): pass
            thread = threading.Thread(target=serve, daemon=True); thread.start()
            proc = subprocess.run([str(ROOT/'build/test_app_client'), path, mode], capture_output=True, timeout=3)
            assert proc.returncode == (0 if success else 1), (proc.returncode, proc.stdout, proc.stderr)
            # Measure client I/O, not loader/sanitizer startup. The subprocess
            # still has its independent three-second outer bound.
            timing = re.search(rb'^operation_ms=([0-9.]+)$', proc.stderr, re.MULTILINE)
            assert timing and float(timing.group(1)) < 1000, ('deadline did not bound I/O', proc.stderr)
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
run([frame({'ok':True,'piece':'x','done':False})]*5+[frame({'ok':True,'done':True,'reason':'stop','generated':5,'duration_ns':400000000})], mode='generate', success=True, expected='xxxxx')
run([frame({'ok':True,'piece':'x','done':False})]*5, mode='generate-total')
print('geistd C client: framing, Unicode/long pieces, refusals, disconnect, deadline and cancellation passed')
