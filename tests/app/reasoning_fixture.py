#!/usr/bin/env python3
"""Controlled daemon peer for HTTP adapter tests. Never used as model evidence."""
import json,os,socket,struct,sys,time,signal
from pathlib import Path
if '--backends' in sys.argv:
    print(json.dumps({'cpu':{'name':'cpu_neon'},'gpu':{'name':'metal','available':False}}));sys.exit()
root=Path(sys.argv[1]).parent.parent
startup=json.loads((root/'fixture.json').read_text())
if startup.get('ignore_term'):signal.signal(signal.SIGTERM,signal.SIG_IGN)
(root/'fixture-pid').write_text(str(os.getpid()))
if startup.get('load_pause'):
    (root/'load-started').write_text('yes');time.sleep(startup['load_pause'])
server=socket.socket(fileno=3)
def exact(conn,n):
    data=b''
    while len(data)<n:
        b=conn.recv(n-len(data))
        if not b:raise ConnectionResetError()
        data+=b
    return data
def frame(conn,obj,body=b''):
    h=json.dumps(obj).encode();conn.sendall(struct.pack('<II',len(h),len(body))+h+body)
while True:
    conn,_=server.accept()
    with conn:
        try:
            a,b=struct.unpack('<II',exact(conn,8));r=json.loads(exact(conn,a));payload=exact(conn,b)
            config=json.loads((root/'fixture.json').read_text());op=r['op']
            (root/(op+'-started')).write_text('yes')
            if op!='prefill':time.sleep(config.get(op+'_pause',0))
            if op=='info':frame(conn,{'ok':True,'ctx':4096,'template':'chatml','backend':os.environ.get('GEIST_BACKEND','cpu_neon')})
            elif op=='open':frame(conn,{'ok':True,'session':'0123456789abcdef'})
            elif op=='tokenize' and config.get('fail')=='context':frame(conn,{'ok':False,'error':'tokenize: text too long for the context'})
            elif op=='tokenize':frame(conn,{'ok':True},struct.pack('<'+'i'*config.get('input',12), *([1]*config.get('input',12))))
            elif op=='prefill':
                (root/'prefill-started').write_text('yes')
                time.sleep(config.get('prefill_pause',0))
                if config.get('fail')=='prefill':frame(conn,{'ok':False,'error':'controlled failure'})
                else:frame(conn,{'ok':True,'n':config.get('input',12),'reused':0})
            elif op=='generate':
                (root/'requested-max').write_text(str(r['max']))
                if config.get('fail')=='generate':frame(conn,{'ok':False,'error':'controlled failure'})
                else:
                    for c in config.get('text','<think>SECRET</think>**Answer** → 🌿'):
                        frame(conn,{'ok':True,'piece':c,'done':False})
                        time.sleep(config.get('piece_pause',0))
                    frame(conn,{'ok':True,'done':True,'reason':config.get('reason','stop'),'generated':config.get('tokens',50),'duration_ns':1000000000})
            else:frame(conn,{'ok':True})
        except (BrokenPipeError,ConnectionResetError):pass
