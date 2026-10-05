#!/usr/bin/env python3
"""Real HTTP/worker concurrency with gated local transfers; no internet downloads.

Default CI mode uses a deterministic socket peer (not quality/inference evidence).
Set GEIST_BACKGROUND_MODEL to a cached SmolLM2 GGUF for the additional real-daemon
lifecycle. Only the instrumented app accepts the localhost download override.
"""
import concurrent.futures
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http_test import App, ROOT


def peer():
    if '--backends' in sys.argv:
        print('{}')
        return
    gate = Path(os.environ['GEIST_PEER_GATE'])
    def reply(conn, obj, body=b''):
        header = json.dumps({'ok': True, **obj}).encode()
        conn.sendall(struct.pack('<II', len(header), len(body)) + header + body)
    def exact(conn, size):
        result = b''
        while len(result) < size:
            piece = conn.recv(size-len(result))
            if not piece: raise EOFError()
            result += piece
        return result
    def handle(conn):
        with conn:
            try:
                while True:
                    header, length = struct.unpack('<II', exact(conn, 8))
                    request = json.loads(exact(conn, header)); data = exact(conn, length)
                    op = request['op']
                    if op == 'info': reply(conn, {'backend': os.environ['GEIST_BACKEND'], 'ctx':4096, 'template':'chatml', 'chat_api':True, **({'engine':json.loads(os.environ['GEIST_PEER_ENGINE'])} if os.environ.get('GEIST_PEER_ENGINE') else {})})
                    elif op == 'open': reply(conn, {'session':'1234567890abcdef'})
                    elif op == 'tokenize': reply(conn, {}, struct.pack('<iii',1,2,3))
                    elif op == 'prefill': reply(conn, {'prefilled':len(data)//4})
                    elif op == 'chat_open': reply(conn, {'chat':'1234567890abcdef','ctx':4096})
                    elif op == 'chat_rewind': reply(conn, {'length':request['keep']})
                    elif op == 'chat_send':  # #148: parts, as geistd's runtime sends them
                        reply(conn, {'part':'answer','text':'Fixture response. ', 'done':False})
                        deadline = time.monotonic()+25
                        while gate.exists() and time.monotonic()<deadline:
                            time.sleep(.05)
                            reply(conn, {'part':'answer','text':'', 'done':False})
                        reply(conn, {'part':'answer','text':'Complete.', 'done':False})
                        reply(conn, {'done':True,'finish':'stop','input_tokens':3,'context_tokens':5,'output_tokens':2,
                                     'dropped':0,'prefill_ms':1,'first_answer_ms':1,'generation_ms':100,'total_ms':101,'length':2})
                        return
                    elif op == 'generate':
                        reply(conn, {'piece':'Fixture response. ', 'done':False})
                        deadline = time.monotonic()+25
                        while gate.exists() and time.monotonic()<deadline:
                            time.sleep(.05)
                            reply(conn, {'piece':'', 'done':False})
                        reply(conn, {'piece':'Complete.', 'done':False})
                        reply(conn, {'done':True,'reason':'stop','generated':2,'duration_ns':100000000})
                        return
                    else: reply(conn, {})
            except (OSError, EOFError): pass  # Owned client disconnect cancels its session.
    with socket.socket(fileno=3) as listener:
        while True:
            conn,_ = listener.accept()
            threading.Thread(target=handle,args=(conn,),daemon=True).start()


class Transfer:
    def __init__(self, source):
        self.source = source
        self.requests = queue.Queue()
        self.gates = []
        self.corrupt = False
        self.disconnect = False
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_GET(self):
                offset = int(self.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
                size = owner.source.stat().st_size
                gate = threading.Event(); owner.gates.append(gate)
                try:
                    self.send_response(206 if offset else 200)
                    self.send_header('Content-Length', str(size-offset))
                    if offset: self.send_header('Content-Range', f'bytes {offset}-{size-1}/{size}')
                    self.end_headers()
                    with owner.source.open('rb') as source:
                        source.seek(offset)
                        piece = source.read(65536)
                        self.wfile.write(piece); self.wfile.flush()
                        owner.requests.put((gate, offset))
                        if not gate.wait(25): return
                        if owner.disconnect: return
                        if owner.corrupt:
                            piece = source.read(1)
                            self.wfile.write(bytes([piece[0] ^ 255]))
                        shutil.copyfileobj(source, self.wfile, 65536)
                except (BrokenPipeError, ConnectionResetError): pass
        self.server = ThreadingHTTPServer(('127.0.0.1',0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
    @property
    def url(self): return f'http://127.0.0.1:{self.server.server_port}/fixture.gguf'
    def close(self):
        for gate in self.gates: gate.set()
        self.server.shutdown(); self.server.server_close(); self.thread.join(2)


def model_catalog(source):
    catalog = json.loads((ROOT/'models/catalog.json').read_text())
    catalog['revision'] += 1
    with source.open('rb') as data: digest = hashlib.file_digest(data,'sha256').hexdigest()
    base = {**catalog['models'][1], 'bytes':source.stat().st_size, 'sha256':digest, 'backends':['cpu']}
    catalog['models'] = [{**base,'id':f'background-{suffix}', 'name':f'Fixture {suffix}',
                          'group_id':f'background-{suffix}','group_name':f'Fixture {suffix}',
                          'file':f'background-{suffix}.gguf'} for suffix in ('a','b')]
    return catalog


def chat(app, api=False):
    request = {'model':'background-a','messages':[{'role':'user','content':'Say hello briefly.'}],'max_tokens':16}
    if not api: request.update(prompt='Say hello briefly.',experimental=True)
    code,body,_ = app.request('/v1/chat/completions' if api else '/app/generate',request)
    assert code == 200, ('chat blocked by unrelated download',code,body)
    if api:
        data=json.loads(body); assert data['choices'][0]['message']['content'] and data['usage']['completion_tokens']>0
    else:
        events=[json.loads(line) for line in body.splitlines()]
        assert events[-1].get('done') and events[-1].get('eval_count',0)>0,body
        assert ''.join(item.get('response','') for item in events).strip()
    return code


def scenario(source, daemon, fake=False):
    binary = Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test'))
    catalog = model_catalog(source); a,b = catalog['models']
    transfer = Transfer(source)
    try:
        with tempfile.TemporaryDirectory(prefix='geist-background-') as temporary:
            home=Path(temporary); (home/'models').mkdir()
            shutil.copyfile(source, home/'models'/a['file'])
            (home/'catalog.json').write_text(json.dumps(catalog))
            (home/'selected').write_text(a['id'])
            peer_gate=home/'hold-generation'
            env={**os.environ,'GEIST_TEST_MODEL_URL':transfer.url,'GEIST_PEER_GATE':str(peer_gate)}
            app=App(home,binary=binary,server=daemon,env=env)
            pool=concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def stable():
                state=app.status()
                assert state['ready'] and state['active_id']==a['id'],state
                assert json.loads(app.request('/app/connections')[1])['daemon_pid']==pid
                assert (home/'selected').read_text().strip()==a['id']
                return state
            def download():
                assert app.request('/app/download',{'id':b['id']})[0]==202
                gate,offset=transfer.requests.get(timeout=10)
                app.wait(lambda s:s['phase']=='downloading' and s['received']>offset)
                return gate,offset
            try:
                app.wait(lambda s:s['ready'] and not s['busy'],timeout=60)
                pid=json.loads(app.request('/app/connections')[1])['daemon_pid']
                gate,offset=download(); assert offset==0
                chat(app); chat(app,api=True)  # Fails with HTTP 409 on the old implementation.
                state=stable(); assert state['busy'] and state['background_download'] and not state['inference_busy']
                for endpoint,body in [('/app/select',{'id':a['id']}),('/app/remove',{'id':a['id']}),
                                      ('/app/download',{'id':a['id']}),('/app/execution',{'mode':'cpu'}),('/app/catalog',catalog)]:
                    assert app.request(endpoint,body)[0]==409,(endpoint,body)
                assert app.request('/app/cancel',{})[0]==200
                app.wait(lambda s:not s['phase']); gate.set(); stable()
                # Initiate/resume a download and pause it while a response is held open.
                if fake:
                    peer_gate.touch(); future=pool.submit(chat,app)
                    app.wait(lambda s:s['inference_busy'])
                    assert app.request('/app/preview',{'id':b['id'],'experimental':True})[0]==200
                    gate,offset=download(); assert offset>0
                    assert stable()['inference_busy']
                    assert app.request('/v1/chat/completions',{'model':a['id'],'messages':[{'role':'user','content':'hello'}]})[0]==429
                    assert app.request('/app/cancel',{})[0]==200
                    app.wait(lambda s:not s['phase']); gate.set()
                    assert stable()['inference_busy'] and not future.done()
                    gate,offset=download(); assert offset>0
                    gate.set()
                    app.wait(lambda s:not s['phase'] and s['models'][1]['installed'])
                    assert stable()['inference_busy'] and not future.done(), 'completion must not stop an in-flight answer'
                    peer_gate.unlink(); assert future.result(timeout=15)==200
                    app.wait(lambda s:not s['inference_busy'])
                    records=[json.loads(line) for line in app.request('/app/performance/export')[1].splitlines()]
                    assert records[-1]['contention'], 'download that starts and finishes mid-request must remain attributed' 
                else:
                    gate,offset=download(); assert offset>0
                    chat(app); gate.set()
                    app.wait(lambda s:not s['phase'] and s['models'][1]['installed'],timeout=90)
                stable()
                with (home/'models'/b['file']).open('rb') as data: assert hashlib.file_digest(data,'sha256').hexdigest()==b['sha256']
                assert app.status()['message']=='Download complete.'
                chat(app)
                # Chat cancellation is isolated from the file transfer as well.
                if fake:
                    assert app.request('/app/remove',{'id':b['id']})[0]==200
                    gate,_=download(); peer_gate.touch()
                    conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
                    conn.request('POST','/app/generate',json.dumps({'prompt':'Hello','experimental':True}),{'Authorization':'Bearer '+app.token})
                    response=conn.getresponse(); assert response.status==200; response.readline()
                    response.close(); conn.close()
                    state=app.wait(lambda s:not s['inference_busy'])
                    assert state['phase']=='downloading' and state['background_download']
                    # Aborting a synchronous request now reaps/reloads only the
                    # owned runtime. The unrelated transfer must remain intact.
                    old_pid=pid
                    pid=json.loads(app.request('/app/connections')[1])['daemon_pid']
                    assert pid>0 and pid!=old_pid
                    try: os.kill(old_pid,0)
                    except ProcessLookupError: pass
                    else: raise AssertionError('cancelled owned runtime survives')
                    peer_gate.unlink(); gate.set(); app.wait(lambda s:not s['phase']); stable()
                    # Network failure then corrupt completion leave A usable and B retryable.
                    for mode in ('disconnect','corrupt'):
                        assert app.request('/app/remove',{'id':b['id']})[0]==200
                        setattr(transfer,mode,True); gate,_=download(); gate.set()
                        state=app.wait(lambda s:not s['phase'])
                        assert not state['models'][1]['installed'] and state['message'],state
                        stable(); chat(app); setattr(transfer,mode,False)
                    gate,_=download(); gate.set(); app.wait(lambda s:not s['phase'] and s['models'][1]['installed']); stable()
            finally:
                peer_gate.unlink(missing_ok=True)
                for gate in transfer.gates: gate.set()
                pool.shutdown(wait=True)
                app.close()
            # Background completion never persists a new selection; explicit click does.
            app=App(home,binary=binary,server=daemon,env=env)
            try:
                state=app.wait(lambda s:s['ready'] and not s['busy'],timeout=60)
                assert state['active_id']==a['id'] and state['models'][1]['installed']
                assert app.request('/app/select',{'id':b['id']})[0]==202
                app.wait(lambda s:s['ready'] and not s['busy'] and s['active_id']==b['id'],timeout=60)
            finally: app.close()
        # A first download on an empty installation still activates automatically.
        with tempfile.TemporaryDirectory(prefix='geist-first-download-') as temporary:
            home=Path(temporary); (home/'catalog.json').write_text(json.dumps(catalog))
            app=App(home,binary=binary,server=daemon,env={**env,'GEIST_PEER_GATE':str(home/'hold')})
            try:
                assert app.request('/app/download',{'id':b['id']})[0]==202
                gate,_=transfer.requests.get(timeout=10)
                state=app.status(); assert state['inference_busy'] and not state['background_download']
                gate.set()
                app.wait(lambda s:s['ready'] and not s['busy'] and s['active_id']==b['id'],timeout=90)
                assert (home/'selected').read_text().strip()==b['id']
            finally: app.close()
    finally: transfer.close()
    print('background download: '+('deterministic peer' if fake else 'REAL CPU INFERENCE')+
          ' — chat + editor endpoint, resume, hash, PID/selection isolation, explicit activation, restart and first download passed',flush=True)


if __name__=='__main__':
    if '--peer' in sys.argv: peer()
    else:
        with tempfile.TemporaryDirectory(prefix='geist-transfer-fixture-') as temporary:
            root=Path(temporary); source=root/'fixture.gguf'; source.write_bytes(b'x'*1048576)
            wrapper=root/'peer'; wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(sys.executable)+' '+shlex.quote(str(Path(__file__).resolve()))+' --peer "$@"\n'); wrapper.chmod(0o700)
            scenario(source,wrapper,fake=True)
        cached=os.environ.get('GEIST_BACKGROUND_MODEL')
        if cached: scenario(Path(cached).resolve(strict=True),Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution')))
        else: print('real background inference SKIPPED: set GEIST_BACKGROUND_MODEL to a cached GGUF',flush=True)
