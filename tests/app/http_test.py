#!/usr/bin/env python3
"""Exercise the real C23 HTTP boundary. No model or internet is required."""
import hashlib
import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]


class App:
    def __init__(self, home, model=None, binary=None, env=None, server=None, port=0):
        args = [str(binary or ROOT / "geist-app"), "--port", str(port), "--home", str(home),
                "--daemon", str(server or (os.environ.get("GEIST_EXECUTION_DAEMON", ROOT / "geistd") if model else "/usr/bin/false"))]
        if model:
            args += ["--model", str(model)]
        self.home = Path(home)
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, env=env)
        if not select.select([self.process.stdout], [], [], 10)[0]:
            self.close()
            raise AssertionError("app did not print its URL")
        line = self.process.stdout.readline().strip()
        if not line.startswith("GEIST_APP_URL="):
            self.log.seek(0)
            raise AssertionError(self.log.read().decode())
        self.url = line.split("=", 1)[1]
        parsed = urlsplit(self.url)
        self.port, self.token = parsed.port, parsed.fragment

    def request(self, path, data=None, auth=True, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        fields = {"Authorization": "Bearer " + self.token} if auth else {}
        fields.update(headers or {})
        payload = json.dumps(data) if data is not None else None
        connection.request("POST" if data is not None else "GET", path, payload, fields)
        response = connection.getresponse()
        body = response.read()
        status, response_headers = response.status, dict(response.getheaders())
        connection.close()
        return status, body, response_headers

    def status(self):
        code, body, _ = self.request("/app/status")
        assert code == 200, body
        return json.loads(body)

    def wait(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.status()
            if predicate(state):
                return state
            time.sleep(.1)
        log = self.home/'server.log'
        diagnostic = log.read_text(errors='replace')[-8192:] if log.exists() else 'No daemon log'
        raise AssertionError((state, diagnostic))

    def raw(self, request):
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall(request)
            return sock.recv(4096)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("app did not cleanly shut down")
        self.log.seek(0)
        log = self.log.read().decode(errors="replace")
        self.log.close()
        assert "ERROR: AddressSanitizer" not in log and "runtime error:" not in log, log
        assert self.process.returncode == 0, log


def main():
    binary = Path(os.environ.get("GEIST_APP_TEST_BINARY", ROOT / "geist-app"))
    with tempfile.TemporaryDirectory(prefix="geist-http-") as home:
        app = App(home, binary=binary)
        try:
            state = app.status()
            assert len(state["models"]) == len(json.loads((ROOT/"models/catalog.json").read_text())["models"]) and state["hardware"]["ram"] > 0
            assert state['runtime'] == 'geistd'
            assert all(m['capabilities'] == {'chat':True, 'vision':False, 'speech_recognition':False} for m in state['models'])
            assert state['hardware']['os'] and state['hardware']['logical_cpus'] > 0
            assert state['resources'] == {'scope':'geistd','rss_bytes':None,'cpu_percent':None,'cpu_interval_ms':0}
            assert all("reason" in m and "performance" in m and m['quality'] == 'unverified' and m['fit'] != 0 for m in state["models"])
            assert app.request('/app/tasks', auth=False)[0] == 403
            catalog = json.loads(app.request('/app/tasks')[1])
            assert len(catalog['tasks']) == 5 and all(t['version'] == '1.0.0' for t in catalog['tasks'])
            for task, version in [('missing','1.0.0'),('summary','0.0.1'),('home-assistant','1.0.0')]:
                assert app.request('/app/generate', {'task':task,'task_version':version,'prompt':'hello','experimental':True})[0] == 400
            assert app.request('/app/generate', {'task':'summary','task_version':'1.0.0','prompt':'x'*6001})[0] == 400
            for language in [None, True, 42, [], {}, 'fr', '']:
                assert app.request('/app/generate', {'prompt':'hello','language':language})[0] == 400
            assert app.request("/app/status", auth=False)[0] == 403
            assert app.request("/app/status", headers={"Authorization": "Bearer wrong"})[0] == 403
            assert app.request("/app/status", headers={"Origin": "https://untrusted.example"})[0] == 403
            assert app.request("/app/status", headers={"Host": "untrusted.example"})[0] == 403
            assert app.request("/app/status", headers={"Origin": f"http://localhost:{app.port}",
                                                        "Host": f"localhost:{app.port}"})[0] == 200
            code, html, headers = app.request("/", auth=False)
            assert code == 200 and b'class="brand"' in html and b'id="catalog-preview"' not in html and b'id="setup-start"' not in html
            assert b'id="chat-speed"' not in html and b'id="chat-memory"' not in html
            assert html.index(b'id="runtime-model"') < html.index(b'id="open-measurements"') < html.index(b'id="transcript"')
            assert b'<dialog id="performance"' in html and b'<details id="performance"' not in html
            assert html == (ROOT/'web/index.html').read_bytes(), 'Served assets must match the built candidate source'
            assert "frame-ancestors 'none'" in headers["Content-Security-Policy"]
            assert "Access-Control-Allow-Origin" not in headers
            assert app.token.encode() not in html
            for manifest_name in ('manifest.json', 'katex-manifest.json'):
                manifest = json.loads((ROOT/'web/vendor'/manifest_name).read_text())
                for name, sha in manifest['files'].items():
                    assert hashlib.sha256((ROOT/'web/vendor'/name).read_bytes()).hexdigest() == sha
            for route, source in [('/marked.js', 'vendor/marked.umd.js'), ('/katex.js', 'vendor/katex.min.js'), ('/markdown.js', 'markdown.js')]:
                code, body, asset_headers = app.request(route, auth=False)
                assert code == 200 and body == (ROOT/'web'/source).read_bytes()
                assert asset_headers['Content-Type'].startswith('text/javascript')
                assert "script-src 'self'" in asset_headers['Content-Security-Policy']
            assert app.request("/app/select", {"id": "../../bad"})[0] == 400
            assert app.request("/app/select", {"id": "bitnet-2b"})[0] == 409
            assert app.request("/app/generate", {"experimental": True, "prompt": "hello"})[0] == 409
            assert app.request("/app/generate", {"experimental": True, "prompt": "x" * 12001})[0] == 400
            assert b" 413 " in app.raw(b"POST /app/select HTTP/1.1\r\nHost: localhost:8766\r\nContent-Length: 32769\r\n\r\n")
            assert b" 400 " in app.raw(b"POST /app/select HTTP/1.1\r\nHost: localhost:8766\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n")
            assert b" 400 " in app.raw(b"POST /app/select HTTP/1.1\r\nHost: localhost:8766\r\nTransfer-Encoding: chunked\r\n\r\n")
            for _ in range(25):
                assert app.request("/app/select", {"id": None})[0] == 400
            assert app.status()["ready"] is False
            assert app.request("/app/quit", {}, auth=False)[0] == 403
            assert app.request("/app/quit", {})[0] == 202
            app.process.wait(timeout=10)
            restarted = App(home, binary=binary, port=app.port)
            try:
                assert restarted.status()["ready"] is False
            finally:
                restarted.close()
            print("app HTTP: authentication, origin/host, limits, error recovery and UI assets passed")
        finally:
            app.close()

    model = os.environ.get("GEIST_TEST_MODEL")
    if not model:
        print("app inference: skipped (set GEIST_TEST_MODEL to an existing GGUF)")
        return
    with tempfile.TemporaryDirectory(prefix="geist-model-") as home:
        app = App(home, model=Path(model), binary=binary)
        try:
            loaded = app.wait(lambda state: state["ready"], timeout=60)
            assert loaded['resources']['rss_bytes'] > 0
            sampled = app.wait(lambda state: state['resources']['cpu_percent'] is not None)
            assert 0 <= sampled['resources']['cpu_percent'] <= 100
            assert 500 <= sampled['resources']['cpu_interval_ms'] <= 10000
            assert app.request("/app/generate", {"prompt":"Hello","experimental":False})[0] == 409
            child = int(subprocess.check_output(['pgrep','-P',str(app.process.pid)],text=True).strip())
            import shlex, stat
            args = shlex.split(subprocess.check_output(['ps','-p',str(child),'-o','args='],text=True))
            private = Path(args[args.index('--socket')+1])
            assert stat.S_IMODE(private.stat().st_mode) == 0o600
            assert stat.S_IMODE(private.parent.stat().st_mode) == 0o700
            code, body, _ = app.request("/app/generate", {"experimental": True, "prompt": "Name three colors."})
            events = [json.loads(line) for line in body.splitlines() if line]
            assert code == 200 and events[-1]["done"] and events[-1]["eval_count"] > 0, body
            assert any(event.get("response") for event in events), body
            assert b"<|im_end|>" not in body, body
            # Client abort closes the proxied connection, then the backend becomes usable.
            conn = http.client.HTTPConnection("127.0.0.1", app.port, timeout=30)
            conn.request("POST", "/app/generate", json.dumps({"experimental": True, "prompt": "List one hundred animal names."}),
                         {"Authorization": "Bearer " + app.token})
            response = conn.getresponse()
            assert response.status == 200
            assert response.readline()
            response.close(); conn.close()
            app.wait(lambda state: not state["busy"] and state["ready"], timeout=30)
            assert app.request("/app/generate", {"experimental": True, "prompt": "Say hello."})[0] == 200
            code, body, _ = app.request('/app/generate', {'experimental': True, 'prompt':' xy'*3900})
            events=[json.loads(x) for x in body.splitlines()]
            assert code==200 and events[-1]['done'], body
            assert events[-1]['prompt_eval_count'] + events[-1]['max_tokens'] < 4096
            assert app.request('/app/generate', {'experimental': True, 'prompt':' xy'*5000})[0] == 400
            assert not app.status()['busy']
            assert app.request("/app/stop", {})[0] == 200
            assert not private.exists() and not private.parent.exists()
            unloaded = app.status()
            assert not unloaded['ready'] and unloaded['resources']['rss_bytes'] is None and unloaded['resources']['cpu_percent'] is None
            print("app inference: real streaming, metrics, client abort, restart of generation and unload passed")
        finally:
            app.close()

    # A curated selection, unlike --model, feeds measured advice back into its card.
    if Path(model).stat().st_size == 386404992:
        with tempfile.TemporaryDirectory(prefix="geist-advice-") as home:
            directory = Path(home) / "models"
            directory.mkdir()
            import shutil
            shutil.copyfile(model, directory / "smollm2-360m-instruct-q8_0.gguf")
            app = App(home, binary=binary, server=Path(os.environ.get("GEIST_EXECUTION_DAEMON", ROOT / "geistd")))
            try:
                assert app.request("/app/select", {"id": "smollm2-360m"})[0] == 202
                app.wait(lambda state: state["ready"], timeout=60)
                code, body, _ = app.request("/app/generate", {
                    "experimental": True,
                    "prompt": "Write a detailed paragraph about how a garden changes through the seasons.",
                    "benchmark": True})
                event = json.loads(body.splitlines()[-1])
                assert code == 200 and 16 <= event["eval_count"] <= 64, body
                card = next(m for m in app.status()["models"] if m["id"] == "smollm2-360m")
                assert card["measured_tokens"] == event["eval_count"]
                expected = event["eval_count"] / (event["eval_duration"] / 1e9)
                assert abs(card["measured_tps"] - expected) < .001
                state=app.wait(lambda s:not s['busy'])
                # Device suitability no longer carries processor-specific speed prose.
                assert abs(state['execution']['performance']['rate'] - expected) < .001
                sample=next(x for x in state['performance_history'] if x['processor']==state['execution']['active'])
                assert sample['tokens']==event['eval_count'] and abs(sample['rate']-expected)<.001
                child = int(subprocess.check_output(['pgrep','-P',str(app.process.pid)],text=True).strip())
                import signal
                os.kill(child, signal.SIGKILL)
                app.wait(lambda state: not state['ready'])
                assert app.request('/app/select', {'id':'smollm2-360m'})[0] == 202
                app.wait(lambda state: state['ready'],timeout=60)
                assert app.request('/app/generate',{'experimental': True, 'prompt':'Say hello.'})[0] == 200
                child = int(subprocess.check_output(['pgrep','-P',str(app.process.pid)],text=True).strip())
                os.kill(child, signal.SIGKILL)
                app.wait(lambda state: not state['ready'] and not state['loading'])
                assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 200
                assert not (directory / 'smollm2-360m-instruct-q8_0.gguf').exists()
                assert app.request("/app/quit", {})[0] == 202
                app.process.wait(timeout=15)
                print("app advice: verified catalog model, actual measured speed and clean quit passed")
            finally:
                app.close()


if __name__ == "__main__":
    main()
