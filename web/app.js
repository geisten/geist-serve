'use strict';
const $ = id => document.getElementById(id);
const token = location.hash.slice(1);
// The capability stays in this page's memory/fragment, never localStorage.
const cards = new Map();
let state = null, controller = null, requesting = false, polling = false;
let lastServerMessage = '', localMessage = false;
let showAllModels = false;
let stopped = false, timer;
let tasks = [], selectedTask = null;
let qualityRecords = [];
function qualityFor(model, task = selectedTask) {
  return qualityRecords.find(r => r.model_sha256 === model?.sha256 && r.task === task?.id &&
    r.task_version === task?.version && r.language === $('language-choice').value &&
    r.device === state?.hardware.device);
}
function allowed(model, task = selectedTask) { return qualityFor(model, task)?.quality === 'passed' || $('experimental').checked; }
const bytes = n => n < 1e9 ? `${Math.round(n / 1e6)} MB` : `${(n / 1e9).toFixed(2)} GB`;
const gib = n => `${(n / 2 ** 30).toFixed(1)} GiB`;

async function api(path, body, signal) {
  const response = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST', cache: 'no-store', signal,
    headers: { Authorization: `Bearer ${token}`, ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
    ...(body === undefined ? {} : { body: JSON.stringify(body) })
  });
  if (!response.ok) {
    let error = `Request failed (${response.status}).`;
    try { error = (await response.json()).error || error; } catch { /* retain status */ }
    throw new Error(error);
  }
  return response;
}

function message(text, local = true) { $('notice').textContent = text; localMessage = local; }
function buttonStates() {
  const ready = selectedTask && !selectedTask.url && state?.ready && !state?.busy && !requesting && !controller && allowed(state?.models.find(m => m.id === state.active_id));
  $('run').disabled = !ready;
  $('benchmark').disabled = !state?.ready || state?.busy || requesting || !!controller ||
    !allowed(state?.models.find(m => m.id === state.active_id), tasks.find(t => t.id === 'freeform'));
  $('unload').disabled = !state?.ready || state?.busy || requesting || !!controller;
  $('stop').hidden = !controller;
  $('prompt').readOnly = !!controller;
  $('task-choice').disabled = !!controller || !tasks.length;
  $('language-choice').disabled = !!controller;
  $('experimental').disabled = !!controller;
  $('use-example').disabled = !!controller || !selectedTask || !!selectedTask.url;
}

function modelCard(model) {
  let card = cards.get(model.id);
  if (!card) {
    card = document.createElement('article'); card.className = 'model'; card.dataset.id = model.id;
    // This template is static. Model metadata and output always use textContent.
    card.innerHTML = '<h3></h3><span class="fit"></span><p class="specs"></p><p class="reason"></p><p class="performance"></p><p class="quality"></p><button type="button"></button>';
    card.querySelector('button').addEventListener('click', () => choose(model.id));
    cards.set(model.id, card); $('models').append(card);
  }
  const active = state.active_id === model.id && state.ready;
  card.hidden = !showAllModels && state.models.indexOf(model) >= 2 && !active;
  const evidence = qualityFor(model);
  const fitValue = model.resource_fit === 2 ? 2 : evidence?.quality === 'passed' ? model.resource_fit : 1;
  const badge = fitValue === 2 ? 'Unavailable' : evidence?.quality !== 'passed' ? 'Experimental' : ['Recommended', 'Conditional'][fitValue];
  card.className = `model${active ? ' active' : ''}${model.fit === 2 ? ' unavailable' : ''}`;
  card.querySelector('h3').textContent = model.name;
  const fit = card.querySelector('.fit'); fit.textContent = badge;
  fit.className = `fit ${['', 'conditional', 'unavailable'][fitValue]}`;
  card.querySelector('.specs').textContent = `${bytes(model.bytes)} download · ${model.ram_gib} GiB RAM guidance`;
  card.querySelector('.reason').textContent = `Resources: ${model.reason}`;
  card.querySelector('.quality').textContent = evidence ? `Task quality: ${evidence.quality} · ${evidence.cases} test cases · ${evidence.language.toUpperCase()}. ${evidence.human_complete ? 'Human sample complete.' : 'Human assessment pending.'}` : 'Task quality: unverified for this task, language and device.';
  card.querySelector('.performance').textContent = model.measured_tps > 0
    ? `Measured here: ${model.measured_tps.toFixed(1)} tokens/s · ${model.measured_tokens} tokens · this session`
    : model.performance;
  const button = card.querySelector('button');
  button.disabled = model.fit === 2 || state.busy || requesting || !!controller || active || !allowed(model);
  button.textContent = active ? 'Running here' : model.installed ? 'Use this model' : model.partial ? 'Resume download' : `Download · ${bytes(model.bytes)}`;
  button.setAttribute('aria-label', `${button.textContent}: ${model.name}. ${model.reason}`);
}

function render(next) {
  state = next;
  $('device-name').textContent = next.hardware.name || 'Hardware information unavailable';
  $('device-specs').textContent = `${gib(next.hardware.ram)} RAM · ${next.hardware.cores} compute cores · ${next.hardware.arch}`;
  $('disk-space').textContent = next.hardware.disk_known ? `${bytes(next.hardware.disk)} disk space available` : 'Disk space could not be read';
  $('runtime-state').textContent = next.phase ? next.phase === 'verifying' ? 'Verifying model' : 'Downloading model' : next.loading ? 'Loading model' : next.ready ? 'Ready on this device' : 'Choose a model';
  $('active-model').textContent = next.active ? next.active : 'Download a suggested model to begin.';
  next.models.forEach(modelCard);
  $('more-models').textContent = showAllModels ? 'Show fewer models' : `Show ${next.models.filter((m, i) => i >= 2 && m.id !== next.active_id).length} more models`;
  $('job').hidden = !next.phase;
  if (next.phase) {
    const model = next.models.find(m => m.id === next.job_model);
    const verifying = next.phase === 'verifying';
    $('job-title').textContent = `${verifying ? 'Verifying' : 'Downloading'} ${model?.name || 'model'}`;
    if (verifying) $('download-progress').removeAttribute('value');
    else $('download-progress').value = Math.min(1, next.received / (model?.bytes || 1));
    $('job-detail').textContent = verifying ? 'Checking the complete file before it can run.' : `${bytes(next.received)} of ${bytes(model?.bytes || 0)} · partial downloads can be resumed`;
  }
  if (!controller && !requesting && (!localMessage || next.message !== lastServerMessage)) message(next.message || '', false);
  lastServerMessage = next.message;
  buttonStates();
}

async function poll() {
  if (polling || stopped) return;
  polling = true;
  try { const next = await (await api('/app/status')).json(); if (!stopped) render(next); }
  catch (error) { if (!stopped) { message(error.message); state = null; buttonStates(); } }
  finally { polling = false; }
}

async function choose(id) {
  if (requesting || controller) return;
  const model = state?.models.find(m => m.id === id);
  if (!model || model.fit === 2 || !allowed(model)) return;
  requesting = true; message('', false); buttonStates(); state.models.forEach(modelCard);
  try { await api(model.installed ? '/app/select' : '/app/download', { id }); }
  catch (error) { message(error.message); }
  finally { requesting = false; await poll(); }
}

function metric(id, value, unit) {
  const target = $(id); target.replaceChildren(document.createTextNode(value));
  const label = document.createElement('small'); label.textContent = unit; target.append(label);
}

async function run(prompt, benchmark = false) {
  const task = benchmark ? tasks.find(t => t.id === 'freeform') : selectedTask;
  if (controller || requesting || !state?.ready || !task || !allowed(state.models.find(m => m.id === state.active_id), task)) return;
  const activeController = new AbortController(); controller = activeController;
  buttonStates(); state.models.forEach(modelCard);
  $('output').textContent = ''; $('output').classList.remove('empty'); $('copy').disabled = true; $('copy').textContent = 'Copy';
  for (const [id, unit] of [['speed', 'tokens/s'], ['first-token', 'seconds'], ['elapsed', 'seconds']]) metric(id, '—', unit);
  $('measurement-note').textContent = benchmark ? 'Short local test running. Results apply to this model and this workload.' : 'Running on your device…';
  message('Waiting for the first text…');
  const start = performance.now(); let first = null, done = false, reader;
  let output = '', pending = '', limited = false;
  function event(line) {
    if (!line.trim()) return;
    const item = JSON.parse(line);
    if (item.error) throw new Error(typeof item.error === 'string' ? item.error : 'The model returned an error.');
    if (item.response) {
      if (first === null) { first = (performance.now() - start) / 1000; metric('first-token', first.toFixed(2), 'seconds'); message('Generating locally…'); }
      output += item.response;
      if (output.length > 131072) throw new Error('Output exceeded the display memory limit.');
      $('output').textContent = output;
    }
    if (item.done) {
      done = true; limited = item.limited === true;
      const seconds = item.eval_duration / 1e9;
      const rate = seconds > 0 && item.eval_count > 0 ? item.eval_count / seconds : null;
      metric('speed', rate === null ? '—' : rate.toFixed(1), 'tokens/s');
      metric('elapsed', ((performance.now() - start) / 1000).toFixed(2), 'seconds');
      $('measurement-note').textContent = `${item.eval_count || 0} generated tokens. Speed uses geistd's generation time, including token streaming; first text and total include the local connection and prompt processing. ${benchmark ? 'A short sample, not a general benchmark.' : ''}`;
    }
  }
  try {
    const response = await api('/app/generate', { prompt, benchmark, language: $('language-choice').value, experimental: $('experimental').checked, task: benchmark ? 'freeform' : selectedTask.id, task_version: benchmark ? '1.0.0' : selectedTask.version }, activeController.signal);
    if (!response.body) throw new Error('This browser does not support streamed responses.');
    reader = response.body.getReader(); const decoder = new TextDecoder();
    for (;;) {
      const chunk = await reader.read();
      pending += decoder.decode(chunk.value, { stream: !chunk.done });
      if (pending.length > 131072) throw new Error('The response exceeded the stream buffer limit.');
      let newline;
      while ((newline = pending.indexOf('\n')) !== -1) { const line = pending.slice(0, newline); pending = pending.slice(newline + 1); event(line); }
      if (chunk.done) { if (pending.trim()) event(pending); break; }
    }
    if (!done) throw new Error('The connection ended before the model completed its response.');
    message(limited ? 'Output limit reached. The result may be incomplete; try a shorter input.' : output ? 'Complete. Your result stays on this device.' : 'The model completed without producing text. Try a different prompt.');
  } catch (error) {
    activeController.abort();
    message(error.name === 'AbortError' ? 'Stopped. Partial output is kept here.' : error.message);
    $('measurement-note').textContent = 'Run incomplete. No final generation speed is reported.';
    metric('speed', '—', 'tokens/s');
    metric('elapsed', ((performance.now() - start) / 1000).toFixed(2), 'seconds');
  } finally {
    if (reader) { try { await reader.cancel(); } catch { /* connection already closed */ } }
    controller = null; $('copy').disabled = !output;
    buttonStates(); if (state) state.models.forEach(modelCard);
    if (stopped) message('Geist is stopping. You can close this tab.');
  }
}

$('task-form').addEventListener('submit', event => { event.preventDefault(); run($('prompt').value.trim()); });
$('prompt').addEventListener('keydown', event => { if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); $('task-form').requestSubmit(); } });
function chooseTask(id) {
  selectedTask = tasks.find(t => t.id === id);
  if (!selectedTask) return;
  $('task-description').textContent = selectedTask.description;
  $('task-evidence').textContent = `${selectedTask.title} · v${selectedTask.version}. Evidence is specific to the model, language and device. ${id === 'freeform' ? 'Tests cover simple chats only, not arbitrary questions.' : ''}`;
  $('task-link').hidden = !selectedTask.url;
  if (selectedTask.url) $('task-link').href = selectedTask.url;
  $('task-form').hidden = !!selectedTask.url;
  $('prompt').maxLength = selectedTask.input_limit || 12000;
  $('prompt').value = '';
  $('prompt').placeholder = selectedTask.example || 'Write your input here…';
  buttonStates();
  if (state) state.models.forEach(modelCard);
}
$('language-choice').addEventListener('change', () => { buttonStates(); if (state) render(state); });
$('experimental').addEventListener('change', () => { buttonStates(); if (state) render(state); });
$('task-choice').addEventListener('change', () => chooseTask($('task-choice').value));
$('use-example').addEventListener('click', () => {
  if (!controller && selectedTask) { $('prompt').value = selectedTask.example; $('prompt').focus(); }
});
async function loadTasks() {
  const catalog = await (await api('/app/tasks')).json();
  tasks = catalog.tasks;
  qualityRecords = catalog.quality_records || [];
  $('task-choice').replaceChildren(...tasks.map(t => {
    const option = document.createElement('option'); option.value = t.id; option.textContent = t.title; return option;
  }));
  chooseTask(tasks[0].id);
}
$('stop').addEventListener('click', () => controller?.abort());
$('benchmark').addEventListener('click', () => run('Explain in a short paragraph how a seed grows into a plant.', true));
$('cancel-download').addEventListener('click', async () => { try { await api('/app/cancel', {}); message('Cancelling…'); } catch (error) { message(error.message); } });
$('unload').addEventListener('click', async () => { try { await api('/app/stop', {}); await poll(); } catch (error) { message(error.message); } });
$('quit').addEventListener('click', async () => {
  try {
    await api('/app/quit', {}); stopped = true; clearInterval(timer); controller?.abort();
    state = null; buttonStates(); $('runtime-state').textContent = 'Stopping';
    document.querySelectorAll('#models button, #quit').forEach(button => { button.disabled = true; });
    message('Geist is stopping. You can close this tab.');
  } catch (error) { message(error.message); }
});
$('copy').addEventListener('click', async () => { try { await navigator.clipboard.writeText($('output').textContent); $('copy').textContent = 'Copied'; } catch { message('Copy is unavailable here. Select the result and copy it manually.'); } });
document.querySelector('.brand').addEventListener('click', event => { event.preventDefault(); window.scrollTo({ top: 0 }); });
document.querySelector('.skip').addEventListener('click', event => { event.preventDefault(); $('prompt').focus(); $('prompt').scrollIntoView({block: 'center'}); });
$('more-models').addEventListener('click', () => {
  showAllModels = !showAllModels; $('more-models').setAttribute('aria-expanded', String(showAllModels));
  if (state) render(state);
});
window.addEventListener('beforeunload', () => controller?.abort());
$('output').classList.add('empty');
if (!/^[a-f0-9]{64}$/.test(token)) message('Open Geist using the private link from the app or Pi launcher. The link authorizes only this local session.');
else { loadTasks().catch(error => message(error.message)); poll(); timer = setInterval(poll, 1800); }
