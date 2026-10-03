"""Build the offline C fallback from the same editable/importable JSON catalog."""
import hashlib
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
catalog=json.loads((root/'models/catalog.json').read_text())
assert catalog['schema']==2 and 0 < len(catalog['models']) <= 32
rows=[]
for m in catalog['models']:
 fields=[json.dumps(m[k],ensure_ascii=True) for k in ('id','name','file','url','sha256')]
 fields += [str(m[k]) for k in ('bytes','working_mib','recommended_ram_gib')]
 fields += [str(sum({'cpu':1,'metal':2,'vulkan':4}[b] for b in m['backends']))]
 fields += [json.dumps(m['unsupported_format']) if 'unsupported_format' in m else 'nullptr']
 fields += [json.dumps(m[k],ensure_ascii=True) for k in ('group_id','group_name','quantization')]
 fields += [json.dumps(m.get('reasoning_format','none'))]
 fields += [json.dumps(json.dumps(m['quality'],separators=(',',':'))) if 'quality' in m else 'nullptr']
 rows.append('    {'+', '.join(fields)+'},')
(root/'build').mkdir(exist_ok=True)
(root/'build/app_models.h').write_text('/* Generated from models/catalog.json. */\nstruct app_model app_models[APP_MODEL_COUNT] = {\n'+'\n'.join(rows)+'\n};\nsize_t app_model_count = '+str(len(rows))+';\nunsigned app_catalog_revision = '+str(catalog['revision'])+';\n')
# #102: same id as workbench/bench.py suite_id(); a catalog result for another id is an older test.
suite=hashlib.sha256(''.join(hashlib.sha256((root/f'workbench/suite/{t}.json').read_bytes()).hexdigest() for t in ('classify','extract','format','context')).encode()).hexdigest()[:12]

with (root/'build/app_models.h').open('a') as out:
    out.write('const char *app_quality_suite = "'+suite+'";\n')
    out.write('const char *app_catalog_json = '+json.dumps(json.dumps(catalog,separators=(',',':')))+';\n')
