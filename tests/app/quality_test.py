#!/usr/bin/env python3
import copy
import importlib.util
import json
from pathlib import Path
import sys
import shutil
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'quality'))
import evidence
from evidence import POLICY, summarize, load_bundle, sha, source_hash
from evaluate import automatic


class QualityTests(unittest.TestCase):
    def fixture(self):
        rows=[{'id':f'{task}-{lang}-{i}', 'task':task, 'language':lang, 'automatic_pass':True, 'critical':False}
              for task in POLICY['tasks'] for lang in POLICY['languages'] for i in range(50)]
        rows += [{'id':f'adverse-{task}-{lang}-{i}', 'task':task, 'language':lang, 'adverse':True, 'automatic_pass':True, 'critical':False}
              for task in POLICY['tasks'] for lang in POLICY['languages'] for i in range(5)]
        run={'split':'acceptance','rows':rows}
        ratings={row['id']:{'pass':True,'critical':False,'reason':'Meets the stated rubric'} for row in rows}
        return run,ratings

    def test_missing_or_partial_human_review_never_passes(self):
        run,ratings=self.fixture()
        self.assertTrue(all(c['quality']=='passed' for c in summarize(run,ratings)))
        self.assertTrue(all(c['quality']=='unverified' for c in summarize(run,{})))
        self.assertFalse(any(c['quality']=='passed' for c in summarize(run,dict(list(ratings.items())[:1]))))

    def test_language_cannot_hide_failure(self):
        run,ratings=self.fixture()
        for row in run['rows']:
            if row['language']=='de' and row['task']=='summary': row['automatic_pass']=False
        cells=summarize(run,ratings)
        self.assertEqual(next(c for c in cells if c['task']=='summary' and c['language']=='de')['quality'],'failed')
        self.assertEqual(next(c for c in cells if c['task']=='summary' and c['language']=='en')['quality'],'passed')

    def test_screening_missing_cases_duplicates_and_critical_errors(self):
        run,ratings=self.fixture();run['split']='screening'
        self.assertFalse(any(c['quality']=='passed' for c in summarize(run,ratings)))
        run['split']='acceptance';run['rows'].pop()
        self.assertNotEqual(summarize(run,ratings)[-1]['quality'],'passed')
        run['rows'].append(run['rows'][0])
        with self.assertRaises(ValueError):summarize(run,ratings)
        run,ratings=self.fixture();run['rows'][0]['critical']=True
        self.assertEqual(summarize(run,ratings)[0]['quality'],'failed')

    def test_runtime_errors_and_truncation_fail(self):
        case={'task':'summary','required':['17']}
        self.assertTrue(automatic(case,'There are 17 guests.')['automatic_pass'])
        self.assertTrue(automatic(case,'There are seventeen guests.')['automatic_pass'])
        self.assertFalse(automatic(case,'There are 170 guests.')['automatic_pass'])
        self.assertFalse(automatic(case,'There are 17 guests.',limited=True)['automatic_pass'])
        self.assertFalse(automatic(case,'There are 17 guests.',error='disconnect')['automatic_pass'])
        ideas={'task':'ideas','required':[]}
        self.assertTrue(automatic(ideas,'Here are three ideas:\n1. Fold paper.\n2. Draw a map.\n3. Write a story.')['automatic_pass'])
        self.assertFalse(automatic(ideas,'1. Fold paper.\n2. Fold paper.\n3. Write a story.')['automatic_pass'])

    def test_frozen_corpus_matrix_and_no_duplicate_prompts_between_splits(self):
        cases=json.loads((ROOT/'quality/corpus.json').read_text())['cases']
        self.assertEqual(len(cases),480)
        self.assertEqual(len({c['id'] for c in cases}),480)
        screen={(c['task'],c['language'],c['prompt']) for c in cases if c['split']=='screening'}
        final={(c['task'],c['language'],c['prompt']) for c in cases if c['split']=='acceptance'}
        self.assertFalse(screen & final)
        self.assertEqual(len(final),400)
        for split,n in [('screening',10),('acceptance',50)]:
            for task in POLICY['tasks']:
                for lang in POLICY['languages']:
                    self.assertEqual(sum(c['split']==split and c['task']==task and c['language']==lang for c in cases),n)

    def test_adverse_and_human_failures_are_not_unverified(self):
        run,ratings=self.fixture()
        sample=summarize(run,ratings)[0]['sample_ids']
        for case in sample[:2]: ratings[case]['pass']=False
        self.assertEqual(summarize(run,ratings)[0]['quality'],'failed')
        run,ratings=self.fixture()
        ratings[sample[-1]]['pass']=False
        self.assertEqual(summarize(run,ratings)[0]['quality'],'failed')
        run,ratings=self.fixture()
        run['rows']=[r for r in run['rows'] if not r.get('adverse')]
        self.assertFalse(any(c['quality']=='passed' for c in summarize(run,ratings)))
        from build_adverse import build
        cases=build()['cases']
        self.assertEqual(len(cases),40)
        self.assertEqual(len({c['id'] for c in cases}),40)

    def test_evidence_import_rejects_tampering_and_stale_identity(self):
        # These fabricated answers exist only in a temporary unit-test fixture;
        # they are never installed as model evidence.
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve()
            for directory in ('src','clients','tasks','quality'):
                shutil.copytree(ROOT/directory,root/directory,ignore=shutil.ignore_patterns('__pycache__','bundles'))
            for name in ('Makefile','App.mk'):shutil.copyfile(ROOT/name,root/name)
            bundle=root/'quality/bundles/unit-test';bundle.mkdir(parents=True)
            for name in ('corpus.json','adverse.json'):shutil.copyfile(root/'quality'/name,bundle/name)
            cases=json.loads((bundle/'corpus.json').read_text())['cases']+json.loads((bundle/'adverse.json').read_text())['cases']
            rows=[]
            for c in cases:
                if c['split']=='screening':continue
                output='1. Fold paper.\n2. Draw a map.\n3. Write a story.' if c['task']=='ideas' else 'Unit fixture: '+' '.join(c['required'])
                rows.append(dict(id=c['id'],task=c['task'],language=c['language'],adverse=c['split']=='adverse',output=output,stats={},error=None,**automatic(c,output)))
            reviews={r['id']:{'pass':True,'critical':False,'reason':'Synthetic unit-test fixture only'} for r in rows}
            with patch.object(evidence,'ROOT',root):
                run=dict(rows=rows,split='acceptance',model_id='bitnet-2b',model_sha256='4221b252fdd5fd25e15847adfeb5ee88886506ba50b8a34548374492884c2162',
                         device='pi5',engine_ref='25861c0bd197f1a98f17e49efe0cdc48a0e40713',app_sha256='a'*64,daemon_sha256='b'*64,
                         scorer_sha256=sha(root/'quality/evaluate.py'),source_sha256=source_hash(),policy_sha256=sha(root/'quality/policy.json'),
                         configuration=POLICY['configuration'],corpus_sha256=sha(bundle/'corpus.json'),adverse_sha256=sha(bundle/'adverse.json'),
                         task_versions={t['id']:t['version'] for t in json.loads((root/'tasks/catalog.json').read_text())['tasks']})
                def save():
                    (bundle/'run.json').write_text(json.dumps(run))
                    (bundle/'reviews.json').write_text(json.dumps(reviews))
                    (bundle/'responses.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
                    (bundle/'manifest.json').write_text(json.dumps({'human_reviewer':'test fixture','sha256':{n:sha(bundle/n) for n in ('run.json','reviews.json','corpus.json','adverse.json','responses.jsonl')}}))
                save();self.assertTrue(all(c['quality']=='passed' for c in load_bundle(bundle)[1]))
                run['engine_ref']='0'*40;save()
                with self.assertRaisesRegex(ValueError,'identity mismatch'):load_bundle(bundle)
                run['engine_ref']='25861c0bd197f1a98f17e49efe0cdc48a0e40713'
                rows[0]['automatic_pass']=False;save()
                with self.assertRaisesRegex(ValueError,'score disagrees'):load_bundle(bundle)
                rows[0]['automatic_pass']=True;save()
                (bundle/'reviews.json').write_text('{}')
                with self.assertRaisesRegex(ValueError,'hash mismatch'):load_bundle(bundle)
                save();run['source_sha256']='0'*64;save()
                with self.assertRaisesRegex(ValueError,'source/policy'):load_bundle(bundle)


if __name__=='__main__':unittest.main()
