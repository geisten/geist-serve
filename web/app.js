'use strict';
const $ = id => document.getElementById(id);
const token = location.hash.slice(1);
// The capability stays in this page's memory/fragment, never localStorage.
const cards = new Map();
let state = null, controller = null, requesting = false, polling = false;
let lastServerMessage = '', localMessage = false;
let manualChoice = null, languageInitialized = false, customPreviewAccepted = false;
let stopped = false, timer;
let tasks = [], selectedTask = null;
let qualityRecords = [];
let connectionTesting = false;
// Page memory only. Never store prompts, answers or conversation in browser storage.
let conversation = [], followLatest = true;
function chatLayout() {
  document.body.classList.toggle('chat-page', !$('test-page').hidden && !$('workspace').hidden);
  resizeComposer();
}
function resizeComposer() {
  const input = $('prompt');
  const previousScroll = input.scrollTop;
  input.style.height = 'auto';
  input.style.height = `${input.scrollHeight}px`;
  input.scrollTop = previousScroll;
}
function scrollLatest(force = false) {
  if (force) followLatest = true;
  if (followLatest) $('transcript').scrollTop = $('transcript').scrollHeight;
  $('latest').hidden = followLatest;
}
$('transcript').addEventListener('scroll', () => {
  const view = $('transcript');
  followLatest = view.scrollHeight - view.clientHeight - view.scrollTop < 48;
  $('latest').hidden = followLatest;
});
$('latest').addEventListener('click', () => scrollLatest(true));
window.addEventListener('resize', () => { resizeComposer(); scrollLatest(); });
function addTurn(prompt) {
  $('chat-empty').hidden = true; $('result').hidden = false;
  // Stable ids identify the latest response for automation/accessibility.
  $('output')?.removeAttribute('id'); $('copy')?.removeAttribute('id');
  const user = document.createElement('article'); user.className = 'chat-message user';
  const userText = document.createElement('div'); userText.className = 'message-text'; userText.textContent = prompt;
  user.setAttribute('aria-label', t('You')); user.append(userText);
  const answer = document.createElement('article'); answer.className = 'chat-message assistant';
  const label = document.createElement('div'); label.className = 'message-label'; label.textContent = `Geist · ${state.models.find(model => model.id === state.active_id)?.name || state.active}`;
  const output = document.createElement('div'); output.id = 'output'; output.className = 'message-text';
  const status = document.createElement('p'); status.className = 'message-status'; status.textContent = t('Waiting for the first text…');
  const actions = document.createElement('div'); actions.className = 'message-actions';
  const copy = document.createElement('button'); copy.id = 'copy'; copy.type = 'button'; copy.className = 'text-button'; copy.disabled = true; copy.textContent = t('Copy'); copy.dataset.label = 'Copy';
  copy.addEventListener('click', async () => {
    try { await copyText(output.textContent); copy.textContent = t('Copied'); }
    catch { message('Copy is unavailable here. Select the result and copy it manually.'); }
  });
  actions.append(copy); answer.append(label, output, status, actions);
  $('result').append(user, answer); scrollLatest(true);
  return {output, status, actions, copy};
}
function qualityFor(model, task = selectedTask) {
  return qualityRecords.find(r => r.model_sha256 === model?.sha256 && r.task === task?.id &&
    r.task_version === task?.version && r.language === $('language-choice').value &&
    r.device === state?.hardware.device);
}
function previewAccepted(model) { return model?.preview_accepted === true || (state?.active_id === 'custom' && customPreviewAccepted); }
function allowed(model, task = selectedTask) {
  // Freeform now includes session history; old single-task ratings do not certify it.
  return previewAccepted(model) || (task?.id !== 'freeform' && qualityFor(model, task)?.quality === 'passed');
}
const bytes = n => n < 1e9 ? `${Math.round(n / 1e6)} MB` : `${(n / 1e9).toFixed(2)} GB`;
const gib = n => `${(n / 2 ** 30).toFixed(1)} GiB`;

// A rolling observation from this window, never an advertised network speed.
// Resuming/reconnecting starts a new sample: saved bytes do not inflate it.
const transfer = {id: '', samples: []};
function downloadEstimate(id, received, total, now = performance.now()) {
  if (transfer.id !== id || received < (transfer.samples.at(-1)?.bytes || 0)) {
    transfer.id = id; transfer.samples = [];
  }
  transfer.samples.push({time: now, bytes: received});
  while (transfer.samples.length > 2 && transfer.samples[1].time < now - 10000) transfer.samples.shift();
  const first = transfer.samples[0], elapsed = (now - first.time) / 1000;
  if (elapsed < 2) return t('Measuring speed…');
  const speed = (received - first.bytes) / elapsed;
  if (speed <= 0) return t('Waiting for data…');
  const rate = speed < 1e6 ? `${Math.round(speed / 1000)} KB/s` : `${(speed / 1e6).toFixed(1)} MB/s`;
  const remaining = Math.max(0, (total - received) / speed);
  return `${rate} · ${t(remaining < 60 ? 'Less than a minute left' : `About ${Math.ceil(remaining / 60)} min left`)}`;
}

async function api(path, body, signal) {
  const response = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST', cache: 'no-store', signal,
    headers: { Authorization: `Bearer ${token}`, ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
    ...(body === undefined ? {} : { body: JSON.stringify(body) })
  });
  if (!response.ok) {
    let error = `Request failed (${response.status}).`;
    try { const detail = (await response.json()).error; error = typeof detail === 'string' ? detail : detail?.message || error; } catch { /* retain status */ }
    throw new Error(error);
  }
  return response;
}

function message(text, local = true) { $('notice').textContent = t(text); localMessage = local; }
function buttonStates() {
  $('test-connection').disabled = !state?.ready || state?.busy || connectionTesting || requesting || !!controller;
  $('copy-connection').disabled = !state?.ready;
  $('connection-endpoint').textContent = t(`${location.origin}/v1`);
  $('connection-model').textContent = t(state?.active_id || 'Choose a model');
  const ready = selectedTask && !selectedTask.url && state?.ready && !state?.busy && !requesting && !controller && allowed(state?.models.find(m => m.id === state.active_id));
  $('run').disabled = !ready || !$('prompt').value.trim();
  $('run').hidden = !!controller;
  $('new-chat').disabled = !!controller;
  $('benchmark').disabled = !state?.ready || state?.busy || requesting || !!controller ||
    !allowed(state?.models.find(m => m.id === state.active_id), tasks.find(t => t.id === 'freeform'));
  $('unload').disabled = !state?.ready || state?.busy || requesting || !!controller;
  $('stop').hidden = !controller;
  $('language-choice').disabled = !!controller;
  $('setup-start').disabled = requesting || !!controller || !state || state.busy || state.loading || !setupCandidate().eligible;
}

function modelCard(model) {
  let card = cards.get(model.id);
  if (!card) {
    card = document.createElement('article'); card.className = 'model'; card.dataset.id = model.id;
    // Static template only; metadata and generated output always use textContent.
    card.innerHTML = '<h3></h3><span class="fit"></span><p class="specs"></p><span></span><details><summary></summary><p class="reason"></p><p class="performance"></p><p class="quality"></p></details><button type="button"></button><button class="remove" type="button"></button>';
    card.querySelector('button').addEventListener('click', () => choose(model.id));
    card.querySelector('.remove').addEventListener('click', () => removeModel(model.id));
    cards.set(model.id, card); $('models').append(card);
  }
  const active = state.active_id === model.id && state.ready;
  const evidence = qualityFor(model);
  card.className = `model${active ? ' active' : ''}${model.resource_fit === 2 ? ' unavailable' : ''}`;
  card.querySelector('h3').textContent = model.name;
  card.querySelector('.fit').textContent = t(active ? 'Running here' : model.resource_fit === 2 ? 'Unavailable' : model.id === state.recommendation.id ? 'Selected for setup' : model.resource_fit === 1 ? 'Conditional' : 'Available');
  card.querySelector('.specs').textContent = `${bytes(model.bytes)} · ${model.ram_gib} GiB RAM`;
  card.querySelector('summary').textContent = t('Details');
  card.querySelector('.reason').textContent = t(model.reason);
  card.querySelector('.performance').textContent = t(model.measured_tps > 0 ? `Measured here: ${model.measured_tps.toFixed(1)} tokens/s · ${model.measured_tokens} tokens · this session` : model.performance);
  card.querySelector('.quality').textContent = t(evidence ? `Task quality: ${evidence.quality} · ${evidence.cases} test cases · ${evidence.language.toUpperCase()}. ${evidence.human_complete ? 'Human sample complete.' : 'Human assessment pending.'}` : 'Task quality: unverified for this task, language and device.');
  const button = card.querySelector('button');
  button.textContent = t(active ? 'Running here' : 'Choose');
  button.disabled = model.resource_fit === 2 || state.busy || state.loading || requesting || !!controller || (active && allowed(model));
  button.setAttribute('aria-label', `${button.textContent}: ${model.name}`);
  const remove = card.querySelector('.remove');
  remove.textContent = t('Remove download');
  remove.hidden = !model.installed && !model.partial;
  remove.disabled = (state.active_id === model.id && (state.ready || state.loading)) || state.busy || requesting || !!controller;
  remove.setAttribute('aria-label', `${t('Remove download')}: ${model.name}`);
}

function setupCandidate() {
  if (!state) return {eligible: false};
  const id = manualChoice || (state.active_id === 'custom' ? 'custom' : state.recommendation.id);
  if (id === 'custom') return {id, name: state.active, eligible: state.ready, installed: true};
  const model = state.models.find(m => m.id === id);
  return {...model, eligible: !!model && (manualChoice ? model.resource_fit !== 2 : state.recommendation.eligible)};
}

function render(next) {
  state = next;
  if (!languageInitialized) {
    $('language-choice').value = next.answer_language || interfaceLanguage;
    languageInitialized = true;
  }
  const candidate = setupCandidate();
  const working = !!next.phase || next.loading;
  const active = next.models.find(m => m.id === next.active_id);
  const usable = next.ready && allowed(active) && (!manualChoice || manualChoice === next.active_id);
  const previouslyHidden = $('workspace').hidden;
  $('workspace').hidden = working || !usable;
  $('setup').hidden = working || usable;
  $('job').hidden = !working;
  if (usable && !working) manualChoice = null;
  if (previouslyHidden && !$('workspace').hidden && !$('test-page').hidden) $('prompt').focus();
  $('setup-detail').textContent = candidate.id ? `${candidate.name} · ${t(candidate.installed ? 'Already on this computer' : 'Download')} ${candidate.installed ? '' : bytes(Math.max(0, candidate.bytes - (candidate.partial || 0)))}` : t('No suitable model available right now.');
  $('setup-reason').textContent = manualChoice ? t(candidate.reason) : t(next.recommendation.source === 'fallback' || !next.recommendation.eligible ? next.recommendation.reason : '');
  $('preview-notice').hidden = !candidate.id || previewAccepted(candidate);
  $('setup-start').textContent = t(next.ready && candidate.id === next.active_id ? 'Try preview' : candidate.installed ? 'Start model' : candidate.partial ? 'Resume download' : 'Set up and start');
  $('device-name').textContent = t(next.hardware.name || 'Hardware information unavailable');
  $('device-specs').textContent = `${gib(next.hardware.ram)} RAM · ${next.hardware.cores} ${t('compute cores')} · ${next.hardware.arch}`;
  $('disk-space').textContent = t(next.hardware.disk_known ? `${bytes(next.hardware.disk)} disk space available` : 'Disk space could not be read');
  $('active-model').textContent = next.active || candidate.name || t('Choose a model');
  $('recommendation-reason').textContent = t(next.recommendation.reason);
  $('runtime-state').textContent = t(next.ready ? 'Ready on this device' : 'Choose a model');
  next.models.forEach(modelCard);
  if (!working) { transfer.id = ''; transfer.samples = []; }
  if (working) {
    const model = next.models.find(m => m.id === next.job_model);
    const verifying = next.phase === 'verifying';
    $('job-title').textContent = t(next.loading ? 'Loading model' : verifying ? 'Verifying model' : 'Downloading model');
    if (verifying || next.loading) $('download-progress').removeAttribute('value');
    else $('download-progress').value = Math.min(1, next.received / (model?.bytes || 1));
    const downloading = !verifying && !next.loading;
    $('job-percent').textContent = downloading ? `${Math.min(100, Math.floor(next.received / (model?.bytes || 1) * 100))}%` : '';
    $('job-speed').textContent = downloading ? downloadEstimate(next.job_model, next.received, model?.bytes || 0) : '';
    $('job-caption').hidden = !downloading;
    $('job-detail').textContent = t(next.loading ? 'Starting the local service…' : verifying ? 'Checking the complete file before it can run.' : `${bytes(next.received)} of ${bytes(model?.bytes || 0)}`);
    $('cancel-download').hidden = next.loading;
  }
  if (!controller && !requesting && (!localMessage || next.message !== lastServerMessage)) message(working ? '' : next.message || '', false);
  lastServerMessage = next.message;
  buttonStates(); chatLayout();
}

async function poll() {
  if (polling || stopped) return;
  polling = true;
  try { const next = await (await api('/app/status')).json(); if (!stopped) render(next); }
  catch (error) { if (!stopped) { message('Service unavailable. Reopen Geist to reconnect.'); state = null; buttonStates(); } }
  finally { polling = false; }
}

function choose(id) {
  if (requesting || controller || state?.busy || state?.loading) return;
  manualChoice = id;
  render(state);
  showPage('test-page');
}
$('setup-start').addEventListener('click', async () => {
  const candidate = setupCandidate();
  if (!candidate.eligible || requesting || controller || state?.busy || state?.loading) return;
  const manual = !!manualChoice;
  requesting = true; buttonStates(); message('', false);
  try {
    // This button is next to the explicit preview notice. Store consent per
    // immutable model hash; generation still sends its own experimental flag.
    if (candidate.id === 'custom') customPreviewAccepted = true;
    else if (!previewAccepted(candidate)) await api('/app/preview', {id: candidate.id, experimental: true});
    if (!state.ready || state.active_id !== candidate.id) {
      await api(manual ? candidate.installed ? '/app/select' : '/app/download' : '/app/setup', {id: candidate.id});
    }
    manualChoice = null;
  } catch (error) { message(error.message); }
  finally { requesting = false; await poll(); }
});

function metric(id, value, unit) {
  const target = $(id); target.replaceChildren(document.createTextNode(value));
  const label = document.createElement('small'); label.textContent = t(unit); target.append(label);
}

async function run(prompt, benchmark = false) {
  prompt = prompt.trim();
  const task = tasks.find(t => t.id === 'freeform');
  if (!prompt || controller || requesting || connectionTesting || state?.busy || !state?.ready || !task || !allowed(state.models.find(m => m.id === state.active_id), task)) return;
  if (new TextEncoder().encode(prompt).length > task.input_limit) { message('Your message is too long. Shorten it before sending; your draft has been kept.'); return; }
  const experimental = previewAccepted(state.models.find(m => m.id === state.active_id));
  const messages = [...conversation, {role: 'user', content: prompt}];
  const payload = {prompt, benchmark, language: $('language-choice').value, experimental, task: 'freeform', task_version: task.version,
    ...(!benchmark ? {model: state.active_id, messages, max_tokens: 1024} : {})};
  if (!benchmark && (messages.length > 63 || new TextEncoder().encode(JSON.stringify(payload)).length > 32768)) {
    message('This conversation is full. Start a new chat to continue. The existing text has been kept.'); return;
  }
  const activeController = new AbortController(); controller = activeController;
  const turn = benchmark ? null : addTurn(prompt);
  const target = turn?.output || $('benchmark-output');
  target.hidden = false; target.textContent = '';
  if (!benchmark) { $('prompt').value = ''; $('chat-help').open = false; resizeComposer(); $('prompt').focus(); }
  buttonStates(); state.models.forEach(modelCard); message('');
  for (const [id, unit] of [['speed', 'tokens/s'], ['first-token', 'seconds'], ['elapsed', 'seconds']]) metric(id, '—', unit);
  $('measurement-note').textContent = t(benchmark ? 'Short local test running. Results apply to this model and this workload.' : 'Running on your device…');
  $('chat-announcement').textContent = t('Waiting for the first text…');
  const start = performance.now(); let first = null, done = false, reader;
  let output = '', pending = '', limited = false;
  function event(line) {
    if (!line.trim()) return;
    const item = JSON.parse(line);
    if (item.error) throw new Error(typeof item.error === 'string' ? item.error : 'The model returned an error.');
    if (item.response) {
      if (first === null) { first = (performance.now() - start) / 1000; metric('first-token', first.toFixed(2), 'seconds'); if (turn) turn.status.textContent = ''; }
      if (output.length + item.response.length > 131072) throw new Error('Output exceeded the display memory limit.');
      output += item.response; target.textContent = output;
      if (turn) scrollLatest();
    }
    if (item.done) {
      done = true; limited = item.limited === true;
      const seconds = item.eval_duration / 1e9;
      const rate = seconds > 0 && item.eval_count > 0 ? item.eval_count / seconds : null;
      metric('speed', rate === null ? '—' : rate.toFixed(1), 'tokens/s');
      metric('elapsed', ((performance.now() - start) / 1000).toFixed(2), 'seconds');
      $('measurement-note').textContent = t(`${item.eval_count || 0} generated tokens. Speed uses geistd's generation time, including token streaming; first text and total include the local connection and prompt processing. ${benchmark ? 'A short sample, not a general benchmark.' : ''}`);
    }
  }
  try {
    const response = await api('/app/generate', payload, activeController.signal);
    if (!response.body) throw new Error('This browser does not support streamed responses.');
    reader = response.body.getReader(); const decoder = new TextDecoder();
    for (;;) {
      const chunk = await reader.read();
      pending += decoder.decode(chunk.value, {stream: !chunk.done});
      if (pending.length > 131072) throw new Error('The response exceeded the stream buffer limit.');
      let newline;
      while ((newline = pending.indexOf('\n')) !== -1) { const line = pending.slice(0, newline); pending = pending.slice(newline + 1); event(line); }
      if (chunk.done) { if (pending.trim()) event(pending); break; }
    }
    if (!done) throw new Error('The connection ended before the model completed its response.');
    const status = limited ? 'Response limit reached. You can ask Geist to continue.' : output ? '' : 'The model completed without producing text. Try a different prompt.';
    if (turn) {
      turn.status.textContent = t(status);
      if (limited) {
        const more = document.createElement('button'); more.type = 'button'; more.className = 'text-button'; more.textContent = t('Continue response'); more.dataset.label = 'Continue response';
        more.addEventListener('click', () => {
          if (controller || state?.busy) return;
          // A continuation from an older reply would target the wrong context.
          if (turn.output.id !== 'output') { message('Continue from the latest reply, or ask a new question.'); return; }
          run(t('Continue from where you stopped.'));
        });
        turn.actions.append(more);
      }
    } else message(status);
    $('chat-announcement').textContent = t(status || 'Response complete.');
  } catch (error) {
    activeController.abort();
    let status = error.name === 'AbortError' ? 'Stopped. Partial output is kept here.' : error.message;
    if (status.includes("does not fit this model's context")) status = 'This conversation does not fit the model’s context. Shorten your draft or start a new chat. No earlier messages have been removed.';
    if (turn) turn.status.textContent = t(status); else message(status);
    $('chat-announcement').textContent = t(status);
    $('measurement-note').textContent = t('Run incomplete. No final generation speed is reported.');
    metric('speed', '—', 'tokens/s');
    metric('elapsed', ((performance.now() - start) / 1000).toFixed(2), 'seconds');
  } finally {
    if (reader) { try { await reader.cancel(); } catch { /* connection already closed */ } }
    if (turn) {
      // Partial answers are visible and explicitly marked, so follow-ups can
      // refer to them. Failed requests without text never enter model context.
      if (output) conversation = [...messages, {role: 'assistant', content: output}];
      else if (!$('prompt').value) { $('prompt').value = prompt; resizeComposer(); }
      turn.copy.disabled = !output;
      scrollLatest();
    }
    controller = null; buttonStates(); if (state) state.models.forEach(modelCard);
    if (stopped) message('Geist is stopping. Reopen the app to start it again.');
    else await poll();
  }
}

$('task-form').addEventListener('submit', event => { event.preventDefault(); run($('prompt').value); });
$('prompt').addEventListener('input', () => {
  resizeComposer();
  if ($('prompt').selectionEnd === $('prompt').value.length) $('prompt').scrollTop = $('prompt').scrollHeight;
  buttonStates(); scrollLatest();
});
$('prompt').addEventListener('keydown', event => {
  if (event.key !== 'Enter' || event.shiftKey || event.isComposing || $('prompt').dataset.composing || event.keyCode === 229) return;
  event.preventDefault();
  if (!event.repeat && !$('run').disabled) $('task-form').requestSubmit();
});
$('prompt').addEventListener('compositionstart', () => { $('prompt').dataset.composing = 'true'; });
$('prompt').addEventListener('compositionend', () => { delete $('prompt').dataset.composing; });
$('new-chat').addEventListener('click', () => {
  if (controller) return;
  if ((conversation.length || $('result').children.length || $('prompt').value) && !confirm(t('Clear this conversation and draft? They are not saved.'))) return;
  conversation = []; $('result').replaceChildren(); $('result').hidden = true; $('chat-empty').hidden = false;
  $('prompt').value = ''; $('chat-help').open = false; message(''); $('chat-announcement').textContent = t('New chat started.');
  scrollLatest(true); resizeComposer(); buttonStates(); $('prompt').focus();
});
function chooseTask(id) {
  selectedTask = tasks.find(task => task.id === id && !task.url);
  $('prompt').placeholder = t('Message Geist…');
  buttonStates(); if (state) state.models.forEach(modelCard);
}
$('language-choice').addEventListener('change', async () => {
  try { await api('/app/preferences', {language: $('language-choice').value}); }
  catch (error) { message(error.message); }
  buttonStates(); if (state) render(state);
});
async function loadTasks() {
  const catalog = await (await api('/app/tasks')).json();
  tasks = catalog.tasks;
  qualityRecords = catalog.quality_records || [];
  chooseTask('freeform');
  if (state) render(state);
}
$('stop').addEventListener('click', () => controller?.abort());
$('benchmark').addEventListener('click', () => run('Explain in a short paragraph how a seed grows into a plant.', true));
$('cancel-download').addEventListener('click', async () => { try { await api('/app/cancel', {}); message('Cancelling…'); } catch (error) { message(error.message); } });
$('unload').addEventListener('click', async () => { try { await api('/app/stop', {}); await poll(); } catch (error) { message(error.message); } });
$('quit').addEventListener('click', async () => {
  if (!confirm(t('Stop the shared service? Terminal and editor connections will stop too. Downloaded models are kept.'))) return;
  try {
    await api('/app/quit', {}); stopped = true; clearInterval(timer); controller?.abort();
    state = null; buttonStates(); $('runtime-state').textContent = t('Stopping');
    document.querySelectorAll('#models button, #quit').forEach(button => { button.disabled = true; });
    message('Geist is stopping. Reopen the app to start it again.');
  } catch (error) { message(error.message); }
});
document.querySelector('.brand').addEventListener('click', event => { event.preventDefault(); showPage('test-page'); });
document.querySelector('.skip').addEventListener('click', event => { event.preventDefault(); const main = $('main'); main.tabIndex = -1; main.focus(); });
window.addEventListener('beforeunload', () => controller?.abort());
const connectionHelp = {
  terminal: 'Paste the copied curl command into your terminal to try the loaded model. Ubuntu also installs geist test and geist chat. On Mac, the CLI is bundled at /Applications/Geist.app/Contents/MacOS/geist-cli.',
  continue: 'In Continue, open your local config.yaml and add the model from this configuration. JSON is valid YAML. Select Geist and use Chat mode. Preserve your existing configuration.',
  opencode: 'Save as opencode.json in a private test folder. Run opencode there and choose geist-chat. This profile disables tools; it does not enable coding-agent workflows.'
};
function updateConnectionHelp() { $('connection-help').textContent = t(connectionHelp[$('connection-client').value]); }
$('connection-client').addEventListener('change', updateConnectionHelp);
updateConnectionHelp();
$('copy-connection').addEventListener('click', async () => {
  try {
    const c = await (await api('/app/connections')).json();
    if (!c.ready) throw new Error('Load a model first.');
    const kind = $('connection-client').value;
    // The URL in a remote browser is the forwarded origin, not an arbitrary host.
    const base = `${location.origin}/v1`;
    let config;
    if (kind === 'continue') config = {name: 'Geist Local', version: '1.0.0', schema: 'v1', models: [{name: 'Geist', provider: 'openai', model: c.model, apiBase: base, apiKey: c.api_key, roles: ['chat'], capabilities: [], defaultCompletionOptions: {contextLength: 4096, maxTokens: 512}}]};
    else if (kind === 'opencode') config = {$schema: 'https://opencode.ai/config.json', provider: {geist: {npm: '@ai-sdk/openai-compatible', name: 'Geist', options: {baseURL: base, apiKey: c.api_key}, models: {[c.model]: {name: 'Geist local text', tool_call: false, limit: {context: 4096, output: 512}}}}}, model: `geist/${c.model}`, default_agent: 'geist-chat', agent: {'geist-chat': {mode: 'primary', description: 'Local text chat without tools', prompt: 'Answer the user briefly. You cannot access files or execute tools.', permission: {'*': 'deny'}}}};
    else {
      const quote = text => `'${text.replaceAll("'", "'\\''")}'`;
      config = `curl ${quote(`${base}/chat/completions`)} -H ${quote(`Authorization: Bearer ${c.api_key}`)} -H 'Content-Type: application/json' --data ${quote(JSON.stringify({model: c.model, messages: [{role: 'user', content: 'Hello'}], max_tokens: 64}))}`;
    }
    await copyText(typeof config === 'string' ? config : JSON.stringify(config, null, 2));
    $('connection-result').textContent = t('Copied. The configuration contains your private local key.');
  } catch (error) { $('connection-result').textContent = t(error.message); }
});
$('test-connection').addEventListener('click', async () => {
  connectionTesting = true; buttonStates(); $('connection-result').textContent = t('Asking the loaded model through the editor endpoint…');
  try {
    const result = await (await api('/v1/chat/completions', {model: state.active_id, messages: [{role: 'user', content: 'Say hello in one sentence.'}], max_tokens: 32})).json();
    if (!result.choices?.[0]?.message?.content || !(result.usage?.completion_tokens > 0)) throw new Error('The model completed without text. Try another model.');
    $('connection-result').textContent = t(`Connected. The shared model returned ${result.usage.completion_tokens} tokens. Now test the configuration in your chosen client.`);
  } catch (error) { $('connection-result').textContent = t(error.message); }
  finally { connectionTesting = false; buttonStates(); }
});
if (!/^[a-f0-9]{64}$/.test(token)) message('Open Geist using the private link from the app or Pi launcher. The link contains your private local API key.');
else { loadTasks().catch(error => message(error.message)); poll(); timer = setInterval(poll, 1800); }

function showPage(id) {
  document.querySelectorAll('.page').forEach(page => { page.hidden = page.id !== id; });
  document.querySelectorAll('[data-page]').forEach(button => {
    if (button.dataset.page === id) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  chatLayout();
  const heading = id === 'test-page' && !$('setup').hidden ? $('setup-title') : $(id).querySelector('h2'); heading.tabIndex = -1; heading.focus();
}
document.querySelectorAll('[data-page]').forEach(button => button.addEventListener('click', () => showPage(button.dataset.page)));
$('ui-language').value = interfaceLanguage;
$('language-choice').value = interfaceLanguage;
$('ui-language').addEventListener('change', async () => {
  interfaceLanguage = $('ui-language').value;
  translateStatic();
  document.querySelectorAll('[data-label]').forEach(element => { element.textContent = t(element.dataset.label); });
  if (selectedTask) chooseTask(selectedTask.id);
  updateConnectionHelp();
  if (state) render(state);
  try {
    if (window.geistDesktop) await desktopMessage('language', interfaceLanguage);
    else localStorage.setItem('geist-language', interfaceLanguage);
  } catch { message(t('The language applies to this window but could not be saved.')); }
});
async function removeModel(id) {
  const model = state?.models.find(item => item.id === id);
  if (!model || requesting || controller || state.busy || (state.active_id === id && (state.ready || state.loading))) return;
  if (!confirm(t(`Remove ${model.name} from this computer? You can download it again later.`))) return;
  requesting = true; buttonStates(); state.models.forEach(modelCard);
  try { await api('/app/remove', {id}); if (manualChoice === id) manualChoice = null; message('', false); }
  catch (error) { message(error.message); }
  finally { requesting = false; await poll(); }
}
