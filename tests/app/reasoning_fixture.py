#!/usr/bin/env python3
"""Controlled daemon peer for HTTP adapter tests. Never used as model evidence."""
import json,os,socket,struct,sys,time,signal
from pathlib import Path
if '--backends' in sys.argv:
    print(json.dumps({'cpu':{'name':'cpu_neon'},'gpu':{'name':'metal','available':os.environ.get('GEIST_FIXTURE_GPU')=='1'}}));sys.exit()
root=Path(sys.argv[1]).parent.parent
startup=json.loads((root/'fixture.json').read_text())
print('fixture startup '+os.environ.get('GEIST_BACKEND','unknown'),flush=True)
if startup.get('crash_load') or (startup.get('crash_gpu') and os.environ.get('GEIST_BACKEND')=='metal'):sys.exit(17)
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
            # #148: chat ops; their stages carry the old markers (open, prefill, generate)
            stage={'chat_open':'open'}.get(op,op)
            if stage!=op:(root/(stage+'-started')).write_text('yes')
            if op not in ('prefill','chat_send'):time.sleep(config.get(stage+'_pause',0))
            if op=='info':frame(conn,{'ok':True,'ctx':4096,'template':'chatml','chat_api':True,'backend':os.environ.get('GEIST_BACKEND','cpu_neon')})
            elif op=='open':frame(conn,{'ok':True,'session':'0123456789abcdef'})
            elif op=='tokenize' and config.get('fail')=='context':frame(conn,{'ok':False,'error':'tokenize: text too long for the context'})
            elif op=='tokenize':frame(conn,{'ok':True},struct.pack('<'+'i'*config.get('input',12), *([1]*config.get('input',12))))
            elif op=='prefill':
                (root/'prefill-started').write_text('yes')
                time.sleep(config.get('prefill_pause',0))
                if config.get('fail')=='prefill':frame(conn,{'ok':False,'error':'controlled failure'})
                else:frame(conn,{'ok':True,'n':config.get('input',12),'reused':0})
            elif op=='chat_open':frame(conn,{'ok':True,'chat':'0123456789abcdef','ctx':4096})
            elif op=='chat_rewind':frame(conn,{'ok':True,'length':r['keep']})
            elif op=='chat_send':
                # What geistd's runtime does: the prompt must fit, the answer arrives as
                # parts with the thinking separated, the effective limit is the rest.
                n=config.get('input',12);effective=r['max'] or 4096-n-1
                (root/'requested-max').write_text(str(effective))
                (root/'prefill-started').write_text('yes')
                time.sleep(config.get('prefill_pause',0))
                if config.get('fail')=='context' or n>=4095:frame(conn,{'ok':False,'status':'context','error':'the conversation does not fit'})
                elif config.get('fail')=='prefill':frame(conn,{'ok':False,'status':'error','error':'controlled failure'})
                else:
                    frame(conn,{'ok':True,'done':False,'part':'answer','text':''})  # input processed
                    (root/'generate-started').write_text('yes');time.sleep(config.get('generate_pause',0))
                    if config.get('fail')=='generate':frame(conn,{'ok':False,'status':'error','error':'controlled failure'})
                    else:
                        text=config.get('text','<think>SECRET</think>**Answer** → 🌿')
                        # the runtime's output stage: thinking only at the start; unterminated is all
                        # thinking; a partial marker at the end is nothing; elsewhere literal text
                        if text.startswith('<think>'):thinking,_,answer=text[7:].partition('</think>')
                        elif '<think>'.startswith(text):thinking,answer='',''
                        else:thinking,answer='',text
                        for part,chars in (('thinking',thinking),('answer',answer)):
                            for c in chars:
                                frame(conn,{'ok':True,'done':False,'part':part,'text':c})
                                time.sleep(config.get('piece_pause',0))
                        tokens=config.get('tokens',50)
                        finish={'stop':'stop','max':'length','context':'context'}.get(config.get('reason','stop'),'stop')
                        frame(conn,{'ok':True,'done':True,'finish':finish,'input_tokens':n,'context_tokens':n+tokens,
                                    'output_tokens':tokens,'dropped':0,'prefill_ms':1,'first_answer_ms':1,'generation_ms':1000,
                                    'total_ms':1001,'length':2})
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
