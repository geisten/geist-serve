#!/usr/bin/env python3
"""bench.py — geisten mini benchmark: what can each local model do on this computer?

    bench.py run --candidate CATALOG_ID [--candidate …] --backend cpu|gpu --output NEW_DIR
    bench.py report RUN_DIR [--json]

Four small tasks (classify, extract, format, context) in German and English,
scored automatically. See docs/MINI-BENCHMARK.md.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
SUITE = Path(__file__).resolve().parent/'suite'
TASKS = ('classify', 'extract', 'format', 'context')
RSS_POLL_S = 1.0


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def load_suite(directory=SUITE):
    return {t: json.loads((Path(directory)/f'{t}.json').read_text(encoding='utf-8')) for t in TASKS}


# ------------------------------------------------------------------- scoring
# One generation per case, no retry and no repair: a code fence around JSON fails.

def has_word(text, word):
    return re.search(r'(?<!\w)' + re.escape(word.casefold()) + r'(?!\w)', text.casefold()) is not None


def json_object(output):
    try:
        value = json.loads(output)
    except ValueError:
        return None
    return value if isinstance(value, dict) else None


def same(got, want):
    if want is None or isinstance(want, str):
        return got == want if want is None else isinstance(got, str) and got.strip().casefold() == want.casefold()
    return type(got) in (int, float) and not isinstance(got, bool) and math.isclose(got, want, abs_tol=1e-9)


def score(task, case, output, marker=None):
    """(passed, reason). reason names the first problem, None when passed."""
    want = case['expected']
    if task in ('classify', 'extract'):
        got = json_object(output)
        if got is None:
            return False, 'not_json'
        if set(got) != set(want):
            return False, 'wrong_keys'
        wrong = [k for k in sorted(want) if not same(got[k], want[k])]
        return (False, 'wrong:' + ','.join(wrong)) if wrong else (True, None)
    if task == 'format':
        check, lines = want['check'], [line for line in output.strip().splitlines() if line.strip()]
        if check == 'bullets':
            if not lines or not all(line.startswith('- ') for line in lines):
                return False, 'not_bullets'
            return (True, None) if len(lines) == want['count'] else (False, f'bullets:{len(lines)}')
        if check == 'json_keys':
            got = json_object(output)
            if got is None:
                return False, 'not_json'
            if sorted(got) != want['keys'] or not all(isinstance(v, str) and v.strip() for v in got.values()):
                return False, 'wrong_keys'
            return True, None
        if check == 'max_words':
            n = len(output.split())
            return (True, None) if 0 < n <= want['words'] else (False, f'words:{n}')
        if check == 'one_line_upper':
            if len(lines) != 1:
                return False, 'not_one_line'
            return (True, None) if any(c.isalpha() for c in output) and output == output.upper() else (False, 'not_upper')
        raise ValueError(f'unknown format check {check}')
    if task == 'context':
        said_absent = marker.casefold() in output.casefold()
        if want.get('absent'):
            # Without the marker the reply may be an invented answer or a paraphrased refusal: only a reader can tell.
            return (True, None) if output.strip().rstrip('.').casefold() == marker.casefold() else (False, 'not_exact' if said_absent else 'no_marker')
        if said_absent:
            return False, 'missed_answer'
        missing = [w for w in want['required'] if not any(has_word(output, alt) for alt in w.split('|'))]
        return (False, 'missing:' + ','.join(missing)) if missing else (True, None)
    raise ValueError(f'unknown task {task}')


# --------------------------------------------------------------------- running

class App:
    """A private geist-app with its own HOME, owned and reaped by this run.

    The candidate is hard-linked into that HOME and selected by catalog id, so
    the app verifies it against the catalog sha256 and knows its backends,
    exactly as for a downloaded model. The user's own geisten home is untouched.
    """

    def __init__(self, entry, model, home):
        home = Path(home)
        (home/'models').mkdir()
        try:
            os.link(model, home/'models'/entry['file'])
        except OSError as error:
            raise RuntimeError(f'cannot link the model into the private home: {error}') from None
        fd = os.open(home/'selected', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.write(fd, entry['id'].encode())
        os.close(fd)
        self.candidate = entry['id']
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(ROOT/'geist-app'), '--port', '0', '--home', str(home), '--daemon', str(ROOT/'geistd')],
                                        stdout=subprocess.PIPE, stderr=self.log, text=True)
        line = self.process.stdout.readline().strip()
        if not line.startswith('GEIST_APP_URL='):
            self.close()
            raise RuntimeError('geist-app did not start')
        url = urlsplit(line.split('=', 1)[1])
        self.port, self.token = url.port, url.fragment

    def call(self, path, data=None, timeout=20):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        try:
            connection.request('POST' if data is not None else 'GET', path, json.dumps(data) if data is not None else None,
                               {'Authorization': 'Bearer ' + self.token, 'Content-Type': 'application/json'})
            response = connection.getresponse()
            return response.status, json.loads(response.read(1 << 20) or b'null')
        finally:
            connection.close()

    def status(self):
        return self.call('/app/status')[1]

    def wait_ready(self, backend, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.status()
            if s.get('ready') and s['execution']['active'] == backend:
                if s.get('active_id') != self.candidate:
                    raise RuntimeError(f'the app runs {s.get("active_id")!r}, not the verified candidate')
                return s
            if s.get('ready') and s['execution']['mode'] != backend:
                code, body = self.call('/app/execution', {'mode': backend})
                if code not in (200, 202):
                    raise RuntimeError(f'cannot select {backend}: {(body or {}).get("error", body)}')
            time.sleep(0.25)
        raise RuntimeError(f'model not ready on {backend} within {timeout} s')

    def complete(self, system, text, max_tokens, timeout=120):
        """One generation through the local OpenAI-compatible endpoint, as a client calls it."""
        body = dict(model=self.candidate, messages=[dict(role='system', content=system), dict(role='user', content=text)],
                    temperature=0, max_tokens=max_tokens, stream=False)
        start = time.monotonic()
        code, reply = self.call('/v1/chat/completions', body, timeout=timeout)
        ms = (time.monotonic() - start) * 1000
        if code != 200:
            raise RuntimeError(f'HTTP {code}: {str(reply)[:200]}')
        choice = reply['choices'][0]
        return choice['message']['content'] or '', choice.get('finish_reason'), reply.get('usage') or {}, ms

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


class MemoryPoller(threading.Thread):
    """Maxima of geistd RSS and GPU allocation as /app/status reports them (sampled, not OS peaks)."""

    def __init__(self, app):
        super().__init__(daemon=True)
        self.app, self.stop, self.rss, self.gpu, self.source, self.samples = app, threading.Event(), None, None, None, 0

    def run(self):
        while not self.stop.wait(RSS_POLL_S):
            try:
                m = self.app.status().get('memory') or {}
            except (OSError, ValueError):
                continue
            if isinstance(m.get('process_rss_bytes'), (int, float)) and m['process_rss_bytes'] >= 0:
                self.rss, self.samples, self.source = max(self.rss or 0, m['process_rss_bytes']), self.samples + 1, m.get('rss_source')
            if isinstance(m.get('gpu_allocated_bytes'), (int, float)) and m['gpu_allocated_bytes'] >= 0:
                self.gpu = max(self.gpu or 0, m['gpu_allocated_bytes'])


def resolve(candidate, model_dir):
    entry = next((m for m in json.loads((ROOT/'models/catalog.json').read_text())['models'] if m['id'] == candidate), None)
    if not entry:
        raise ValueError(f'{candidate}: not in models/catalog.json')
    path = Path(model_dir)/entry['file']
    if path.is_symlink() or not path.is_file() or path.stat().st_size != entry['bytes']:
        raise ValueError(f'{candidate}: {path} is missing or has the wrong size (download it in geisten first)')
    # The private app checks the catalog sha256 before loading (App).
    # ponytail: a full hash on every run; reuse the app's receipt when runs get frequent.
    return entry, path


def engine_identity():
    pin = re.search(r'^GEIST_REF\s*\?=\s*(\S+)', (ROOT/'Makefile').read_text(), re.M)

    def git(*args):
        try:
            return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True, timeout=10).stdout.strip()
        except OSError:
            return ''
    return dict(source=git('rev-parse', 'HEAD') or None, source_dirty=bool(git('status', '--porcelain', '--untracked-files=no')),
                engine_pin=pin and pin.group(1), app_sha256=sha256(ROOT/'geist-app'), geistd_sha256=sha256(ROOT/'geistd'))


def host_identity():
    """Hardware class only: no host name, user name or home path."""
    try:
        memory = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
    except (ValueError, OSError):
        memory = None
    cpu = platform.processor()
    if sys.platform == 'darwin':
        cpu = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'], capture_output=True, text=True).stdout.strip() or cpu
    return dict(os=platform.system(), os_release=platform.release(), machine=platform.machine(), cpu=cpu,
                logical_cpus=os.cpu_count(), memory_bytes=memory)


def run_candidate(suite, entry, model, backend, out):
    out.mkdir()
    record = dict(schema=1, kind='geist-mini-benchmark', candidate=entry['id'], backend_requested=backend,
                  suite={t: dict(version=s['version'], sha256=sha256(SUITE/f'{t}.json')) for t, s in suite.items()},
                  model=dict(catalog_id=entry['id'], file=entry['file'], sha256=entry['sha256'], bytes=entry['bytes'],
                             quantization=entry.get('quantization'), source=entry['url']),
                  engine=engine_identity(), host=host_identity(),
                  sampler=dict(endpoint='/v1/chat/completions', temperature=0, generations_per_case=1, retries=0, warmup=1),
                  started_utc=datetime.now(timezone.utc).isoformat(), errors=[])
    # Same file system as the model, so it can be hard-linked instead of copied.
    with tempfile.TemporaryDirectory(prefix='.geist-bench-', dir=model.parent.parent) as home:
        start = time.monotonic()
        app = App(entry, model, home)
        poller = MemoryPoller(app)
        try:
            status = app.wait_ready(backend, 600)
            record['prepare_ms'] = (time.monotonic() - start) * 1000  # catalog hash check + load + backend switch
            record['execution'] = dict(active=status['execution']['active'], backend=status['execution']['backend'])
            poller.start()
            first = suite['classify']
            app.complete(first['prompt']['en'], first['cases'][0]['input'], first['max_tokens'])  # declared warm-up, not scored
            with open(out/'results.jsonl', 'x', encoding='utf-8') as raw:
                for task, spec in suite.items():
                    for case in spec['cases']:
                        row = dict(task=task, id=case['id'], language=case['language'], tags=case['tags'])
                        try:
                            output, finish, usage, ms = app.complete(spec['prompt'][case['language']], case['input'], spec['max_tokens'])
                            passed, reason = score(task, case, output, (spec.get('marker') or {}).get(case['language']))
                            row.update(output=output, finish_reason=finish, usage=usage, ms=ms, passed=passed, reason=reason)
                        except (OSError, RuntimeError, ValueError, KeyError) as error:
                            row.update(output='', ms=None, passed=False, reason='runtime_error', error=str(error)[:300])
                        raw.write(json.dumps(row, ensure_ascii=False) + '\n')
                        raw.flush()
                        print(f'{entry["id"]} {case["id"]}: {"pass" if row["passed"] else row["reason"]}', flush=True)
        except (OSError, RuntimeError, ValueError, KeyError) as error:
            record['errors'].append(str(error)[:500])
        finally:
            poller.stop.set()
            app.close()
    record['memory'] = dict(metric='max of /app/status memory.process_rss_bytes and gpu_allocated_bytes (geistd)',
                            rss_source=poller.source, poll_interval_ms=RSS_POLL_S * 1000, samples=poller.samples,
                            max_rss_bytes=poller.rss, max_gpu_allocated_bytes=poller.gpu)
    record['finished_utc'] = datetime.now(timezone.utc).isoformat()
    with open(out/'run.json', 'x', encoding='utf-8') as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
        f.write('\n')


def run(args):
    suite = load_suite()
    if len(set(args.candidate)) != len(args.candidate):
        raise ValueError('candidates must be distinct')
    for binary in ('geist-app', 'geistd'):
        if not (ROOT/binary).is_file():
            raise ValueError(f'{binary} is missing: run make and make app first')
    resolved = [resolve(c, args.model_dir) for c in args.candidate]
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)  # never overwrite results
    for entry, model in resolved:  # one resident model at a time
        print(f'{entry["id"]}: {model.name} on {args.backend}', flush=True)
        run_candidate(suite, entry, model, args.backend, out/entry['id'])
    print(f'results: {out}\nreport:  {Path(__file__).name} report {out}')


# --------------------------------------------------------------------- report

def p95(values):
    """Nearest-rank p95."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def summarize(directory, suite):
    run = json.loads((directory/'run.json').read_text())
    rows = [json.loads(line) for line in (directory/'results.jsonl').read_text().splitlines()] if (directory/'results.jsonl').exists() else []
    expected = {(t, c['id']) for t, s in suite.items() for c in s['cases']}
    current = {t: sha256(SUITE/f'{t}.json') for t in suite}
    tasks = {}
    for task in suite:
        cell = {}
        for language in ('de', 'en'):
            mine = [r for r in rows if r['task'] == task and r['language'] == language]
            total = sum(1 for c in suite[task]['cases'] if c['language'] == language)
            cell[language] = dict(passed=sum(r['passed'] for r in mine), total=total)
        if task == 'context':
            cell['no_marker'] = sum(r.get('reason') == 'no_marker' for r in rows if r['task'] == task)
            cell['not_in_text'] = sum('not-in-text' in r['tags'] for r in rows if r['task'] == task)
        tasks[task] = cell
    timed = [r for r in rows if r.get('ms') is not None]
    tokens = sum((r.get('usage') or {}).get('completion_tokens', 0) for r in timed)
    seconds = sum(r['ms'] for r in timed) / 1000
    return dict(candidate=run['candidate'], backend=(run.get('execution') or {}).get('backend'), host=run['host'],
                model_bytes=run['model']['bytes'], quantization=run['model']['quantization'],
                complete={(r['task'], r['id']) for r in rows} == expected and not run['errors'],
                stale=sorted(t for t in suite if run['suite'][t]['sha256'] != current[t]), errors=run['errors'], tasks=tasks,
                passed=sum(r['passed'] for r in rows), total=len(expected),
                median_ms=statistics.median(r['ms'] for r in timed) if timed else None, p95_ms=p95([r['ms'] for r in timed]) if timed else None,
                output_tokens_per_s=tokens / seconds if seconds else None, prepare_ms=run.get('prepare_ms'),
                max_rss_bytes=run['memory']['max_rss_bytes'], max_gpu_allocated_bytes=run['memory']['max_gpu_allocated_bytes'])


def report(args):
    directory, suite = Path(args.run), load_suite()
    results = [summarize(d, suite) for d in sorted(directory.iterdir()) if (d/'run.json').is_file()]
    if not results:
        raise ValueError(f'{directory}: no candidate runs')
    if args.json:
        print(json.dumps(results, indent=2, ensure_ascii=False))
        return
    gib = lambda b: '–' if b is None else f'{b / 2**30:.1f}'
    print('geisten mini benchmark (passed / cases, de · en)\n')
    print(f'{"model":<18}{"backend":<10}' + ''.join(f'{t:<14}' for t in TASKS) + f'{"total":<9}{"p50 s":<7}{"p95 s":<7}{"tok/s":<7}{"RSS":<6}GPU GiB')
    for r in results:
        cells = ''.join(f'{c["de"]["passed"]:>2}·{c["en"]["passed"]:<2}/{c["de"]["total"]:<7}' for c in r['tasks'].values())
        secs = lambda ms: '–' if ms is None else f'{ms / 1000:.1f}'
        rate = '–' if r['output_tokens_per_s'] is None else f'{r["output_tokens_per_s"]:.0f}'
        print(f'{r["candidate"]:<18}{(r["backend"] or "?"):<10}{cells}{r["passed"]:>3}/{r["total"]:<5}'
              f'{secs(r["median_ms"]):<7}{secs(r["p95_ms"]):<7}{rate:<7}{gib(r["max_rss_bytes"]):<6}{gib(r["max_gpu_allocated_bytes"])}')
        c = r['tasks']['context']
        notes = [f'{c["no_marker"]} of {c["not_in_text"]} unanswerable context questions answered without the marker (read them: invented or paraphrased refusal)']
        notes += ['INCOMPLETE'] if not r['complete'] else []
        notes += [f'STALE: suite changed since the run ({", ".join(r["stale"])})'] if r['stale'] else []
        notes += [f'error: {e}' for e in r['errors']]
        print(''.join(f'  {n}\n' for n in notes), end='')
    hosts = {json.dumps(r['host'], sort_keys=True) for r in results}
    print('\nLatency and memory compare only within one host and backend. RSS leaves out GPU buffers.'
          + ('' if len(hosts) == 1 else ' These runs come from different hosts.'))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    r = sub.add_parser('run')
    r.add_argument('--candidate', action='append', required=True)
    r.add_argument('--backend', choices=('cpu', 'gpu'), required=True)
    r.add_argument('--output', required=True)
    base = Path.home()/('Library/Application Support' if sys.platform == 'darwin' else '.local/share')
    home = next((base/n for n in ('geisten', 'Geist' if sys.platform == 'darwin' else 'geist') if (base/n).is_dir()), base/'geisten')
    r.add_argument('--model-dir', default=os.environ.get('GEIST_HOME', str(home)) + '/models')
    p = sub.add_parser('report')
    p.add_argument('run')
    p.add_argument('--json', action='store_true')
    args = parser.parse_args()
    try:
        run(args) if args.command == 'run' else report(args)
    except (ValueError, OSError) as error:
        sys.exit(f'bench: {error}')


if __name__ == '__main__':
    main()
