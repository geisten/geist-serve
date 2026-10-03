#!/usr/bin/env python3
"""Model-free checks for the mini benchmark: frozen suite, scoring and the report."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402
import build_suite  # noqa: E402


class SuiteTests(unittest.TestCase):
    def test_builder_reproduces_the_frozen_suite(self):
        before = {p.name: p.read_bytes() for p in bench.SUITE.glob('*.json')}
        build_suite.build()
        self.assertEqual(before, {p.name: p.read_bytes() for p in bench.SUITE.glob('*.json')})

    def test_twenty_unique_cases_per_task_and_language(self):
        inputs = set()
        for task, spec in bench.load_suite().items():
            for language in ('de', 'en'):
                self.assertEqual(sum(c['language'] == language for c in spec['cases']), 20, (task, language))
            inputs |= {c['input'] for c in spec['cases']}
        self.assertEqual(len(inputs), 160)


class ScoreTests(unittest.TestCase):
    def check(self, task, expected, output, marker=None):
        return bench.score(task, dict(expected=expected), output, marker)

    def test_classify_and_extract(self):
        want = dict(category='billing', priority='normal')
        self.assertEqual(self.check('classify', want, '{"category":"Billing","priority":"normal"}'), (True, None))
        self.assertEqual(self.check('classify', want, '```json\n{"category":"billing","priority":"normal"}\n```'), (False, 'not_json'))
        self.assertEqual(self.check('classify', want, '{"category":"billing","priority":"urgent"}'), (False, 'wrong:priority'))
        self.assertEqual(self.check('classify', want, '{"category":"billing"}'), (False, 'wrong_keys'))
        want = dict(name='Anna Weber', people=4, amount=1249.5, time=None)
        self.assertEqual(self.check('extract', want, '{"name":"anna weber","people":4,"amount":1249.50,"time":null}'), (True, None))
        self.assertEqual(self.check('extract', want, '{"name":"Anna Weber","people":"4","amount":1249.5,"time":null}'), (False, 'wrong:people'))
        self.assertEqual(self.check('extract', want, '{"name":"Anna Weber","people":true,"amount":1249.5,"time":"19:00"}'),
                         (False, 'wrong:people,time'))

    def test_format(self):
        self.assertEqual(self.check('format', dict(check='bullets', count=3), '- a\n- b\n- c\n'), (True, None))
        self.assertEqual(self.check('format', dict(check='bullets', count=3), 'Here:\n- a\n- b\n- c'), (False, 'not_bullets'))
        self.assertEqual(self.check('format', dict(check='bullets', count=3), '- a\n- b'), (False, 'bullets:2'))
        keys = dict(check='json_keys', keys=['summary', 'title'])
        self.assertEqual(self.check('format', keys, '{"title":"T","summary":"S"}'), (True, None))
        self.assertEqual(self.check('format', keys, '{"title":"T","summary":""}'), (False, 'wrong_keys'))
        self.assertEqual(self.check('format', dict(check='max_words', words=3), 'one two three'), (True, None))
        self.assertEqual(self.check('format', dict(check='max_words', words=3), 'one two three four'), (False, 'words:4'))
        self.assertEqual(self.check('format', dict(check='one_line_upper'), 'GO HIKING'), (True, None))
        self.assertEqual(self.check('format', dict(check='one_line_upper'), 'Go hiking'), (False, 'not_upper'))
        self.assertEqual(self.check('format', dict(check='one_line_upper'), 'A\nB'), (False, 'not_one_line'))

    def test_context(self):
        need = dict(required=['8|08', 'Keller'])
        self.assertEqual(self.check('context', need, 'At 8:00, says Keller.', 'NOT IN TEXT'), (True, None))
        self.assertEqual(self.check('context', need, 'At 18:00, says Keller.', 'NOT IN TEXT'), (False, 'missing:8|08'))
        self.assertEqual(self.check('context', need, 'NOT IN TEXT', 'NOT IN TEXT'), (False, 'missed_answer'))
        absent = dict(absent=True)
        self.assertEqual(self.check('context', absent, 'Nicht im Text.', 'NICHT IM TEXT'), (True, None))
        self.assertEqual(self.check('context', absent, 'Twelve employees.', 'NOT IN TEXT'), (False, 'no_marker'))
        self.assertEqual(self.check('context', absent, 'Sorry, NOT IN TEXT', 'NOT IN TEXT'), (False, 'not_exact'))


class ReportTests(unittest.TestCase):
    def make_run(self, root, name, passed=True, change_suite=False):
        suite = bench.load_suite()
        d = root/name
        d.mkdir(parents=True)
        rows = [dict(task=t, id=c['id'], language=c['language'], tags=c['tags'], passed=passed, ms=1000.0 + i,
                     reason=None if passed else 'no_marker', usage=dict(completion_tokens=10))
                for t, s in suite.items() for i, c in enumerate(s['cases'])]
        (d/'results.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
        hashes = {t: dict(version='1.0.0', sha256='0' * 64 if change_suite else bench.sha256(bench.SUITE/f'{t}.json')) for t in suite}
        run = dict(candidate=name, execution=dict(active='gpu', backend='metal'), host={}, errors=[], suite=hashes,
                   model=dict(bytes=1, quantization='Q8_0'), prepare_ms=5.0, finished_utc='2026-10-03T20:00:00+00:00',
                   engine=dict(source_dirty=False, engine_pin='33db79d7764b4f6177d944e8ae4fd9f7fadea9be'), memory=dict(max_rss_bytes=1 << 30, max_gpu_allocated_bytes=None))
        (d/'run.json').write_text(json.dumps(run))

    def test_summary_and_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_run(root, 'good')
            self.make_run(root, 'bad', passed=False, change_suite=True)
            suite = bench.load_suite()
            good, bad = bench.summarize(root/'good', suite), bench.summarize(root/'bad', suite)
            self.assertEqual((good['passed'], good['total'], good['complete'], good['stale']), (160, 160, True, []))
            self.assertEqual(good['tasks']['classify']['de'], dict(passed=20, total=20))
            self.assertAlmostEqual(good['output_tokens_per_s'], 1600 / sum(1000.0 + i for i in range(40)) * 1000 / 4)
            self.assertEqual(bad['tasks']['context']['no_marker'], 40)  # synthetic rows: every one marked no_marker
            self.assertEqual(bad['stale'], sorted(bench.TASKS))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                bench.report(type('A', (), dict(run=str(root), json=False))())
            self.assertIn('STALE', out.getvalue())
            self.assertIn('good', out.getvalue())

    def test_quality_fields_only_from_clean_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_run(root/'one', 'good')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                bench.quality(type('A', (), dict(run=str(root/'one'), write=False))())
            field = json.loads(out.getvalue())['good']
            self.assertEqual((field['suite'], field['date'], field['engine']), (bench.suite_id(), '2026-10-03', '33db79d7764b'))
            self.assertEqual(field['tasks']['classify'], dict(de=[20, 20], en=[20, 20]))
            self.make_run(root/'two', 'bad', change_suite=True)
            with self.assertRaises(ValueError):
                bench.quality(type('A', (), dict(run=str(root/'two'), write=False))())

    def test_catalog_quality_has_its_evidence(self):
        models = json.loads((bench.ROOT/'models/catalog.json').read_text())['models']
        for model in (m for m in models if 'quality' in m):
            q = model['quality']
            raw = HERE/'evidence'/q['suite']/model['id']/'results.jsonl'
            self.assertEqual(bench.sha256(raw), q['evidence'], model['id'])
            if q['suite'] == bench.suite_id():
                r = bench.summarize(raw.parent, bench.load_suite())
                self.assertEqual({t: {l: [c[l]['passed'], c[l]['total']] for l in ('de', 'en')} for t, c in r['tasks'].items()}, q['tasks'])

    def test_p95_is_nearest_rank(self):
        self.assertEqual(bench.p95(list(range(1, 21))), 19)
        self.assertEqual(bench.p95([5.0]), 5.0)


if __name__ == '__main__':
    unittest.main()
