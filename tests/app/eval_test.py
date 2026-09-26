import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('compare',Path(__file__).resolve().parents[2]/'eval/compare.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert m.summary([{'complete':False,'output':'wrong','phase':'warm','wall_ms':0}]) == {}
assert m.summary([{'complete':True,'output':'','phase':'warm','wall_ms':0}]) == {}
assert m.summary([{'complete':True,'output':'ok','phase':'warm','wall_ms':2},{'complete':True,'output':'ok','phase':'warm','wall_ms':4}])['warm']['median_wall_ms']==3
try: m.summary([{'complete':True,'output':'ok','phase':'warm','wall_ms':-1}]); raise AssertionError('accepted negative')
except ValueError: pass
assert m.compatible({},{}), 'missing metadata cannot prove comparability'
config={k:1 for k in ('model_sha256','prompt_token_ids','threads','context','temperature','top_p','top_k','seed','max_tokens','backend','kv_type','cache_enabled')}
assert not m.compatible(config,config)
assert m.compatible(config,dict(config,cache_enabled=False))==['cache_enabled']
assert m.compatible(config,dict(config,model_sha256='other'))==['model_sha256']
print('evaluation: incomplete/empty runs, invalid duration, missing metadata, cache and model mismatch passed')

# A failed child startup preserves an error artifact instead of a success row.
import argparse, json, sys, tempfile
if sys.platform == 'darwin':
    with tempfile.TemporaryDirectory() as home:
        model=Path(home)/'model'; model.write_bytes(b'fixture')
        output=Path(home)/'failed.json'
        args=argparse.Namespace(model=str(model),output=str(output),prompt_file=None,
            threads=2,repeats=1,geistd='/usr/bin/false',llama_server='/usr/bin/false')
        try: m.run(args); raise AssertionError('accepted failed engine')
        except RuntimeError: pass
        assert 'run_error' in json.loads(output.read_text())
        try: m.run(args); raise AssertionError('overwrote raw evidence')
        except ValueError: pass
    print('evaluation: failed startup retained, existing raw result protected')
