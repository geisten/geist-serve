// chrome_coverage.mjs: run tests/desktop/chat_checks.js in headless Chrome against a
// real geist-app and report line coverage of the web UI (V8 precise coverage).
//
//   node tests/desktop/chrome_coverage.mjs [MIN_LINE_PERCENT]
//
// Needs Chrome, ./geist-app, ./geistd and GEIST_TEST_MODEL (the small reference
// model). Writes build/coverage/web.json and prints a per-file table. The native
// shells (Mac, Linux) run the same checks; this adds the coverage numbers.
import {spawn} from 'node:child_process';
import {mkdtempSync, mkdirSync, linkSync, readFileSync, writeFileSync, rmSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const min = Number(process.argv[2] || 0);
const chrome = process.env.CHROME || ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'].find(existsSync);
const model = process.env.GEIST_TEST_MODEL;
if (!chrome || !model || !existsSync(model)) {
  console.log('web coverage: SKIPPED (needs Chrome and GEIST_TEST_MODEL)');
  process.exit(0);
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const home = mkdtempSync(join(tmpdir(), 'geist-web-cov-'));
const children = [];
const cleanup = () => { for (const c of children) c.kill('SIGTERM'); };
process.on('exit', () => { cleanup(); rmSync(home, {recursive: true, force: true}); });

// The service, with the reference model as a catalog download (hard link, as bench.py does).
const catalog = JSON.parse(readFileSync(join(ROOT, 'models/catalog.json'), 'utf8'));
const entry = catalog.models.find(m => m.id === 'smollm2-360m');
mkdirSync(join(home, 'models'));
linkSync(model, join(home, 'models', entry.file));
const app = spawn(join(ROOT, 'geist-app'), ['--port', '0', '--home', home, '--daemon', join(ROOT, 'geistd')], {stdio: ['ignore', 'pipe', 'inherit']});
children.push(app);
const port = await new Promise((ok, fail) => {
  let text = '';
  app.stdout.on('data', d => { text += d; const m = text.match(/http:\/\/127\.0\.0\.1:(\d+)/); if (m) ok(Number(m[1])); });
  app.on('exit', code => fail(new Error(`geist-app exited ${code}`)));
});
const key = readFileSync(join(home, 'api-key'), 'utf8').trim();

const profile = mkdtempSync(join(tmpdir(), 'geist-web-chrome-'));
const browser = spawn(chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--window-size=1280,900', 'about:blank'], {stdio: ['ignore', 'ignore', 'pipe']});
children.push(browser);
const wsBrowser = await new Promise(ok => {
  let text = '';
  browser.stderr.on('data', d => { text += d; const m = text.match(/DevTools listening on (ws:\/\/\S+)/); if (m) ok(m[1]); });
});
const debugPort = new URL(wsBrowser).port;
const target = (await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json()).find(t => t.type === 'page');
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r, {once: true}));
let sequence = 0;
const pending = new Map();
ws.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) { pending.get(message.id)(message); pending.delete(message.id); }
});
const send = (method, params = {}) => new Promise((ok, fail) => {
  const id = ++sequence;
  pending.set(id, m => m.error ? fail(new Error(`${method}: ${m.error.message}`)) : ok(m.result));
  ws.send(JSON.stringify({id, method, params}));
});
const evaluate = async expression => (await send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true})).result?.value;
const waitFor = async (expression, seconds = 60) => {
  for (let i = 0; i < seconds * 4; i++) { if (await evaluate(expression) === true) return; await sleep(250); }
  throw new Error(`timed out: ${expression}`);
};

// A headless window never has focus; focus/blur events still have to fire.
await send('Emulation.setFocusEmulationEnabled', {enabled: true});
await send('Profiler.enable');
await send('Profiler.startPreciseCoverage', {callCount: true, detailed: true});
await send('Page.navigate', {url: `http://127.0.0.1:${port}/#${key}`});
await waitFor("typeof state !== 'undefined' && state?.models.length > 0 && document.querySelectorAll('.model').length === state.models.length");
await evaluate("showPage('models-page'); document.querySelector('[data-id=\"smollm2-360m\"] .model-name').click(); true");
await waitFor("state?.ready === true && !document.getElementById('workspace').hidden", 120);
await evaluate(readFileSync(join(ROOT, 'tests/desktop/chat_checks.js'), 'utf8') + '\ntrue');
await waitFor('window.chatChecksDone || !!window.chatChecksError', 600);
const error = await evaluate("window.chatChecksError && `${window.chatChecksError} (stage: ${window.chatChecksStage})`");
const {result} = await send('Profiler.takePreciseCoverage');

// V8 block ranges → line coverage: a line counts as executed when the innermost
// range covering its first non-blank character ran at least once.
const files = {};
for (const script of result) {
  const name = new URL(script.url || 'about:blank').pathname.replace(/^\//, '');
  if (!/^(app|i18n|markdown)\.js$/.test(name)) continue;
  const source = readFileSync(join(ROOT, 'web', name), 'utf8');
  const ranges = script.functions.flatMap(f => f.ranges).sort((a, b) => a.startOffset - b.startOffset || b.endOffset - a.endOffset);
  let offset = 0, hit = 0, total = 0;
  const missed = [];
  source.split('\n').forEach((line, number) => {
    const at = offset + line.search(/\S/);
    offset += line.length + 1;
    if (!line.trim() || /^\s*(\/\/|\*|\/\*|[}\])]+[;,)]*$)/.test(line)) return;
    let count = 1;
    for (const r of ranges) if (r.startOffset <= at && at < r.endOffset) count = r.count;
    total++;
    if (count > 0) hit++; else missed.push(number + 1);
  });
  files[name] = {hit, total, missed};
}
mkdirSync(join(ROOT, 'build/coverage'), {recursive: true});
writeFileSync(join(ROOT, 'build/coverage/web.json'), JSON.stringify(files, null, 1));
let hit = 0, total = 0;
for (const [name, f] of Object.entries(files)) {
  console.log(`${(100 * f.hit / f.total).toFixed(1).padStart(6)}%  ${String(f.total - f.hit).padStart(5)} missed  web/${name}`);
  hit += f.hit; total += f.total;
}
const percent = total ? 100 * hit / total : 0;
console.log(`${percent.toFixed(1).padStart(6)}%  ${String(total - hit).padStart(5)} missed  TOTAL (web)`);
if (error) { console.error(`web checks failed: ${error}\n${String(await evaluate('window.chatChecksStack')).split('\n').slice(0, 4).join('\n')}`); process.exit(1); }
if (!total) { console.error('web coverage: no web scripts were loaded'); process.exit(1); }
if (percent < min) { console.error(`web coverage: below ${min}%`); process.exit(1); }
process.exit(0);
