'use strict';
const $ = id => document.getElementById(id);
const token = location.hash.slice(1);
// The capability stays in this page's memory/fragment, never localStorage.
const cards = new Map();
let state = null, controller = null, requesting = false, polling = false;
let lastServerMessage = '', localMessage = false;
let pendingModel = null, languageInitialized = false, customPreviewAccepted = false;
let stopped = false, timer;
let tasks = [], selectedTask = null;
let qualityRecords = [];
let connectionTesting = false;
// Page memory only. Never store prompts, answers or conversation in browser storage.
let conversation = [], followLatest = true;
let lastReply = null, replyPending = false;
const pendingMarkdown = new Set();
function updateMarkdown(target, source) {
  target.markdownSource = source;
  pendingMarkdown.add(target);
  flushMarkdown();
}
function flushMarkdown() {
  const selection = window.getSelection();
  for (const target of pendingMarkdown) {
    if (!target.isConnected) { pendingMarkdown.delete(target); continue; }
    // A changing final Markdown block must not replace a selected/focused node.
    if (target.contains(document.activeElement) || (selection?.rangeCount && !selection.isCollapsed && selection.getRangeAt(0).intersectsNode(target))) continue;
    chatMarkdown.render(target, target.markdownSource);
    pendingMarkdown.delete(target);
  }
  scrollLatest();
}
document.addEventListener('selectionchange', flushMarkdown);
document.addEventListener('focusout', () => setTimeout(flushMarkdown, 0));

function chatLayout() {
  document.body.classList.toggle('manager-page', !$('models-page').hidden);
  document.body.classList.toggle('has-model', !$('workspace').hidden);
  resizeComposer(); positionPerformance();
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
$('latest').addEventListener('click', () => { scrollLatest(true); $('prompt').focus({preventScroll: true}); });
window.addEventListener('resize', () => { resizeComposer(); scrollLatest(); positionPerformance(); });
function addTurn(prompt) {
  $('chat-empty').hidden = true; $('result').hidden = false;
  // Stable ids identify the latest response for automation/accessibility.
  $('output')?.removeAttribute('id'); $('copy')?.removeAttribute('id');
  const user = document.createElement('article'); user.className = 'chat-message user';
  const userText = document.createElement('div'); userText.className = 'message-text'; userText.textContent = prompt;
  user.setAttribute('aria-label', t('You')); user.append(userText);
  const answer = document.createElement('article'); answer.className = 'chat-message assistant';
  const label = document.createElement('div'); label.className = 'message-label'; label.textContent = `Geist · ${state.models.find(model => model.id === state.active_id)?.name || state.active}`;
  const output = document.createElement('div'); output.id = 'output'; output.className = 'message-text markdown'; output.markdownSource = '';
  const status = document.createElement('p'); status.className = 'message-status'; uiText(status, 'Waiting for the first text…');
  const actions = document.createElement('div'); actions.className = 'message-actions';
  const copy = document.createElement('button'); copy.id = 'copy'; copy.type = 'button'; copy.className = 'text-button'; copy.disabled = true; copy.textContent = t('Copy'); copy.dataset.label = 'Copy';
  copy.addEventListener('click', async () => {
    try { await copyText(output.markdownSource); copy.textContent = t('Copied'); }
    catch { message('Copy is unavailable here. Select the result and copy it manually.'); }
  });
  const metrics = document.createElement('span'); metrics.className = 'reply-metrics';
  actions.append(copy, metrics); answer.append(label, output, status, actions);
  $('result').append(user, answer); scrollLatest(true);
  return {output, status, actions, copy, metrics};
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
const bytes = n => n < 1e9 ? `${formatNumber(n / 1e6)} MB` : `${formatNumber(n / 1e9, 2)} GB`;
const gib = n => `${formatNumber(n / 2 ** 30, 1)} GiB`;

const knownNumber = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
const modelIdentity = () => JSON.stringify([state?.active_id, state?.active]);
const rateText = value => `${knownNumber(value) ? formatNumber(value, 1) : '—'} ${t('tok/s')}`;
const timeText = value => knownNumber(value) ? `${formatNumber(value, 2)} s` : '—';
function renderReplyMetrics(element) {
  const m = element.replyMetrics;
  if (!m) return;
  element.textContent = `${rateText(m.rate)} · ${knownNumber(m.tokens) ? m.tokens : '—'} ${t('tokens')} · ${timeText(m.total)}`;
  element.title = `${t('First text')}: ${timeText(m.first)}`;
}
function renderPerformance() {
  if (lastReply && lastReply.model !== modelIdentity()) lastReply = null;
  const h = state?.hardware, r = state?.resources;
  const rss = r?.scope === 'geistd' && knownNumber(r.rss_bytes) ? r.rss_bytes : null;
  const cpu = r?.scope === 'geistd' && knownNumber(r.cpu_percent) ? r.cpu_percent : null;
  $('test-speed').textContent = replyPending ? t('Measuring…') : rateText(lastReply?.rate);
  $('test-memory').textContent = `${rss === null ? '—' : gib(rss)} RAM`;
  const model = state?.models.find(item => item.id === state.active_id);
  $('test-size').textContent = model && knownNumber(model.bytes) ? bytes(model.bytes) : '—';
  $('performance-system').textContent = h?.name || t('Not available');
  $('performance-os').textContent = h ? [h.os, h.arch, h.logical_cpus ? `${h.logical_cpus} ${t('logical CPUs')}` : null].filter(Boolean).join(' · ') : '—';
  $('performance-cpu').textContent = cpu === null ? '—' : `${formatNumber(cpu, 1)} %`;
  $('performance-ram').textContent = h?.known && knownNumber(h.ram) ? gib(h.ram) : '—';
  $('performance-available').textContent = h?.available_known && knownNumber(h.available) ? gib(h.available) : '—';
  $('performance-tokens').textContent = knownNumber(lastReply?.tokens) ? String(lastReply.tokens) : '—';
  $('performance-first').textContent = timeText(lastReply?.first);
  $('performance-total').textContent = timeText(lastReply?.total);
}

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

function uiText(element, source) {
  element.dataset.uiText = source; element.textContent = t(source);
}
function message(text, local = true) { uiText($('notice'), text); localMessage = local; }
function buttonStates() {
  renderPerformance();
  $('test-connection').disabled = !state?.ready || state?.busy || connectionTesting || requesting || !!controller;
  $('copy-connection').disabled = !state?.ready;
  $('connection-endpoint').textContent = t(`${location.origin}/v1`);
  $('connection-model').textContent = t(state?.active_id || 'Choose a model');
  const ready = selectedTask && !selectedTask.url && state?.ready && !state?.busy && !requesting && !controller && !connectionTesting && allowed(state?.models.find(m => m.id === state.active_id));
  $('run').disabled = !ready || !$('prompt').value.trim();
  $('run').hidden = !!controller;
  $('new-chat').disabled = !!controller || !(conversation.length || $('result').children.length || $('prompt').value);
  $('new-chat').hidden = !(conversation.length || $('result').children.length || $('prompt').value);
  $('benchmark').disabled = !state?.ready || state?.busy || requesting || !!controller ||
    !allowed(state?.models.find(m => m.id === state.active_id), tasks.find(t => t.id === 'freeform'));
  if (!controller && document.activeElement === $('stop')) $('prompt').focus({preventScroll: true});
  $('stop').hidden = !controller;
  document.body.classList.toggle('generating', !!controller);
  const runtimeStatus = t(controller ? 'Generating locally…' : state?.ready ? 'Model ready' : 'No model loaded');
  $('runtime-state').setAttribute('aria-label', runtimeStatus); $('runtime-state').title = runtimeStatus;
  $('runtime-state').classList.toggle('inactive', !state?.ready);
  $('runtime-name').textContent = state?.models.find(model => model.id === state.active_id)?.name || state?.active || '';
  $('language-choice').disabled = !!controller;
}

const ringMarkup = '<svg viewBox="0 0 36 36" aria-hidden="true"><circle class="ring-track" cx="18" cy="18" r="14"/><circle class="ring-fill" cx="18" cy="18" r="14" pathLength="100"/><path class="ring-check" d="m12 18 4 4 8-8"/></svg>';
function downloadState(model, current = state) {
  const total = Math.max(0, model?.bytes || 0);
  const transferring = current?.job_model === model?.id && !!current?.phase;
  const checking = transferring && current.phase === 'verifying';
  const received = Math.max(0, transferring ? current.received || 0 : model?.partial || 0);
  const percent = total ? Math.min(100, Math.floor(received / total * 100)) : 0;
  if (checking) return {stage:'verifying', percent:null, text:'Checking download…'};
  if (transferring) return {stage:'downloading', percent, text:`Downloading · ${percent}%`};
  if (model?.installed) return {stage:'downloaded', percent:100, text:'Downloaded'};
  if (received) return {stage:'paused', percent, text:`Paused · ${percent}%`};
  return {stage:'missing', percent:0, text:'Not downloaded'};
}
function renderRing(element, model, current = state) {
  const status = downloadState(model, current);
  if (!element.firstChild) element.innerHTML = ringMarkup; // Fixed, local markup only.
  element.dataset.stage = status.stage;
  element.style.setProperty('--ring-progress', status.percent === null ? 72 : status.percent);
  const progress = ['downloading', 'paused', 'verifying'].includes(status.stage);
  element.setAttribute('role', progress ? 'progressbar' : 'img');
  element.removeAttribute('aria-hidden');
  element.setAttribute('aria-label', `${model?.name || ''}: ${t(status.text)}`);
  if (progress) element.setAttribute('aria-valuetext', t(status.text));
  else element.removeAttribute('aria-valuetext');
  if (progress && status.percent !== null) {
    element.setAttribute('aria-valuemin', '0'); element.setAttribute('aria-valuemax', '100'); element.setAttribute('aria-valuenow', String(status.percent));
  } else for (const attribute of ['aria-valuemin', 'aria-valuemax', 'aria-valuenow']) element.removeAttribute(attribute);
  return status;
}

const modelIcons = {
  download: '<path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"/>',
  start: '<path d="m8 4 12 8-12 8z"/>',
  pause: '<path d="M8 5v14M16 5v14"/>',
  active: '<path d="m5 12 4 4L19 6"/>'
};
function canPause(model) {
  return state?.job_model === model.id && state.phase === 'downloading' && !state.loading;
}
function modelCard(model) {
  let card = cards.get(model.id);
  if (!card) {
    card = document.createElement('article'); card.className = 'model'; card.dataset.id = model.id;
    // One semantic button covers the name and icon. Details/removal are siblings.
    card.innerHTML = '<button class="model-pick" type="button" aria-describedby="catalog-preview"><span class="model-ring"></span><span class="model-info"><span class="model-name"></span><span class="download-state"></span></span><span class="model-action" aria-hidden="true"></span></button><span class="fit"></span><span class="transfer-detail"></span><details><summary></summary><p class="specs"></p><p class="reason"></p><p class="performance"></p><p class="quality"></p></details><button class="remove text-button icon-button" type="button"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M10 10v7m4-7v7"/></svg></button>';
    card.querySelector('.model-pick').addEventListener('click', event => { if (event.detail < 2) choose(model.id); });
    card.querySelector('.model-pick').addEventListener('keydown', event => {
      if (event.repeat && (event.key === 'Enter' || event.key === ' ')) event.preventDefault();
    });
    card.querySelector('.remove').addEventListener('click', () => removeModel(model.id));
    cards.set(model.id, card); $('models').append(card);
  }
  const active = state.active_id === model.id && state.ready;
  const pending = pendingModel === model.id;
  const preparing = state.job_model === model.id && (!!state.phase || state.loading);
  const paused = canPause(model);
  const evidence = qualityFor(model);
  card.className = `model${active ? ' active' : ''}${preparing || pending ? ' preparing' : ''}${model.resource_fit === 2 ? ' unavailable' : ''}`;
  card.querySelector('.model-name').textContent = model.name;
  const download = renderRing(card.querySelector('.model-ring'), model);
  // The button's complete name exposes status; a duplicate nested progress role
  // is unnecessary to screen readers. Numeric ring attributes remain inspectable.
  card.querySelector('.model-ring').setAttribute('aria-hidden', 'true');
  card.querySelector('.model-ring').dataset.fit = String(model.resource_fit);
  card.querySelector('.model-ring').title = model.resource_fit ? t(model.reason || 'Limited on this computer') : t(download.text);
  const status = pending ? 'Getting ready…' : preparing && state.loading ? 'Loading model' : download.text;
  card.querySelector('.download-state').textContent = t(status);
  card.querySelector('.download-state').hidden = !pending && !preparing && (download.stage === 'missing' || download.stage === 'downloaded');
  card.querySelector('.fit').hidden = model.resource_fit === 0 && (model.installed || model.id !== state.recommendation.id);
  card.querySelector('.fit').textContent = t(active ? 'Running here' : model.resource_fit === 2 ? 'Unavailable' : model.id === state.recommendation.id ? 'Suggested' : model.resource_fit === 1 ? 'Conditional' : 'Available');
  const detail = card.querySelector('.transfer-detail');
  detail.hidden = !preparing || state.loading;
  detail.textContent = paused ? downloadEstimate(model.id, state.received || 0, model.bytes || 0) : preparing ? t('Checking download…') : '';
  card.querySelector('.specs').textContent = model.bytes ? t(`${bytes(model.bytes)} download · ${model.ram_gib} GiB RAM guidance`) : t('Local model');
  card.querySelector('summary').textContent = t('Details');
  card.querySelector('.reason').textContent = t(model.reason || '');
  card.querySelector('.performance').textContent = t(model.measured_tps > 0 ? `Measured here: ${model.measured_tps.toFixed(1)} tokens/s · ${model.measured_tokens} tokens · this session` : model.performance || '');
  card.querySelector('.quality').textContent = t(evidence ? `Task quality: ${evidence.quality} · ${evidence.cases} test cases · ${evidence.language.toUpperCase()}. ${evidence.human_complete ? 'Human sample complete.' : 'Human assessment pending.'}` : 'Task quality: unverified for this task, language and device.');
  const button = card.querySelector('.model-pick');
  const action = paused ? 'Pause download' : active && allowed(model) ? 'Active' : model.installed ? 'Start model' : model.partial ? 'Resume download' : 'Download and start';
  const icon = paused ? 'pause' : active && allowed(model) ? 'active' : model.installed ? 'start' : 'download';
  const glyph = card.querySelector('.model-action');
  if (glyph.dataset.icon !== icon) {
    glyph.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true">${modelIcons[icon]}</svg>`;
    glyph.dataset.icon = icon;
  }
  glyph.hidden = icon === 'active';
  button.title = `${t(action)}: ${model.name}`;
  button.setAttribute('aria-label', `${t(action)}: ${model.name} · ${t(status)}${model.resource_fit ? ` · ${t(model.reason || 'Limited on this computer')}` : ''}`);
  button.setAttribute('aria-describedby', 'catalog-preview');
  button.disabled = requesting || !!controller || connectionTesting || (!paused && (model.resource_fit === 2 || state.busy || state.loading || !!state.phase || (active && allowed(model))));
  const remove = card.querySelector('.remove');
  remove.title = `${t('Remove download')}: ${model.name}`;
  remove.hidden = model.id === 'custom' || (!model.installed && !model.partial);
  remove.disabled = state.busy || state.loading || !!state.phase || requesting || !!controller || connectionTesting;
  remove.setAttribute('aria-label', `${t('Remove download')}: ${model.name}`);
}

function visibleModels() {
  const models = state?.models || [];
  return state?.active_id === 'custom' && state.ready ? [{id:'custom', name:state.active, installed:true, resource_fit:0}, ...models] : models;
}
function render(next) {
  const modelChanged = state && (state.active_id !== next.active_id || state.active !== next.active);
  state = next;
  if (!languageInitialized) {
    $('language-choice').value = next.answer_language || interfaceLanguage;
    languageInitialized = true;
  }
  const working = !!next.phase || next.loading;
  const active = next.models.find(m => m.id === next.active_id);
  const usable = next.ready && allowed(active);
  if (modelChanged || working || !usable) $('performance').open = false;
  const previouslyHidden = $('workspace').hidden;
  $('workspace').hidden = working || !usable;
  $('test-unavailable').hidden = usable && !working;
  $('model-prompt').textContent = t(working ? 'Getting ready…' : 'Choose a model to begin.');
  if (previouslyHidden && !$('workspace').hidden && !$('models-page').hidden &&
      (document.activeElement === document.body || document.activeElement.closest('.model-pick'))) $('prompt').focus({preventScroll:true});
  $('disk-space').textContent = t(next.hardware.disk_known ? `${bytes(next.hardware.disk)} disk space available` : 'Disk space could not be read');
  const models = visibleModels();
  if (!models.some(model => model.id === 'custom') && cards.has('custom')) { cards.get('custom').remove(); cards.delete('custom'); }
  // Keep existing nodes and ordering stable while downloading and polling.
  const ordered = cards.size ? models : [...models].sort((a, b) =>
    (b.id === next.active_id) - (a.id === next.active_id) ||
    (b.id === next.recommendation.id) - (a.id === next.recommendation.id) ||
    Number(b.installed) - Number(a.installed) || a.resource_fit - b.resource_fit);
  if (!working) { transfer.id = ''; transfer.samples = []; }
  ordered.forEach(modelCard);
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

async function choose(id) {
  const model = visibleModels().find(item => item.id === id);
  if (!model || stopped || requesting || controller || connectionTesting) return;
  const pause = canPause(model);
  if (!pause && (state.busy || state.loading || state.phase || model.resource_fit === 2)) return;
  if (!pause && state.ready && state.active_id === id && allowed(model)) return;
  requesting = true; pendingModel = id; buttonStates(); visibleModels().forEach(modelCard); message('', false);
  try {
    if (pause) {
      await api('/app/cancel', {});
      message('Cancelling…');
      return;
    }
    // The deliberate row action is next to the visible preview notice and is
    // described by it for assistive technology. Consent remains bound to a hash.
    if (id === 'custom') customPreviewAccepted = true;
    else if (!previewAccepted(model)) await api('/app/preview', {id, experimental:true});
    if (!state.ready || state.active_id !== id) {
      await api(model.installed ? '/app/select' : '/app/download', {id});
    }
  } catch (error) { message(error.message); }
  finally {
    // Keep the row locked until a fresh status reflects the completed action.
    // A periodic poll already in flight may still describe the previous state.
    while (polling) await new Promise(resolve => setTimeout(resolve, 40));
    await poll();
    requesting = false; pendingModel = null;
    if (state) render(state); else buttonStates();
  }
}

function metric(id, value, unit) {
  const target = $(id); target.replaceChildren(document.createTextNode(value));
  const label = document.createElement('small'); label.textContent = t(unit); target.append(label);
}

async function run(prompt, benchmark = false, preserveDraft = false) {
  prompt = prompt.trim();
  const task = tasks.find(t => t.id === 'freeform');
  if (!prompt || controller || requesting || connectionTesting || state?.busy || !state?.ready || !task || !allowed(state.models.find(m => m.id === state.active_id), task)) return;
  if (new TextEncoder().encode(prompt).length > task.input_limit) { message('Your message is too long. Shorten it before sending; your draft has been kept.'); return; }
  const experimental = previewAccepted(state.models.find(m => m.id === state.active_id));
  const messages = [...conversation, {role: 'user', content: prompt}];
  const payload = {prompt, benchmark, language: $('language-choice').value, experimental, task: 'freeform', task_version: task.version,
    ...(!benchmark ? {model: state.active_id, messages, max_tokens: 1024} : {})};
  if (!benchmark && (messages.length > 63 || new TextEncoder().encode(JSON.stringify(payload)).length > 32768)) {
    message('This test is full. Use Clear test to start again. The existing text has been kept.'); return;
  }
  const activeController = new AbortController(); controller = activeController;
  const turn = benchmark ? null : addTurn(prompt);
  const requestModel = modelIdentity();
  if (turn) { lastReply = null; replyPending = true; }
  const target = turn?.output || $('benchmark-output');
  target.hidden = false; target.textContent = '';
  if (!benchmark) { if (!preserveDraft) $('prompt').value = ''; $('chat-help').open = false; $('performance').open = false; resizeComposer(); $('prompt').focus(); }
  buttonStates(); visibleModels().forEach(modelCard); message('');
  for (const [id, unit] of [['speed', 'tokens/s'], ['first-token', 'seconds'], ['elapsed', 'seconds']]) metric(id, '—', unit);
  uiText($('measurement-note'), benchmark ? 'Short local test running. Results apply to this model and this workload.' : 'Running on your device…');
  uiText($('chat-announcement'), 'Waiting for the first text…');
  const start = performance.now(); let first = null, done = false, reader, completion = null;
  let output = '', pending = '', limited = false, paintTimer = null;
  function paint() { paintTimer = null; if (turn) updateMarkdown(target, output); else target.textContent = output; }
  function event(line) {
    if (!line.trim()) return;
    const item = JSON.parse(line);
    if (item.error) throw new Error(typeof item.error === 'string' ? item.error : 'The model returned an error.');
    if (item.response) {
      if (first === null) { first = (performance.now() - start) / 1000; metric('first-token', first.toFixed(2), 'seconds'); if (turn) turn.status.textContent = ''; }
      if (output.length + item.response.length > 131072) throw new Error('Output exceeded the display memory limit.');
      output += item.response;
      if (paintTimer === null) paintTimer = setTimeout(paint, 60);
    }
    if (item.done) {
      done = true; limited = item.limited === true;
      completion = item;
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
    const duration = knownNumber(completion?.eval_duration) ? completion.eval_duration / 1e9 : null;
    const tokens = Number.isSafeInteger(completion?.eval_count) && completion.eval_count >= 0 ? completion.eval_count : null;
    const measured = {model: requestModel, tokens, first, total: (performance.now() - start) / 1000,
      rate: duration > 0 && tokens > 0 ? tokens / duration : null};
    metric('speed', measured.rate === null ? '—' : measured.rate.toFixed(1), 'tokens/s');
    metric('elapsed', measured.total.toFixed(2), 'seconds');
    uiText($('measurement-note'), `${tokens ?? 0} generated tokens. Speed uses geistd's generation time, including token streaming; first text and total include the local connection and prompt processing. ${benchmark ? 'A short sample, not a general benchmark.' : ''}`);
    if (turn) { lastReply = measured; turn.metrics.replyMetrics = measured; renderReplyMetrics(turn.metrics); }
    const status = limited ? 'Response limit reached. You can ask Geist to continue.' : output ? '' : 'The model completed without producing text. Try a different prompt.';
    if (turn) {
      uiText(turn.status, status);
      if (limited) {
        const more = document.createElement('button'); more.type = 'button'; more.className = 'text-button'; more.textContent = t('Continue response'); more.dataset.label = 'Continue response';
        more.addEventListener('click', () => {
          if (controller || state?.busy) return;
          // A continuation from an older reply would target the wrong context.
          if (turn.output.id !== 'output') { message('Continue from the latest reply, or ask a new question.'); return; }
          run(t('Continue from where you stopped.'), false, true);
        });
        turn.actions.append(more);
      }
    } else message(status);
    uiText($('chat-announcement'), status || 'Response complete.');
  } catch (error) {
    activeController.abort();
    let status = error.name === 'AbortError' ? 'Stopped. Partial output is kept here.' : error.message;
    if (status.includes("does not fit this model's context")) status = 'This test does not fit the model’s context. Shorten your draft or use Clear test to start again. No earlier messages have been removed.';
    if (turn) uiText(turn.status, status); else message(status);
    uiText($('chat-announcement'), status);
    uiText($('measurement-note'), 'Run incomplete. No final generation speed is reported.');
    metric('speed', '—', 'tokens/s');
    metric('elapsed', ((performance.now() - start) / 1000).toFixed(2), 'seconds');
  } finally {
    clearTimeout(paintTimer); paint();
    if (reader) { try { await reader.cancel(); } catch { /* connection already closed */ } }
    if (turn) {
      // Partial answers are visible and explicitly marked, so follow-ups can
      // refer to them. Failed requests without text never enter model context.
      if (output) conversation = [...messages, {role: 'assistant', content: output}];
      else if (!preserveDraft && !$('prompt').value) { $('prompt').value = prompt; resizeComposer(); }
      turn.copy.disabled = !output;
      scrollLatest();
    }
    controller = null; replyPending = false; buttonStates(); if (state) visibleModels().forEach(modelCard);
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
  conversation = []; lastReply = null; pendingMarkdown.clear(); $('result').replaceChildren(); $('result').hidden = true; $('chat-empty').hidden = false;
  $('prompt').value = ''; $('chat-help').open = false; $('performance').open = false; message(''); uiText($('chat-announcement'), 'Test cleared.');
  resizeComposer(); buttonStates(); followLatest = true; $('latest').hidden = true; $('transcript').scrollTop = 0; $('prompt').focus();
});
function chooseTask(id) {
  selectedTask = tasks.find(task => task.id === id && !task.url);
  $('prompt').placeholder = t('Message Geist…');
  buttonStates(); if (state) visibleModels().forEach(modelCard);
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
$('stop').addEventListener('click', () => { controller?.abort(); $('prompt').focus({preventScroll: true}); });
$('benchmark').addEventListener('click', () => run('Explain in a short paragraph how a seed grows into a plant.', true));
document.querySelector('.brand').addEventListener('click', event => { event.preventDefault(); showPage('models-page'); });
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
    uiText($('connection-result'), 'Copied. The configuration contains your private local key.');
  } catch (error) { uiText($('connection-result'), error.message); }
});
$('test-connection').addEventListener('click', async () => {
  connectionTesting = true; buttonStates(); uiText($('connection-result'), 'Asking the loaded model through the editor endpoint…');
  try {
    const result = await (await api('/v1/chat/completions', {model: state.active_id, messages: [{role: 'user', content: 'Say hello in one sentence.'}], max_tokens: 32})).json();
    if (!result.choices?.[0]?.message?.content || !(result.usage?.completion_tokens > 0)) throw new Error('The model completed without text. Try another model.');
    uiText($('connection-result'), `Connected. The shared model returned ${result.usage.completion_tokens} tokens. Now test the configuration in your chosen client.`);
  } catch (error) { uiText($('connection-result'), error.message); }
  finally { connectionTesting = false; buttonStates(); }
});
if (!/^[a-f0-9]{64}$/.test(token)) message('Open Geist using the private link from the app or Pi launcher. The link contains your private local API key.');
else { loadTasks().catch(error => message(error.message)); poll(); timer = setInterval(poll, 1800); }

function showPage(id) {
  if (!['models-page', 'connect-page', 'settings-page', 'test-page'].includes(id)) return false;
  const quickTest = id === 'test-page';
  if (quickTest) id = 'models-page'; // Native quick-test shortcut focuses the shared pane.
  document.querySelectorAll('.page').forEach(page => { page.hidden = page.id !== id; });
  document.querySelectorAll('nav [data-page]').forEach(button => {
    if (button.dataset.page === id) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  $('chat-help').open = false; $('performance').open = false;
  chatLayout();
  if (quickTest && !$('workspace').hidden) $('prompt').focus({preventScroll: true});
  else { const heading = $(id).querySelector('h1, h2'); heading.tabIndex = -1; heading.focus({preventScroll:true}); }
  window.scrollTo(0, 0);
  return true;
}
// Native menus route only to these fixed pages, without reloading the document.
window.geistNavigate = showPage;
document.querySelectorAll('[data-page]').forEach(button => button.addEventListener('click', () => showPage(button.dataset.page)));
$('ui-language').value = interfacePreference;
$('language-choice').value = interfaceLanguage;
$('ui-language').addEventListener('change', async () => {
  interfacePreference = languagePreference($('ui-language').value);
  interfaceLanguage = resolveLanguage(interfacePreference, systemLanguage);
  if (!state?.answer_language) $('language-choice').value = interfaceLanguage;
  translateStatic();
  document.querySelectorAll('[data-ui-label]').forEach(element => element.setAttribute('aria-label', t(element.dataset.uiLabel)));
  document.querySelectorAll('[data-ui-title]').forEach(element => { element.title = t(element.dataset.uiTitle); });
  document.querySelectorAll('[data-ui-text]').forEach(element => { element.textContent = t(element.dataset.uiText); });
  document.querySelectorAll('.chat-message.user').forEach(element => element.setAttribute('aria-label', t('You')));
  document.querySelectorAll('.reply-metrics').forEach(renderReplyMetrics);
  document.querySelectorAll('[data-label]').forEach(element => { element.textContent = t(element.dataset.label); });
  if (selectedTask) chooseTask(selectedTask.id);
  updateConnectionHelp();
  if (state) render(state);
  try {
    if (window.geistDesktop) await desktopMessage('language', interfacePreference);
    else localStorage.setItem('geist-language', interfacePreference);
  } catch { message('The language applies to this window but could not be saved.'); }
});
async function removeModel(id) {
  const model = state?.models.find(item => item.id === id);
  if (!model || requesting || controller || state.busy || state.loading || state.phase || connectionTesting) return;
  const warning = state.active_id === id && state.ready ? `${t('The running model will stop. Connected programs will need another model.')}\n\n` : '';
  if (!confirm(warning + t(`Remove ${model.name} from this computer? You can download it again later.`))) return;
  requesting = true; buttonStates(); visibleModels().forEach(modelCard);
  const remove = cards.get(id)?.querySelector('.remove');
  let removed = false;
  try { await api('/app/remove', {id}); removed = true; message('', false); }
  catch (error) { message(error.message); }
  finally {
    while (polling) await new Promise(resolve => setTimeout(resolve, 40));
    await poll();
    requesting = false;
    if (state) render(state); else buttonStates();
    // The file disappears, but the same row remains a download choice.
    if (removed && !$('models-page').hidden &&
        (document.activeElement === remove || document.activeElement === document.body)) {
      const pick = cards.get(id)?.querySelector('.model-pick');
      if (pick && !pick.disabled) pick.focus({preventScroll:true});
    }
  }
}

function positionPerformance() {
  if (!$('performance').open) return;
  // Bound the expanded model information to the viewport, even at text zoom.
  // It overlays the transcript without moving the composer or existing messages.
  const bottom = $('performance').querySelector('summary').getBoundingClientRect().bottom;
  $('performance').style.setProperty('--performance-space', `${Math.max(0, innerHeight - bottom - 18)}px`);
}

// Only one disclosure is open. Escape returns focus; polling never moves it.
for (const id of ['chat-help', 'performance']) {
  $(id).addEventListener('toggle', () => {
    if ($(id).open) {
      $(id === 'chat-help' ? 'performance' : 'chat-help').open = false;
      if (id === 'performance') positionPerformance();
    }
  });
}
document.addEventListener('keydown', event => {
  if (event.key !== 'Escape') return;
  for (const id of ['chat-help', 'performance']) if ($(id).open) {
    event.preventDefault(); $(id).open = false;
    $(id).querySelector('summary').focus({preventScroll: true});
  }
});
document.addEventListener('pointerdown', event => {
  for (const id of ['chat-help', 'performance']) if (!$(id).contains(event.target)) $(id).open = false;
});
