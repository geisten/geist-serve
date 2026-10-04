'use strict';
let historySaving = false;
const $ = id => document.getElementById(id);
const token = location.hash.slice(1);
const statusPollInterval = 1800;
// The capability stays in this page's memory/fragment, never localStorage.
const cards = new Map(), modelGroups = new Map();
let groupSequence = 0;
let stateReceivedAt = performance.now();
let state = null, controller = null, requesting = false, polling = false;
let downloadRequest = false;
let lastServerMessage = '', localMessage = false;
let pendingModel = null, languageInitialized = false, customPreviewAccepted = false;
let stopped = false, timer;
let tasks = [], selectedTask = null;
let qualityRecords = [];
let measuringModel = null;
let modelsFoldedByUser = null; // #125: null until the list button is used // #103: start this model, then measure its speed
let connectionTesting = false;
// Page memory only. Never store prompts, answers or conversation in browser storage.
let conversation = [], conversationModel = '', followLatest = true;
let lastReply = null, replyPending = false, cancellingModel = null, resultModel = '';
let workspaceModel = null, pendingExecution = null;
let activityInstance='', activitySnapshot=null, activityAt=0, requestAfter=0, activeTurn=null, cancellingActivity=false, activityReturn=null;
const activitySequences = new Map();
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
$('latest').addEventListener('click', () => { scrollLatest(true); $('prompt').focus({preventScroll: true}); });
window.addEventListener('resize', () => { resizeComposer(); scrollLatest(); });
const replyCopyIcon = '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3"/></svg>';
const replyCopiedIcon = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m5 12 4 4L19 6"/></svg>';
function renderReplyCopy(button) {
  const label = t(button.dataset.copied === 'true' ? 'Copied' : 'Copy response');
  button.title = label;
  button.setAttribute('aria-label', label);
  button.innerHTML = button.dataset.copied === 'true' ? replyCopiedIcon : replyCopyIcon;
}
function addTurn(prompt) {
  $('chat-empty').hidden = true; $('result').hidden = false;
  // Stable ids identify the latest response for automation/accessibility.
  $('output')?.removeAttribute('id'); $('copy')?.removeAttribute('id');
  const user = document.createElement('article'); user.className = 'chat-message user';
  const userText = document.createElement('div'); userText.className = 'message-text'; userText.textContent = prompt;
  user.setAttribute('aria-label', t('You')); user.append(userText);
  const answer = document.createElement('article'); answer.className = 'chat-message assistant';
  const replyModel = state.models.find(model => model.id === state.active_id);
  const label = document.createElement('div'); label.className = 'message-label';
  label.textContent = `geisten · ${replyModel ? `${replyModel.group_name || replyModel.name} · ${t(plainVariant(replyModel))}` : modelLabel(replyModel)}`;
  label.title = modelLabel(replyModel); // #123: the weight format as detail
  const output = document.createElement('div'); output.id = 'output'; output.className = 'message-text markdown'; output.markdownSource = '';
  const status = document.createElement('p'); status.className = 'message-status'; uiText(status, 'Sending…');
  const actions = document.createElement('div'); actions.className = 'message-actions';
  const copy = document.createElement('button'); copy.id = 'copy'; copy.type = 'button'; copy.className = 'text-button reply-copy'; copy.disabled = true;
  renderReplyCopy(copy);
  let copiedTimer;
  copy.addEventListener('click', async () => {
    try {
      await copyText(output.markdownSource);
      if (!copy.isConnected) return;
      copy.dataset.copied = 'true'; renderReplyCopy(copy);
      uiText($('chat-announcement'), 'Response copied.');
      clearTimeout(copiedTimer);
      copiedTimer = setTimeout(() => { copy.dataset.copied = 'false'; if (copy.isConnected) renderReplyCopy(copy); }, 1800);
    }
    catch { message('Copy is unavailable here. Select the result and copy it manually.'); }
  });
  const metrics = document.createElement('span'); metrics.className = 'reply-metrics';
  actions.append(copy, metrics); answer.append(label, output, status, actions);
  $('result').append(user, answer); scrollLatest(true);
  return {output, status, actions, copy, metrics};
}
// #93: the model's thinking, as plain text in a collapsed disclosure. Session-only:
// never part of the answer, its copy or the conversation sent back to the model.
function appendThinking(turn, text) {
  if (!turn.thinking) {
    const details = document.createElement('details'); details.className = 'thinking';
    const summary = document.createElement('summary'); uiText(summary, 'Show thinking');
    const body = document.createElement('div'); body.className = 'thinking-text';
    turn.thinking = document.createTextNode(''); body.append(turn.thinking);
    details.append(summary, body); turn.output.before(details);
  }
  if (turn.thinking.length + text.length <= 131072) turn.thinking.appendData(text);
}
const clock = seconds => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
function qualityFor(model, task = selectedTask) {
  if (state?.execution?.active === 'gpu') return undefined;
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

const variantLabel = model => model?.quantization || t('Local model');
// #51/#80: the primary line names the weight format in plain words. It says
// nothing about answer quality: that is not tested (status quality "unverified").
// I2_S and PQ2_0 are the models' native ternary weights, not a reduced copy.
const plainVariant = model => ({Q8_0:'8-bit', Q4_0:'4-bit', Q4_K_M:'4-bit', I2_S:'Ternary (native)', PQ2_0:'Ternary (native)'})[model?.quantization] || 'Local model';
const hostName = () => window.geistDesktop === 'mac' ? 'Mac' : 'computer';
// The platform default or fallback, until the user has chosen: exactly one card.
const recommendedModel = snapshot => {
  const best = snapshot?.best_choice;
  return best && best.action !== 'active' ? snapshot.models?.find(m => m.id === best.id) || null : null;
};
const ramReasons = new Set(['RAM is smaller than the model file, before context and OS memory.', 'Below the RAM recommendation; swapping or allocation failures are possible.', 'Available RAM is tight now. Close other apps before loading this model.']);
const modelLabel = model => model ? model.quantization ? `${model.group_name || model.name} · ${model.quantization}` : model.name : state?.active || '';
const knownNumber = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
const backendName = value => ({metal:'Metal',vulkan:'Vulkan',cuda:'CUDA',hip:'ROCm',sycl:'SYCL'})[value] || value || '';
const modelIdentity = () => JSON.stringify([state?.active_id, state?.active, state?.execution?.backend]);
const workspaceIdentity = () => JSON.stringify([state?.active_id, state?.active, state?.models.find(m => m.id === state.active_id)?.sha256]);
const executionLoading = () => pendingExecution !== null || !!state?.loading;
// #52: say where a number comes from in words; the raw API names stay in the
// status JSON and the JSONL export for diagnosis.
const sourceNames = {'macos.proc_pid_rusage.ri_resident_size': 'Measured by macOS', 'linux.proc_pid_stat.rss': 'Measured by Linux', 'metal.MTLDevice.currentAllocatedSize': 'Reported by Metal'};
const sourceText = source => source ? t(sourceNames[source] || 'Measured by the system') : null;

// #55: one wording for the engine line in both dialogs. Unreported provenance
// (legacy daemon, or none yet) hides the line instead of "Unknown · Unknown".
// The speed sentence in a fit reason comes from the measured numbers (#90):
// "CPU 13 t/s · GPU 30 t/s", or the newest values from an earlier build.
const tps = value => `${formatNumber(value, value < 10 ? 1 : 0)} t/s`;
const speedParts = s => ['cpu', 'gpu'].filter(k => knownNumber(s?.[k]) && s[k] > 0).map(k => `${k.toUpperCase()} ${tps(s[k])}`);
function fitReason(model) {
  const base = ["Fits this Mac's memory.", 'No known resource restriction.'].find(b => model.reason?.startsWith(b));
  const now = speedParts(model.speed), before = speedParts(model.earlier);
  if (!base || (!now.length && !before.length)) return t(model.reason || '');
  if (now.length) return `${t(base)} ${now.join(' · ')}`;
  const version = model.earlier.engine?.split('.').slice(0, 2).join('.');
  const sameVersion = version && version === state?.engine?.geistlib?.version?.split('.').slice(0, 2).join('.');
  const label = version ? `${t('geisten engine')} ${version}${sameVersion ? ` (${t('earlier build')})` : ''}` : t('Earlier version');
  return `${t(base)} ${label} · ${before.join(' · ')}. ${t('Not measured with this version yet.')}`;
}
function engineText(engine) {
  const lib = engine?.geistlib;
  if (!lib?.version) return '';
  return `geistlib ${lib.version}${lib.revision ? ` · ${lib.revision.slice(0, 12)}` : ''}${lib.source_state === 'modified' ? ` · ${t('Modified build')}` : ''}`;
}
const rateText = value => `${knownNumber(value) ? formatNumber(value, 1) : '—'} ${t('tokens/s')}`;
const timeText = value => knownNumber(value) ? `${formatNumber(value, 2)} s` : '—';
// #123: the reply shows how long it took; rate, tokens and first-text times are
// its tooltip and accessible name (layer 2/3 detail).
function renderReplyMetrics(element) {
  const m = element.replyMetrics;
  if (!m) return;
  const detail = `${rateText(m.rate)} · ${knownNumber(m.tokens) ? m.tokens : '—'} ${t('tokens')} · ${timeText(m.total)}`;
  element.textContent = timeText(m.total);
  element.title = `${detail} · ${t('First text')}: ${timeText(m.first)} · ${t('First answer')}: ${timeText(m.firstAnswer)}${m.reasoning ? ' · ' + t('Tokens and time include answer preparation.') : ''}`;
  element.setAttribute('aria-label', detail);
}
function renderMemory() {
  const r = state?.resources, memory = state?.memory;
  const localAge=Math.max(0,performance.now()-stateReceivedAt);
  const rssFresh=knownNumber(memory?.process_rss_sample_age_ms) && memory.process_rss_sample_age_ms+localAge<=6000;
  const rss = !executionLoading() && state?.ready && rssFresh && r?.scope === 'geistd' && knownNumber(memory?.process_rss_bytes) ? memory.process_rss_bytes : null;
  const rssReason=t(!rssFresh && knownNumber(memory?.process_rss_sample_age_ms) ? 'Stale measurement' : ({query_failed:'Measurement failed',stale:'Stale measurement'})[memory?.rss_unavailable_reason] || 'Not measured yet');
  $('test-memory').textContent = rss === null ? '—' : gib(rss);
  $('test-memory').title=rss===null ? rssReason : sourceText(memory.rss_source) || t('Process RSS');
  $('test-memory').setAttribute('aria-label',`${t('Process RSS')}: ${rss===null?rssReason:gib(rss)}`);
  const age=(memory?.gpu_sample_age_ms ?? 0)+localAge;
  const stale=age>6000;
  const gpuMemory = !executionLoading() && !stale && memory?.status===1 && knownNumber(memory.gpu_allocated_bytes) ? memory.gpu_allocated_bytes : null;
  const memoryReason = t(stale ? 'Stale measurement' : ({unsupported:'Unsupported',query_failed:'Measurement failed',stale:'Stale measurement'})[memory?.gpu_unavailable_reason] || 'Not measured yet');
  $('test-gpu-memory').textContent = gpuMemory===null ? '—' : gib(gpuMemory);
  $('test-gpu-memory').title = gpuMemory===null ? memoryReason : `${sourceText(memory.gpu_source)} · ${formatNumber(age/1000,1)} s`;
  $('test-gpu-memory').setAttribute('aria-label',`${t('Metal allocated')}: ${gpuMemory===null ? memoryReason : gib(gpuMemory)}`);
  $('memory-live').textContent = `${t('Process RSS')}: ${rss===null?rssReason:gib(rss)} · ${t('Metal allocated')}: ${gpuMemory===null?memoryReason:gib(gpuMemory)}`;
  $('memory-source').textContent = [sourceText(memory?.rss_source), sourceText(memory?.gpu_source), memory?.gpu_source && knownNumber(memory?.gpu_sample_age_ms) ? `${t('Sample age')}: ${formatNumber(age/1000,1)} s` : null, memory?.unified_memory ? t('Shared memory; values overlap.') : null].filter(Boolean).join(' · ');
}
function renderPerformance() {
  if (lastReply && lastReply.model !== modelIdentity()) lastReply = null;
  // #83: a connection-test result describes one model; drop it when the model changes.
  if (resultModel !== (state?.active_id || '')) { resultModel = state?.active_id || ''; uiText($('connection-result'), ''); }
  const h=state?.hardware, r=state?.resources;
  const cpu=!executionLoading() && state?.ready && r?.scope==='geistd' && knownNumber(r.cpu_percent) ? r.cpu_percent : null;
  renderMemory();
  const model = state?.models.find(item => item.id === state.active_id);
  $('test-size').textContent = model && knownNumber(model.bytes) ? bytes(model.bytes) : '—';
  $('performance-system').textContent = h?.name || t('Not available');
  $('performance-os').textContent = h ? [h.os, h.arch, h.logical_cpus ? `${h.logical_cpus} ${t('logical CPUs')}` : null].filter(Boolean).join(' · ') : '—';
  $('performance-cpu').textContent = cpu === null ? '—' : `${formatNumber(cpu, 1)} %`;
  $('performance-ram').textContent = h?.known && knownNumber(h.ram) ? gib(h.ram) : '—';
  $('performance-available').textContent = h?.available_known && knownNumber(h.available) ? gib(h.available) : '—';
  const profile = state?.performance_profile;
  const compatible = profile && profile.artifact === model?.sha256;
  for (const mode of ['cpu', 'gpu']) {
    const sample = compatible ? profile[mode] : null;
    // #81: the GPU backend by its reported name (Metal or Vulkan), never a hard-coded one.
    const own = r => mode === 'gpu' ? r.backend === state?.execution?.gpu_backend : r.backend?.startsWith('cpu');
    const availability = mode==='gpu' && !state?.execution?.gpu_available ? 'Not available' : profile?.recent?.some(r=>r.historical && own(r)) ? 'Historical' : mode === 'gpu' ? 'No GPU reply yet' : 'No CPU reply yet';
    for (const id of ['rate','typical']) $(`history-${mode}-${id}`).textContent = knownNumber(sample?.rate) ? rateText(sample.rate) : t(availability);
    $(`history-${mode}-range`).textContent = sample?.count >= 5 ? `${formatNumber(sample.q25,1)}–${formatNumber(sample.q75,1)} ${t('tokens/s')}` : '—';
    $(`summary-${mode}-first`).textContent = `◷ ${timeText(sample?.first_answer)}`;
    $(`summary-${mode}-count`).textContent = sample ? `n=${sample.count}` : 'n=—';
    $(`summary-${mode}-count`).title = t(sample ? sample.count<5 ? 'First observations' : 'Observed' : availability);
    $(`summary-${mode}-count`).setAttribute('aria-label', `${$(`summary-${mode}-count`).title} · ${sample?.count || 0}`);
    $(`summary-${mode}-first`).setAttribute('aria-label', `${t('First answer')}: ${timeText(sample?.first_answer)}`);
    $(`summary-${mode}-first`).title = `${t('Known values')}: ${sample?.first_answer_count || 0}`;
    $(`history-${mode}-answer`).textContent = `${timeText(sample?.first_answer)} · n=${sample?.first_answer_count || 0}`;
    $(`history-${mode}-first`).textContent = timeText(sample?.first);
    $(`history-${mode}-total`).textContent = timeText(sample?.total);
    $(`history-${mode}-tokens`).textContent = knownNumber(sample?.tokens) ? formatNumber(sample.tokens,Number.isInteger(sample.tokens)?0:1) : '—';
    $(`history-${mode}-ram`).textContent = knownNumber(sample?.rss_bytes) ? gib(sample.rss_bytes) : '—';
    $(`history-${mode}-peak`).textContent = knownNumber(sample?.sampled_peak_rss) ? gib(sample.sampled_peak_rss) : '—';
    $(`history-${mode}-gpu-memory`).textContent = knownNumber(sample?.gpu_allocated_bytes) ? `${gib(sample.gpu_allocated_bytes)} · n=${sample.gpu_known_count}` : '—';
    $(`history-${mode}-gpu-peak`).textContent = knownNumber(sample?.gpu_sampled_peak) ? gib(sample.gpu_sampled_peak) : '—';
    $(`history-${mode}-count`).textContent = sample ? `${sample.count} · ${t(sample.count < 5 ? 'First observations' : 'Typical')}` : '—';
    $(`profile-${mode}-confidence`).textContent = sample ? `${sample.count} · ${t(sample.count < 5 ? 'First observations' : 'Typical')}` : t(availability);
    $(`history-${mode}-time`).textContent = sample?.recorded_at ? new Intl.DateTimeFormat(interfaceLanguage, {dateStyle:'short',timeStyle:'short'}).format(new Date(sample.recorded_at * 1000)) : '—';
    $(`history-${mode}`).title = `${mode.toUpperCase()}: ${t(sample ? sample.count < 5 ? 'First observations' : 'Typical' : availability)} · ${sample?.count || 0}`;
  }
  // The engine that produced these measurements, else the one installed now (#55).
  const engineLine = engineText((compatible ? profile[state?.execution?.active]?.engine : null) || state?.engine);
  $('profile-engine').textContent = engineLine; $('profile-engine').hidden = !engineLine;
  // #81: each processor has its own workload; describe the one of the active processor.
  const group = compatible ? profile[`${state?.execution?.active}_group`] || profile.group : null;
  $('profile-collection').textContent = t(profile?.enabled ? 'Collection enabled' : 'Collection disabled');
  $('profile-group').textContent = group ? [t('Latest workload'), `${t('Input')}: ${['≤512','513–2048','>2048'][group.input]}`, `${t('Output')}: ${['<32','32–127','128–511','≥512'][group.output]}`, t('tokens'), t(group.cached ? 'Cache reused' : 'No cache reuse'), t(group.cold ? 'First reply after load' : 'Warm'), group.contention ? t('Download overlap') : '', group.controlled ? t('Controlled comparison') : t('Ordinary use')].filter(Boolean).join(' · ') : t('Not measured yet');
  $('profile-confidence').textContent = t('First observations: fewer than 5 replies. No automatic processor changes.');
  // Keep focused disclosure nodes stable during polling; replace only changed text.
  const recent = compatible ? profile.recent || [] : [];
  const recentKey = JSON.stringify([interfaceLanguage,recent]);
  if ($('profile-recent').dataset.key !== recentKey) {
    $('profile-recent').dataset.key = recentKey;
    $('profile-recent').replaceChildren(...recent.map(item => {
      const li = document.createElement('li');
      const rate = item.output && item.generation_ns > 0 ? item.output / (item.generation_ns / 1e9) : null;
      li.textContent = [new Intl.DateTimeFormat(interfaceLanguage,{dateStyle:'short',timeStyle:'short'}).format(new Date(item.timestamp*1000)), item.backend, rateText(rate), `${item.input}/${item.output} ${t('tokens')}`, t(item.outcome), t(item.source), item.historical ? t('Earlier configuration') : '', item.warmup ? t('Warmup') : '', item.contention ? t('Download overlap') : ''].filter(Boolean).join(' · ');
      return li;
    }));
  }
  if (profile && !historySaving) {
    $('history-enabled').checked = profile.enabled;
    $('history-days').value = String(profile.days);
  }
  $('history-storage').textContent = profile ? `${profile.retained} ${t('observations retained')} · ${profile.days} ${t('days')} · ${t('Up to 20 MiB / 8192 observations')}` : '—';
  $('profile-error').hidden = !profile?.error && !profile?.invalid;
  const profileError = profile?.error ? `${t('History could not be saved.')} ${profile.dropped} ${t('unsaved observations')}` : profile?.invalid ? `${profile.invalid} ${t('invalid records skipped')}` : '';
  if ($('profile-error').textContent !== profileError) $('profile-error').textContent = profileError;
  const comparison = state?.comparison;
  $('comparison-stop').hidden = !comparison?.running;
  const comparisonText = comparison?.running ? `${t(comparison.phase)} · ${comparison.step}/${state?.execution?.gpu_available ? 8 : 4}` : comparison?.result ? t(comparison.result) : '';
  if ($('comparison-status').textContent !== comparisonText) $('comparison-status').textContent = comparisonText;

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
    ...(body === undefined ? {} : { body: typeof body === 'string' ? body : JSON.stringify(body) })
  });
  if (!response.ok) {
    let error = `Request failed (${response.status}).`;
    try { const detail = (await response.json()).error; error = typeof detail === 'string' ? detail : detail?.message || error; } catch { /* retain status */ }
    throw new Error(error);
  }
  return response;
}

function uiText(element, source) {
  element.dataset.uiText = source; const text=t(source); if(element.textContent!==text) element.textContent=text;
}
function message(text, local = true) { uiText($('notice'), text); localMessage = local; }
// File transfers and inference have independent lifecycles. Preserve the old
// conservative boundary when talking to a service without the new status fields.
function inferenceBusy() { return state?.inference_busy ?? state?.busy; }
function runtimeRequest() { return requesting && !downloadRequest; }
// #82: another model is being prepared while the old one still answers status as ready.
// The server reports inference_busy while an activation job owns the runtime, not for a background download.
const switching = s => !!s?.active_id && !!s.job_model && s.job_model !== s.active_id && s.inference_busy === true && !s.background_download;
function buttonStates() {
  renderPerformance();
  renderExecution();
  $('test-connection').disabled = !state?.ready || inferenceBusy() || connectionTesting || runtimeRequest() || !!controller;
  $('copy-connection').disabled = !state?.ready;
  $('connection-endpoint').textContent = t(`${location.origin}/v1`);
  // #58: without a model this is a hint, not a copyable value.
  $('connection-model').classList.toggle('is-empty', !state?.active_id);
  if (state?.active_id) $('connection-model').textContent = state.active_id;
  else uiText($('connection-model'), 'No model loaded yet');
  // #59: say why the buttons are disabled, one cause at a time.
  const connectionReason = !state ? 'Service unavailable. Reopen geisten to reconnect.'
    : switching(state) ? 'The model is switching. Wait until it is ready.'
    : !state.ready ? 'Load a model first to copy or test the connection.'
    : $('test-connection').disabled && !connectionTesting ? 'Wait until the current answer finishes to test the connection.' : '';
  $('connection-disabled').hidden = !connectionReason;
  if (connectionReason) uiText($('connection-disabled-text'), connectionReason);
  $('connection-choose').hidden = !state || !!state.ready;
  const ready = selectedTask && !selectedTask.url && state?.ready && !inferenceBusy() && !runtimeRequest() && !controller && !connectionTesting && allowed(state?.models.find(m => m.id === state.active_id));
  $('run').disabled = !ready || !$('prompt').value.trim();
  $('run').hidden = !!controller;
  $('new-chat').disabled = !!controller || !(conversation.length || $('result').children.length || $('prompt').value);
  $('new-chat').hidden = false;
  $('benchmark').disabled = !state?.ready || state?.busy || runtimeRequest() || !!controller ||
    !allowed(state?.models.find(m => m.id === state.active_id), tasks.find(t => t.id === 'freeform'));
  if (!controller && document.activeElement === $('stop')) $('prompt').focus({preventScroll: true});
  $('stop').hidden = !controller;
  document.body.classList.toggle('generating', !!controller);
  const loading = executionLoading();
  const runtimeStatus = t(loading ? (pendingExecution !== null ? 'Switching processor…' : 'Loading model') : controller ? 'Generating locally…' : switching(state) ? 'Getting ready…' : state?.ready ? 'Model ready' : 'No model loaded');
  $('runtime-state').setAttribute('aria-label', runtimeStatus); $('runtime-state').title = runtimeStatus;
  $('runtime-state').classList.toggle('inactive', !state?.ready);
  $('runtime-state').classList.toggle('loading', loading);
  // #123: plain words in the header; the weight format (Q8_0) is the tooltip.
  const headerModel = state?.models.find(model => model.id === (switching(state) ? state.job_model : state.active_id || state.job_model));
  $('runtime-name').textContent = headerModel ? `${headerModel.group_name || headerModel.name} · ${t(plainVariant(headerModel))}` : modelLabel(headerModel);
  $('runtime-name').title = modelLabel(headerModel);
  const processor = state?.execution?.active === 'gpu' ? 'gpu' : 'cpu', answer = headerModel?.verdict?.seconds?.[processor];
  const speedOn = state?.ready && knownNumber(answer);
  $('runtime-speed').textContent = speedOn ? `~${formatNumber(Math.max(1, Math.round(answer)))} s` : '';
  $('runtime-speed').title = speedOn ? t(`about ${Math.max(1, Math.round(answer))} s per answer on the ${processor === 'gpu' ? 'graphics chip' : 'processor'}`) : '';
  renderActivity();
  $('language-choice').disabled = !!controller;
}

const ringMarkup = '<svg viewBox="0 0 36 36" aria-hidden="true"><circle class="ring-track" cx="18" cy="18" r="14"/><circle class="ring-fill" cx="18" cy="18" r="14" pathLength="100"/><path class="ring-check" d="m12 18 4 4 8-8"/><path class="ring-download" d="M18 7v16m-6-6 6 6 6-6M8 25v4h20v-4"/><path class="ring-pause" d="M15 13v10m6-10v10"/><path class="ring-resume" d="m15 12 9 6-9 6z"/><circle class="ring-active" cx="18" cy="18" r="6"/></svg>';
function downloadState(model, current = state) {
  const total = Math.max(0, model?.bytes || 0);
  const transferring = current?.job_model === model?.id && !!current?.phase;
  const checking = transferring && current.phase === 'verifying';
  const received = Math.max(0, transferring ? current.received || 0 : model?.partial || 0);
  const progress = total ? Math.min(100, received / total * 100) : 0;
  const percent = Math.floor(progress);
  if (checking) return {stage:'verifying', percent:null, text:'Checking model…'};
  if ((transferring && current.phase === 'preparing') || (current?.loading && current.active_id === model?.id)) return {stage:'loading', percent:null, text:'Loading model'};
  if (transferring) return {stage:'downloading', progress, percent, text:`Downloading · ${percent}%`};
  if (model?.installed) return {stage:'downloaded', percent:100, text:'Downloaded'};
  if (received) return {stage:'paused', progress, percent, text:`Paused · ${percent}%`};
  return {stage:'missing', percent:0, text:'Not downloaded'};
}
function renderRing(element, model, current = state) {
  const status = downloadState(model, current);
  if (!element.firstChild) element.innerHTML = ringMarkup; // Fixed, local markup only.
  const progressValue = status.progress ?? status.percent ?? 72;
  const previousValue = Number(element.style.getPropertyValue('--ring-progress'));
  const sameModel = element.dataset.model === model?.id;
  // Interpolate only towards bytes already received, over one polling interval.
  // Repeated renders must not interrupt a transition. Pause, reset, model changes
  // and verified completion snap to their actual state instead of trailing it.
  if (!sameModel || element.dataset.stage !== status.stage || previousValue !== progressValue) {
    element.dataset.animate = String(sameModel && element.dataset.stage === 'downloading' &&
      status.stage === 'downloading' && progressValue > previousValue);
  }
  element.dataset.model = model?.id || '';
  element.dataset.stage = status.stage;
  element.style.setProperty('--ring-duration', `${statusPollInterval}ms`);
  element.style.setProperty('--ring-progress', progressValue);
  const progress = ['downloading', 'paused', 'verifying', 'loading'].includes(status.stage);
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

// These are application capabilities, not claims about the model architecture.
const capabilityIcons = {
  chat: {label:'Text chat', path:'<path d="M4 4h16v12H9l-5 4z"/><path d="M8 8h8m-8 4h5"/>'},
  speech_recognition: {label:'Speech recognition', path:'<rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3m-4 0h8"/>'},
  vision: {label:'Image understanding', path:'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8" cy="8" r="1"/><path d="m3 17 6-6 4 4 3-3 5 5"/>'}
};
const fitIcons = [
  '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 1v4m6-4v4M9 19v4m6-4v4M1 9h4m-4 6h4m14-6h4m-4 6h4m-10-3 2 2 4-4"/>',
  '<path d="m12 3 10 18H2zM12 9v5m0 3v.01"/>',
  '<circle cx="12" cy="12" r="9"/><path d="m6 6 12 12"/>'
];
// #102: reference benchmark counts from the catalog (bench.py, one reference machine).
// A symbol with the share of correct answers; the words are in the tooltip.
const qualityIcon = '<path d="M10 6h10M10 12h10M10 18h10"/><path d="m3 6 1.5 1.5L7 5m-4 7 1.5 1.5L7 11m-4 7 1.5 1.5L7 17"/>';
const benchmarkTasks = {classify:'Classification', extract:'Data extraction', format:'Following a format', context:'Facts from context'};
function qualityCounts(model) {
  const tasks = Object.values(model?.quality_evidence?.tasks || {});
  const sum = language => tasks.reduce((a, c) => [a[0] + c[language][0], a[1] + c[language][1]], [0, 0]);
  const de = sum('de'), en = sum('en');
  return tasks.length ? {de, en, passed: de[0] + en[0], total: de[1] + en[1]} : null;
}
function qualityText(model) {
  const q = model.quality_evidence, c = qualityCounts(model);
  if (!c) return t('Answer quality not tested yet');
  return [`${t('Reference test')}: ${c.passed}/${c.total} ${t('correct')} (DE ${c.de.join('/')} · EN ${c.en.join('/')})`,
    ...Object.entries(q.tasks).map(([k, v]) => `${t(benchmarkTasks[k] || k)} ${v.de[0] + v.en[0]}/${v.de[1] + v.en[1]}`),
    ...(q.suite === state?.quality_suite ? [] : [t('older test version')])].join(' · ');
}
// #103 layer 1: one verdict per model. Symbols first; the words are the tooltip
// and the accessible name, in plain language (no t/s, no weight format).
const verdictSymbols = {good:'✓', usable:'◐', not_recommended:'✗', unknown:'?'};
const verdictNames = {good:'Good choice', usable:'Usable, with limits', not_recommended:'Not recommended here', unknown:'Not measured yet'};
const plainModel = model => `${model.group_name || model.name} (${t(plainVariant(model))})`;
function verdictReason(model) {
  const v = model.verdict, time = v.processor ? v.seconds[v.processor] : null;
  const speed = time === null ? null : t(`about ${Math.max(1, Math.round(time))} s per answer on the ${v.processor === 'gpu' ? 'graphics chip' : 'processor'}`) +
    (v.basis === 'estimated' ? ` (${t('estimated, download to measure')})` : '');
  const correct = v.total ? `${v.passed}/${v.total} ${t('correct')}` : null;
  return {good: [speed, correct], slow: [speed, correct], too_slow: [speed], tight_memory: [t('Memory is tight on this computer'), speed],
    unavailable: [t(model.reason || 'Unavailable on this computer')], unreliable: [correct && `${t('Reference test')}: ${correct}`],
    quality_unknown: [t('Answer quality not tested yet')], speed_unknown: [t('Speed on this computer not measured yet'), correct]}[v.reason] || [];
}
// #103: what a verdict rests on: the measurement here (date, processor), the
// estimate (from how many measured models) and the reference test (date, engine).
const referenceText = (model, r) => `${t('Reference, not this computer')}: ${model.group_name || model.name} · ${r.platform} · ${r.backend.toUpperCase()} · ${secondsText(r.answer_ms / 1000)} · ${r.date}`;
function verdictBasis(model) {
  const v = model?.verdict, q = model?.quality_evidence;
  if (!v) return '';
  const day = seconds => new Date(seconds * 1000).toLocaleDateString(interfaceLanguage === 'de' ? 'de-DE' : 'en-US');
  const measured = ['cpu', 'gpu'].filter(p => knownNumber(v.measured_at?.[p])).map(p => `${p.toUpperCase()} ${day(v.measured_at[p])}`);
  const speed = v.basis === 'estimated' ? `${t('Speed estimated from measured models')}: ${v.estimated_from}`
    : measured.length ? `${t('Speed measured on this computer')}: ${measured.join(', ')}` : t('Speed on this computer not measured yet');
  const quality = q ? `${t('Reference test')}: ${q.date} · ${t('engine')} ${q.engine}${q.suite === state?.quality_suite ? '' : ` · ${t('older test version')}`}`
    : t('Answer quality not tested yet');
  return [speed, quality, ...(model.reference || []).map(r => referenceText(model, r))].join(' · ');
}
function verdictText(model) {
  if (!model?.verdict) return '';
  return [t(verdictNames[model.verdict.value]), ...verdictReason(model)].filter(Boolean).join(' · ');
}
// #122: one recommendation, from the service's best_choice: the sentence above
// the list, the main action in the empty pane and the card tag all follow it.
function recommendation(snapshot) {
  const best = snapshot?.best_choice, model = best && snapshot.models.find(m => m.id === best.id);
  if (!model) return {text: t('No model fits this computer well.'), short: t('No model fits this computer well.'), model: null, action: null};
  const reason = verdictReason(model).filter(Boolean).join(' · ');
  const text = best.verdict === 'good'
    ? `${verdictSymbols.good} ${t('Recommended here')}: ${plainModel(model)}${reason ? ` · ${reason}` : ''}`
    : `${verdictSymbols[best.verdict]} ${t('Best option here')}: ${plainModel(model)} · ${verdictText(model)}`;
  const action = best.action === 'active' ? null : best.action === 'start' ? t('Start') : `${t('Download')} (${bytes(model.bytes)})`;
  // #125: the sidebar shows the short form; the reasons are its tooltip.
  const short = `${verdictSymbols[best.verdict]} ${t(best.verdict === 'good' ? 'Recommended here' : 'Best option here')}: ${plainModel(model)}`;
  return {text, short, model, action, verdict: best.verdict};
}
function renderBestChoice(snapshot) {
  const r = recommendation(snapshot);
  $('best-choice').hidden = false;
  $('best-choice').dataset.verdict = r.verdict || 'none';
  uiText($('best-choice'), r.short);
  $('best-choice').title = r.text;
  for (const id of ['best-action', 'recommend-action']) {
    $(id).hidden = !r.action || !!snapshot.phase || !!snapshot.loading;
    $(id).textContent = r.action || '';
    $(id).dataset.id = r.model?.id || '';
  }
  $('model-prompt-hint').hidden = !r.model;
  uiText($('model-prompt-hint'), r.model ? r.text : '');
}
// #103 layer 3: compare installed models. Chart and table show the same values;
// the table is the accessible equivalent. Markup is built from numbers and
// escaped catalog names only.
const escapeText = text => String(text).replace(/[&<>"']/g, c => `&#${c.charCodeAt(0)};`);
const secondsText = value => value === null || value === undefined ? '–' : `${formatNumber(value, value < 10 ? 1 : 0)} s`;
function renderCompare() {
  if (!$('compare-dialog').open || !state) return;
  const limits = state.limits || {fast_s: 10, usable_s: 30, reliable: .9, intent: 'chat'};
  const models = state.models.filter(m => m.installed && m.verdict);
  const points = models.flatMap(m => ['cpu', 'gpu'].filter(p => knownNumber(m.verdict.seconds[p]) && m.verdict.total)
    .map(p => ({m, p, x: m.verdict.seconds[p], y: m.verdict.passed / m.verdict.total})));
  // #104: speed on reference platforms, a faint labelled mark, never "this computer".
  const references = models.flatMap(m => (m.reference || []).filter(r => m.verdict.total)
    .map(r => ({m, r, x: r.answer_ms / 1000, y: m.verdict.passed / m.verdict.total})));
  const W = 340, H = 230, L = 40, R = 10, T = 10, B = 30;
  const xmax = Math.max(limits.usable_s * 1.25, ...points.map(p => p.x * 1.1), ...references.map(p => p.x * 1.1));
  const sx = x => L + (W - L - R) * Math.min(x, xmax) / xmax, sy = y => T + (H - T - B) * (1 - y);
  const tick = (x1, y1, x2, y2, text, anchor, tx, ty) => `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="compare-grid"/><text x="${tx}" y="${ty}" text-anchor="${anchor}">${escapeText(text)}</text>`;
  $('compare-chart').innerHTML =
    `<rect class="compare-zone" x="${sx(0)}" y="${sy(1)}" width="${sx(limits.fast_s) - sx(0)}" height="${sy(limits.reliable) - sy(1)}"/>` +
    `<line class="compare-usable" x1="${sx(limits.usable_s)}" y1="${sy(1)}" x2="${sx(limits.usable_s)}" y2="${sy(0)}"/>` +
    `<path class="compare-axis" d="M${L} ${T}V${sy(0)}H${W - R}"/>` +
    [0, limits.fast_s, limits.usable_s].map(x => tick(sx(x), sy(0), sx(x), sy(0) + 4, `${formatNumber(x)} s`, 'middle', sx(x), sy(0) + 15)).join('') +
    [0, .5, limits.reliable, 1].filter((y, i, all) => all.indexOf(y) === i).map(y => tick(L - 4, sy(y), L, sy(y), `${Math.round(y * 100)}%`, 'end', L - 6, sy(y) + 3)).join('') +
    `<text x="${W - R}" y="${H - 2}" text-anchor="end">${escapeText(t('seconds per typical answer'))}</text>` +
    references.map(({m, r, x, y}) => `<circle class="compare-reference" cx="${sx(x)}" cy="${sy(y)}" r="5"><title>${escapeText(referenceText(m, r))}</title></circle>`).join('') +
    points.map(({m, p, x, y}) => `<g class="compare-point" data-verdict="${m.verdict.value}" data-id="${escapeText(m.id)}"><circle cx="${sx(x)}" cy="${sy(y)}" r="5"/><text x="${sx(x) + 7}" y="${sy(y) + 3}">${escapeText(`${m.group_name || m.name} · ${p.toUpperCase()}`)}</text><title>${escapeText(`${modelLabel(m)} · ${p.toUpperCase()} · ${secondsText(x)} · ${m.verdict.passed}/${m.verdict.total}`)}</title></g>`).join('');
  $('compare-table').tBodies[0].innerHTML = models.length ? models.map(m => `<tr data-id="${escapeText(m.id)}" tabindex="-1"><td><span class="model-verdict" data-verdict="${m.verdict.value}" title="${escapeText(verdictText(m))}">${verdictSymbols[m.verdict.value]}</span><span class="sr-only">${escapeText(verdictText(m))}</span></td><th scope="row">${escapeText(modelLabel(m))}</th><td>${secondsText(m.verdict.seconds.cpu)}</td><td>${secondsText(m.verdict.seconds.gpu)}</td><td>${m.verdict.total ? `${m.verdict.passed}/${m.verdict.total}` : '–'}</td><td><span class="compare-basis" title="${escapeText(verdictBasis(m))}" aria-hidden="true">ⓘ</span><span class="sr-only">${escapeText(verdictBasis(m))}</span></td></tr>`).join('')
    : `<tr><td colspan="6">${escapeText(t('No model installed yet.'))}</td></tr>`;
  const fields = {'verdict-intent': limits.intent, 'verdict-fast': limits.fast_s, 'verdict-usable': limits.usable_s, 'verdict-reliable': Math.round(limits.reliable * 100)};
  for (const [id, value] of Object.entries(fields)) if (document.activeElement !== $(id)) $(id).value = value;
}
async function saveVerdictSettings(body) {
  try { await api('/app/verdict-settings', body); uiText($('verdict-result'), 'Saved.'); await poll(); }
  catch(error) { uiText($('verdict-result'), error.message); }
}
function renderModelLegend() {
  const icon = path => `<svg viewBox="0 0 24 24" aria-hidden="true">${path}</svg>`;
  $('model-legend-items').innerHTML = `<span>${icon(fitIcons[0])}<span data-ui-text="Fits this computer"></span></span><span>${icon(capabilityIcons.chat.path)}<span data-ui-text="Text chat"></span></span><span>${icon(qualityIcon)}<span data-ui-text="Correct answers in the reference test"></span></span>` +
    Object.entries(verdictSymbols).map(([key, symbol]) => `<span><span class="model-verdict" data-verdict="${key}" aria-hidden="true">${symbol}</span><span data-ui-text="${verdictNames[key]}"></span></span>`).join('');
  $('model-legend').querySelectorAll('[data-ui-text]').forEach(e => { e.textContent = t(e.dataset.uiText); });
}
function renderModelBadges(element, model) {
  const fit = [0, 1, 2].includes(model.resource_fit) ? model.resource_fit : 1;
  const label = ['Fits this computer', 'Limited on this computer', 'Unavailable on this computer'][fit];
  const enabled = Object.entries(capabilityIcons).filter(([key]) => model.capabilities?.[key] === true);
  const counts = qualityCounts(model);
  const tested = counts ? model.quality_evidence.suite === state?.quality_suite ? 'current' : 'older' : 'none';
  const share = counts ? `${Math.round(100 * counts.passed / counts.total)}%` : '';
  const signature = `${fit}:${enabled.map(([key]) => key).join(',')}:${tested}:${share}`;
  if (element.dataset.icons !== signature) {
    // All markup is local and allowlisted; metadata text is never interpreted as HTML.
    element.innerHTML = `<span class="model-fit" data-fit="${fit}"><svg viewBox="0 0 24 24" aria-hidden="true">${fitIcons[fit]}</svg></span>` + enabled.map(([key, icon]) => `<span data-capability="${key}"><svg viewBox="0 0 24 24" aria-hidden="true">${icon.path}</svg></span>`).join('') +
      `<span class="model-quality" data-tested="${tested}"><svg viewBox="0 0 24 24" aria-hidden="true">${qualityIcon}</svg>${share}</span><span class="model-seconds"></span>`;
    element.insertAdjacentHTML('afterbegin', '<span class="model-size"></span>');
    element.dataset.icons = signature;
  }
  const fitText = `${t(label)}${model.reason ? `: ${fitReason(model)}` : ''}`;
  element.querySelector('.model-fit').title = fitText;
  for (const [key, icon] of enabled) element.querySelector(`[data-capability="${key}"]`).title = t(icon.label);
  element.querySelector('.model-quality').title = qualityText(model);
  // #121: the second row: size · share · seconds per typical answer (measured or estimated).
  // A single variant names its weight format here; in a group the row title does.
  const single = element.closest('.model')?.dataset.single === 'true';
  element.querySelector('.model-size').textContent = [single ? t(plainVariant(model)) : '', model.bytes ? bytes(model.bytes) : ''].filter(Boolean).join(' · ');
  const v = model.verdict, seconds = v?.processor ? v.seconds?.[v.processor] : null;
  element.querySelector('.model-seconds').textContent = knownNumber(seconds) ? `~${formatNumber(Math.max(1, Math.round(seconds)))} s` : '';
  element.querySelector('.model-seconds').title = knownNumber(seconds) ? verdictReason(model)[0] || '' : '';
  const specification = model.bytes ? t(`${bytes(model.bytes)} download · ${model.ram_gib} GiB RAM guidance`) : t('Local model');
  const description = [fitText, ...enabled.map(([, icon]) => t(icon.label)), qualityText(model), specification].join(' · ');
  element.title = description;
  element.setAttribute('aria-label', description);
}
const shortFitReasons = {
  'This model requires PQ2_0 and Hadamard support, unavailable in the bundled engine.': 'Unsupported format',
  'This CPU instruction set or platform is not supported by the bundled engine.': 'Unsupported platform',
  'Not enough disk space for the download plus 256 MiB reserve.': 'Not enough disk space',
  'RAM is smaller than the model file, before context and OS memory.': 'Not enough RAM',
  'Below the RAM recommendation; swapping or allocation failures are possible.': 'Below recommended RAM',
  'Available RAM is tight now. Close other apps before loading this model.': 'Available RAM is tight',
  'Last replies were below 8 tokens/s on every available processor. Slower tasks remain possible.': 'Slow on available processors'
};
function renderModelGroups(models) {
  const focused = document.activeElement;
  const grouped = new Map();
  for (const model of models) {
    const id = model.group_id || model.id;
    if (!grouped.has(id)) grouped.set(id, []);
    grouped.get(id).push(model);
  }
  // #57: one order for one catalog: the platform default group first, then catalog
  // order. It never depends on selection, downloads, timing or language, so a row
  // never moves under the pointer; only a catalog change can reorder groups.
  const preferred = state.recommendation?.preferred_id;
  const ordered = [...grouped].sort((a, b) => (b[1].some(m => m.id === preferred)) - (a[1].some(m => m.id === preferred)));
  ordered.forEach(([id], index) => { const group = modelGroups.get(id); if (group && $('models').children[index] !== group) $('models').insertBefore(group, $('models').children[index] || null); });
  for (const [id, variants] of ordered) {
    let group = modelGroups.get(id);
    if (!group) {
      group = document.createElement('section'); group.className = 'model-group'; group.dataset.group = id;
      const heading = document.createElement('h3'); heading.id = `model-group-${++groupSequence}`;
      const rows = document.createElement('div'); rows.className = 'model-variants'; rows.setAttribute('role', 'list');
      group.setAttribute('aria-labelledby', heading.id); group.append(heading, rows);
      modelGroups.set(id, group); $('models').insertBefore(group, $('models').children[ordered.findIndex(([key]) => key === id)] || null);
    }
    group.querySelector('h3').textContent = variants[0].group_name || variants[0].name;
    group.classList.toggle('single', variants.length === 1);
    const rows = group.querySelector('.model-variants');
    variants.forEach((model, index) => {
      modelCard(model);
      const card = cards.get(model.id);
      if (rows.children[index] !== card) rows.insertBefore(card, rows.children[index] || null);
    });
  }
  for (const [id, group] of modelGroups) if (!grouped.has(id)) { group.remove(); modelGroups.delete(id); }
  // A catalog import may regroup an existing row; ordinary polls never move it.
  if (focused?.isConnected && focused !== document.activeElement && !focused.disabled) focused.focus({preventScroll:true});
}
function canPause(model) {
  return state?.job_model === model.id && state.phase === 'downloading' && !state.loading;
}
function modelActionDisabled(model) {
  if (stopped || requesting || cancellingModel === model.id) return true;
  if (canPause(model)) return false;
  if (state.comparison?.running) return true; /* the service refuses model actions during a comparison (#83) */
  if (state.loading || state.phase || model.resource_fit === 2) return true;
  if (state.ready && state.active_id === model.id && allowed(model)) return true;
  // A missing, different artifact can download during a resident answer.
  if (!model.installed && state.ready && state.active_id !== model.id &&
      typeof state.inference_busy === 'boolean') return false;
  return !!(state.busy || controller || connectionTesting);
}
function modelCard(model) {
  let card = cards.get(model.id);
  if (!card) {
    card = document.createElement('article'); card.className = 'model'; card.dataset.id = model.id; card.setAttribute('role', 'listitem');
    // One button covers the name and download state. Information and removal are siblings.
    card.innerHTML = '<button class="model-pick" type="button"><span class="model-ring"></span><span class="model-info"><span class="model-title"><span class="model-name"></span><span class="variant-recommended" hidden></span><span class="variant-active"></span><span class="model-verdict" aria-hidden="true"></span></span><span class="variant-size sr-only"></span><span class="download-state"></span></span></button><span class="model-badges" role="img" tabindex="0"></span><button class="measure text-button icon-button" type="button" hidden><svg class="measure-start" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="13" r="8"/><path d="M12 9v4l2 2M9 2h6"/></svg><svg class="measure-stop" viewBox="0 0 24 24" aria-hidden="true"><rect x="7" y="7" width="10" height="10" rx="1"/></svg></button><span class="variant-warning"></span><span class="transfer-detail"></span><button class="remove text-button icon-button" type="button"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M10 10v7m4-7v7"/></svg></button>';
    card.querySelector('.model-pick').addEventListener('click', event => { if (event.detail < 2) choose(model.id); });
    card.querySelector('.model-pick').addEventListener('keydown', event => {
      if (event.repeat && (event.key === 'Enter' || event.key === ' ')) event.preventDefault();
    });
    card.querySelector('.remove').addEventListener('click', () => removeModel(model.id));
    card.querySelector('.measure').addEventListener('click', () => measureSpeed(model.id));
    cards.set(model.id, card);
  }
  const active = state.active_id === model.id && state.ready && !state.loading && !switching(state);
  const pending = pendingModel === model.id;
  const preparing = state.job_model === model.id && (!!state.phase || state.loading);
  const paused = canPause(model);
  card.className = `model${active ? ' active' : ''}${preparing || pending ? ' preparing' : ''}${model.resource_fit === 2 ? ' unavailable' : ''}`;
  // #121: one row per model; the group name joins the row when there is no second variant.
  const single = state.models.filter(m => (m.group_id || m.id) === (model.group_id || model.id)).length === 1;
  card.querySelector('.model-name').textContent = single ? model.group_name || model.name : t(plainVariant(model));
  card.dataset.single = single;
  const verdict = card.querySelector('.model-verdict');
  verdict.dataset.verdict = model.verdict?.value || 'unknown';
  verdict.textContent = verdictSymbols[verdict.dataset.verdict];
  verdict.title = `${verdictText(model)}\n${verdictBasis(model)}`;
  card.querySelector('.variant-size').textContent = [variantLabel(model), model.bytes ? bytes(model.bytes).replace(' ', '\u00a0') : ''].filter(Boolean).join(' · ');
  const recommendedTag = card.querySelector('.variant-recommended');
  recommendedTag.hidden = recommendedModel(state)?.id !== model.id || active;
  recommendedTag.textContent = t('Suggested start');
  card.querySelector('.variant-active').textContent = t('Active');
  card.querySelector('.variant-active').hidden = !active;
  const warning = card.querySelector('.variant-warning');
  warning.hidden = !model.resource_fit;
  warning.textContent = model.resource_fit ? t(shortFitReasons[model.reason] || model.reason || 'Limited on this computer') +
    (ramReasons.has(model.reason) && model.ram_gib ? ` · ${t(`${model.ram_gib} GiB RAM recommended`)}` : '') : '';
  warning.title = model.resource_fit ? t(model.reason || 'Limited on this computer') : '';
  const download = renderRing(card.querySelector('.model-ring'), model);
  // The button's complete name exposes status; a duplicate nested progress role
  // is unnecessary to screen readers. Numeric ring attributes remain inspectable.
  card.querySelector('.model-ring').setAttribute('aria-hidden', 'true');
  card.querySelector('.model-ring').title = t(download.text);
  const status = cancellingModel === model.id ? 'Cancelling…' : pending ? 'Getting ready…' : preparing && state.loading ? 'Loading model' : download.text;
  card.querySelector('.download-state').textContent = t(status);
  card.querySelector('.download-state').hidden = !pending && !preparing && (download.stage === 'missing' || download.stage === 'downloaded');
  renderModelBadges(card.querySelector('.model-badges'), model);
  // #103: measure on demand; the same button cancels its own run.
  const measure = card.querySelector('.measure'), running = !!state.comparison?.running && state.active_id === model.id;
  measure.hidden = !running && !(model.installed && model.resource_fit !== 2 && model.verdict?.processor === null);
  measure.disabled = !running && (stopped || requesting || state.loading || !!state.phase || state.busy || !!state.comparison?.running || measuringModel !== null);
  measure.dataset.running = running;
  measure.title = t(running ? 'Cancel speed measurement' : 'Measure speed on this computer');
  measure.setAttribute('aria-label', `${measure.title}: ${modelLabel(model)}`);
  if (running) { card.querySelector('.download-state').hidden = false; card.querySelector('.download-state').textContent = `${t('Measuring speed')} · ${state.comparison.step}/${state.execution?.gpu_available ? 8 : 4}`; }
  const detail = card.querySelector('.transfer-detail');
  detail.hidden = !preparing || state.loading;
  detail.textContent = paused ? downloadEstimate(model.id, state.received || 0, model.bytes || 0) : preparing ? t(state.phase === 'verifying' ? 'Checking model…' : 'Getting ready…') : '';
  const button = card.querySelector('.model-pick');
  const action = paused ? 'Pause download' : active && allowed(model) ? 'Active' : model.installed ? 'Start model' : model.partial ? 'Resume download' : state.ready ? 'Download model' : 'Download and start';
  button.title = model.resource_fit === 2 ? `${modelLabel(model)} · ${t(model.reason || 'Unavailable on this computer')}` : `${t(action)}: ${modelLabel(model)}`;
  button.setAttribute('aria-label', `${t(action)}: ${modelLabel(model)} · ${verdictText(model)} · ${t(status)}${model.resource_fit ? ` · ${t(model.reason || 'Limited on this computer')}` : ''}`);
  button.disabled = modelActionDisabled(model);
  const remove = card.querySelector('.remove');
  remove.title = `${t('Remove download')}: ${modelLabel(model)}`;
  remove.hidden = model.id === 'custom' || (!model.installed && !model.partial);
  remove.disabled = state.busy || state.loading || !!state.phase || requesting || !!controller || connectionTesting;
  remove.setAttribute('aria-label', `${t('Remove download')}: ${modelLabel(model)}`);
}

function visibleModels() {
  const models = state?.models || [];
  return state?.active_id === 'custom' && state.ready ? [{id:'custom', name:state.active, installed:true, resource_fit:0, capabilities:{chat:true}}, ...models] : models;
}
let shownActive = ''; // last active model scrolled into view (#50)
function render(next) {
  if (cancellingModel && next.job_model !== cancellingModel) cancellingModel = null; // the paused job has ended (#83)
  const modelChanged = state && (state.active_id !== next.active_id || state.active !== next.active);
  state = next;
  stateReceivedAt=performance.now();
  acceptActivity(next.activity);
  if (!languageInitialized) {
    $('language-choice').value = next.answer_language || interfaceLanguage;
    languageInitialized = true;
  }
  const working = !!next.phase || next.loading;
  const active = next.models.find(m => m.id === next.active_id);
  const usable = next.ready && allowed(active);
  if (usable) workspaceModel = workspaceIdentity();
  const retained = workspaceModel !== null && ((workspaceModel === workspaceIdentity() && !!active && allowed(active)) || working);
  if (modelChanged || (!usable && !retained)) closeMeasurements();
  const previouslyHidden = $('workspace').hidden;
  $('workspace').hidden = !usable && !retained && !(next.activity?.load && !next.activity.load.outcome);
  $('test-unavailable').hidden = !$('workspace').hidden;
  // #125: in a narrow window the model list folds away once a model is ready;
  // after the first toggle the user's choice wins.
  const folded = modelsFoldedByUser ?? !$('workspace').hidden;
  document.body.classList.toggle('models-folded', folded);
  $('toggle-models').setAttribute('aria-expanded', String(!folded));
  $('model-prompt').textContent = t(working ? 'Getting ready…' : 'Choose a model to begin.');
  $('model-legend').hidden = !!next.active_id;
  renderBestChoice(next);
  continueMeasurement(next);
  renderCompare();
  if (usable && previouslyHidden && !$('workspace').hidden && !$('models-page').hidden &&
      (document.activeElement === document.body || document.activeElement.closest('.model-pick'))) $('prompt').focus({preventScroll:true});
  $('disk-space').textContent = t(next.hardware.disk_known ? `${bytes(next.hardware.disk)} disk space available` : 'Disk space could not be read');
  const models = visibleModels();
  for (const [id, card] of cards) if (!models.some(model => model.id === id)) { card.remove(); cards.delete(id); }
  $('catalog-revision').textContent = next.catalog_revision ? `${t('Version')} ${next.catalog_revision}` : ''; // #56: not a bare #N
  $('catalog-file').disabled = requesting || next.busy || next.loading || !!next.phase || !!controller;
  $('catalog-choose').disabled = $('catalog-file').disabled;
  if (!working) { transfer.id = ''; transfer.samples = []; }
  renderModelGroups(models);
  // Stacked layout (#50): bring the newly active card into the list's view once, not on every poll.
  if (next.active_id && next.active_id !== shownActive) {
    shownActive = next.active_id;
    document.querySelector(`.model[data-id="${CSS.escape(next.active_id)}"]`)?.scrollIntoView({block: 'nearest'});
  }
  // #82: remember a server notice only once it is shown, so one that arrives
  // during a reply or a request still appears afterwards.
  if (!controller && !requesting && (!localMessage || next.message !== lastServerMessage)) {
    message(working ? '' : next.message || '', false);
    lastServerMessage = working ? '' : next.message;
  }
  buttonStates(); chatLayout();
}

async function poll() {
  if (polling || stopped) return;
  polling = true;
  try { const next = await (await api('/app/status')).json(); if (!stopped) render(next); }
  // Not a local message: the first successful poll replaces it (#82).
  catch (error) { if (!stopped) { message('Service unavailable. Reopen geisten to reconnect.', false); lastServerMessage = ''; state = null; buttonStates(); } }
  finally { polling = false; }
}

// #103: the existing controlled comparison, for the chosen model. Another
// installed model is started the normal (verified) way first; render() starts
// the measurement once it is ready.
async function measureSpeed(id) {
  try {
    if (state.comparison?.running) { await api('/app/performance/cancel', {}); await poll(); return; }
    if (state.active_id === id && state.ready) { await api('/app/performance/compare', {confirm: true}); await poll(); return; }
    measuringModel = id; await choose(id);
  } catch(error) { measuringModel = null; message(error.message); }
}
function continueMeasurement(snapshot) {
  if (measuringModel === null || pendingModel !== null || snapshot.loading || snapshot.job_model) return;
  const id = measuringModel; measuringModel = null;
  if (snapshot.active_id === id && snapshot.ready && !snapshot.busy) measureSpeed(id);
}
async function choose(id) {
  const model = visibleModels().find(item => item.id === id);
  if (!model || modelActionDisabled(model)) return;
  const pause = canPause(model);
  // #51: say the size before a multi-GB download starts; a resume or a small model starts directly.
  if (!pause && !model.installed && !model.partial && model.bytes > 1e9 &&
      !confirm(t(`Download ${modelLabel(model)} (${bytes(model.bytes)})? You can pause or remove it later.`))) return;
  downloadRequest = !!state.ready && state.active_id !== id && (pause || !model.installed);
  requesting = true; pendingModel = pause ? null : id; if (pause) cancellingModel = id; buttonStates(); visibleModels().forEach(modelCard); message('', false);
  try {
    if (pause) {
      await api('/app/cancel', {});
      message('Cancelling…');
      return;
    }
    // A deliberate row action enables the local test for this exact model hash.
    // It does not mark response quality as approved.
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
    requesting = false; downloadRequest = false; pendingModel = null;
    if (state) render(state); else buttonStates();
  }
}


async function run(prompt, benchmark = false, preserveDraft = false) {
  prompt = prompt.trim();
  const task = tasks.find(t => t.id === 'freeform');
  if (!prompt || controller || runtimeRequest() || connectionTesting || inferenceBusy() || !state?.ready || !task || !allowed(state.models.find(m => m.id === state.active_id), task)) return;
  if (new TextEncoder().encode(prompt).length > task.input_limit) { message('Your message is too long. Shorten it before sending; your draft has been kept.'); return; }
  const experimental = previewAccepted(state.models.find(m => m.id === state.active_id));
  const messages = [...conversation, {role: 'user', content: prompt}];
  const payload = {prompt, benchmark, language: $('language-choice').value, experimental, task: 'freeform', task_version: task.version,
    // #82: a conversation belongs to its model; after a switch the server's guard asks to clear it.
    ...(!benchmark ? {model: conversation.length ? conversationModel : state.active_id, messages} : {})};
  if (!benchmark && (messages.length > 63 || new TextEncoder().encode(JSON.stringify(payload)).length > 32768)) {
    message('This test is full. Use Clear chat to start again. The existing text has been kept.'); return;
  }
  const activeController = new AbortController(); controller = activeController; requestAfter=activitySnapshot?.request?.id || 0;
  const turn = benchmark ? null : addTurn(prompt); activeTurn=turn;
  const requestModel = modelIdentity();
  if (turn) { lastReply = null; replyPending = true; }
  const target = turn.output;
  target.hidden = false; target.textContent = '';
  if (!benchmark) { if (!preserveDraft) $('prompt').value = ''; $('chat-help').open = false; closeMeasurements(); resizeComposer(); $('prompt').focus(); }
  buttonStates(); visibleModels().forEach(modelCard); message('');
  uiText($('chat-announcement'), 'Sending…');
  const start = performance.now(); let first = null, done = false, reader, completion = null;
  let output = '', pending = '', limited = false, paintTimer = null;
  // #93: what happens before the answer: reading the input, then thinking (tokens, time, rate).
  let inputTokens = 0, thinkingStart = null, thoughtTokens = 0, ticker = null;
  function preparing() {
    if (!turn || first !== null) return;
    if (thinkingStart === null) { turn.status.textContent = inputTokens ? `${t('Reading your input')} · ${formatNumber(inputTokens)} ${t('tokens')}…` : `${t('Reading your input')}…`; return; }
    const seconds = (performance.now() - thinkingStart) / 1000;
    const rate = seconds >= 1 && thoughtTokens ? ` · ${formatNumber(thoughtTokens / seconds, 1)} t/s` : '';
    turn.status.textContent = `${t('Thinking')} · ${clock(seconds)} · ${formatNumber(thoughtTokens)} ${t('tokens')}${rate}`;
  }
  function paint() { paintTimer = null; if (turn) updateMarkdown(target, output); else target.textContent = output; }
  function event(line) {
    if (!line.trim()) return;
    const item = JSON.parse(line);
    if (item.error) throw new Error(typeof item.error === 'string' ? item.error : 'The model returned an error.');
    if ((item.phase === 'tokenize' || item.phase === 'prefill') && first === null && turn) {
      if (Number.isSafeInteger(item.tokens) && item.tokens > 0) inputTokens = item.tokens;
      preparing();
    }
    if (item.phase === 'preparing' && first === null && turn) {
      if (thinkingStart === null) { thinkingStart = performance.now(); uiText($('chat-announcement'), 'Thinking…'); ticker = setInterval(preparing, 1000); }
      if (Number.isSafeInteger(item.tokens)) thoughtTokens = item.tokens;
      preparing();
    }
    if (typeof item.thinking === 'string' && turn) appendThinking(turn, item.thinking);
    if (item.response) {
      if (turn) turn.answerSeen=true;
      if (first === null) { first = (performance.now() - start) / 1000; clearInterval(ticker); if (turn) turn.status.textContent = ''; }
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
    const measured = {model: requestModel, tokens,
      first: knownNumber(completion?.first_model_text_ns) ? completion.first_model_text_ns / 1e9 : first,
      firstAnswer: knownNumber(completion?.first_answer_ns) ? completion.first_answer_ns / 1e9 : first,
      reasoning: completion?.reasoning === true, total: (performance.now() - start) / 1000,
      rate: duration > 0 && tokens > 0 ? tokens / duration : null};
    if (turn) { lastReply = measured; turn.metrics.replyMetrics = measured; renderReplyMetrics(turn.metrics); }
    const status = !output.trim() ? 'No answer was produced. Try again with a shorter question.'
      : limited ? 'The model’s context limit was reached. Start a new chat or ask a shorter question.' : '';
    if (turn) {
      uiText(turn.status, status);
      if (!output.trim()) {
        const retry = document.createElement('button'); retry.type = 'button'; retry.className = 'text-button';
        uiText(retry, 'Retry'); retry.addEventListener('click', () => run(prompt, false, $('prompt').value.trim() !== prompt));
        turn.actions.append(retry);
      }
    } else message(status);
    uiText($('chat-announcement'), status || 'Response complete.');
  } catch (error) {
    activeController.abort();
    let status = error.name === 'AbortError' ? (output.trim() ? 'Stopped. Partial output is kept here.' : 'Stopped before an answer was produced.') : error.message;
    if (status.includes("does not fit this model's context")) status = 'This test does not fit the model’s context. Shorten your draft or use Clear chat to start again. No earlier messages have been removed.';
    if (turn) uiText(turn.status, status); else message(status);
    uiText($('chat-announcement'), status);
  } finally {
    clearInterval(ticker);
    clearTimeout(paintTimer); paint();
    if (reader) { try { await reader.cancel(); } catch { /* connection already closed */ } }
    if (turn) {
      // Partial answers are visible and explicitly marked, so follow-ups can
      // refer to them. Failed requests without text never enter model context.
      if (output.trim()) { if (!conversation.length) conversationModel = payload.model; conversation = [...messages, {role: 'assistant', content: output}]; }
      else if (!preserveDraft && !$('prompt').value) { $('prompt').value = prompt; resizeComposer(); }
      turn.copy.disabled = !output.trim();
      scrollLatest();
    }
    controller = null; activeTurn=null; replyPending = false; buttonStates(); if (state) visibleModels().forEach(modelCard);
    if (stopped) message('geisten is stopping. Reopen the app to start it again.');
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
  conversation = []; conversationModel = ''; lastReply = null; pendingMarkdown.clear(); $('result').replaceChildren(); $('result').hidden = true; $('chat-empty').hidden = false;
  $('prompt').value = ''; $('chat-help').open = false; closeMeasurements(); message(''); uiText($('chat-announcement'), 'Chat cleared.');
  resizeComposer(); buttonStates(); followLatest = true; $('latest').hidden = true; $('transcript').scrollTop = 0; $('prompt').focus();
});
function chooseTask(id) {
  selectedTask = tasks.find(task => task.id === id && !task.url);
  $('prompt').placeholder = t('Message geisten…');
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
$('benchmark').addEventListener('click', async () => {
  if (!confirm(t('Run a short CPU/GPU comparison? Each processor loads once, warms up, then answers three times. Your previous processor setting is restored.'))) return;
  try { await api('/app/performance/compare',{confirm:true}); await poll(); }
  catch(error) { message(error.message); }
});
$('comparison-stop').addEventListener('click', async () => {
  try { await api('/app/performance/cancel',{}); await poll(); }
  catch(error) { message(error.message); }
});
for (const id of ['history-enabled','history-days']) $(id).addEventListener('change', async () => {
  historySaving = true; $('history-enabled').disabled = $('history-days').disabled = true;
  try { await api('/app/performance/settings',{enabled:$('history-enabled').checked,days:Number($('history-days').value)}); uiText($('history-result'),'Saved.'); }
  catch(error) { uiText($('history-result'),error.message); }
  finally { historySaving=false; $('history-enabled').disabled = $('history-days').disabled = false; await poll(); }
});
$('history-clear').addEventListener('click', async () => {
  if (!confirm(t('Delete local measurement history? Models and this chat are kept.'))) return;
  try { await api('/app/performance/clear',{confirm:true}); uiText($('history-result'),'History deleted.'); await poll(); }
  catch(error) { uiText($('history-result'),error.message); }
});
// #101: the export goes where the user can find it. The native windows ask
// where to save (Downloads preset) and fetch the data themselves; a browser
// downloads it. Nothing is written into the data folder.
$('history-export').addEventListener('click', async () => {
  const name = `geisten-measurements-${new Date().toISOString().slice(0, 10)}.jsonl`;
  try {
    if (window.geistDesktop) {
      const saved = await desktopMessage('export', name, 600000);
      if (saved === 'cancelled') { uiText($('history-result'), ''); return; }
      uiText($('history-result'), `Export saved: ${typeof saved === 'string' ? saved : name}`);
    } else {
      const data = await (await api('/app/performance/export')).blob();
      const link = document.createElement('a');
      link.href = URL.createObjectURL(data); link.download = name;
      document.body.append(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(link.href), 60000);
      uiText($('history-result'), `Export saved: ${name}`);
    }
  } catch(error) { uiText($('history-result'), error.message); }
});
document.querySelector('.brand').addEventListener('click', event => { event.preventDefault(); showPage('models-page'); });
document.querySelector('.skip').addEventListener('click', event => { event.preventDefault(); const main = $('main'); main.tabIndex = -1; main.focus(); });
window.addEventListener('beforeunload', () => controller?.abort());
// #60: the Terminal hint names only the host's own command line tool. The Mac
// shell injects the installed geist-cli path; a plain browser gets both.
const terminalHelp = window.geistDesktop === 'mac'
  ? `Paste the copied curl command into your terminal to try the loaded model. The command line tool is at ${window.geistCLIPath || '/Applications/geisten.app/Contents/MacOS/geist-cli'}.`
  : window.geistDesktop === 'linux'
  ? 'Paste the copied curl command into your terminal to try the loaded model. You can also run geisten test and geisten chat.'
  : 'Paste the copied curl command into your terminal to try the loaded model. Ubuntu also installs geisten test and geisten chat. On Mac, the CLI is bundled at /Applications/geisten.app/Contents/MacOS/geist-cli.';
const connectionHelp = {
  terminal: terminalHelp,
  continue: 'In Continue, open your local config.yaml and add the model from this configuration. JSON is valid YAML. Select geisten and use Chat mode. Preserve your existing configuration.',
  opencode: 'Save as opencode.json in a private test folder. Run opencode there and choose geist-chat. This profile disables tools; it does not enable coding-agent workflows.'
};
function updateConnectionHelp() { $('connection-help').textContent = t(connectionHelp[$('connection-client').value]); }
$('connection-client').addEventListener('change', updateConnectionHelp);
updateConnectionHelp();
$('connection-choose').addEventListener('click', () => showPage('models-page'));
$('copy-connection').addEventListener('click', async () => {
  try {
    const c = await (await api('/app/connections')).json();
    if (!c.ready) throw new Error('Load a model first.');
    const kind = $('connection-client').value;
    // The URL in a remote browser is the forwarded origin, not an arbitrary host.
    const base = `${location.origin}/v1`;
    let config;
    if (kind === 'continue') config = {name: 'geisten Local', version: '1.0.0', schema: 'v1', models: [{name: 'geisten', provider: 'openai', model: c.model, apiBase: base, apiKey: c.api_key, roles: ['chat'], capabilities: [], defaultCompletionOptions: {contextLength: 4096, maxTokens: 512}}]};
    else if (kind === 'opencode') config = {$schema: 'https://opencode.ai/config.json', provider: {geist: {npm: '@ai-sdk/openai-compatible', name: 'geisten', options: {baseURL: base, apiKey: c.api_key}, models: {[c.model]: {name: 'geisten local text', tool_call: false, limit: {context: 4096, output: 512}}}}}, model: `geist/${c.model}`, default_agent: 'geist-chat', agent: {'geist-chat': {mode: 'primary', description: 'Local text chat without tools', prompt: 'Answer the user briefly. You cannot access files or execute tools.', permission: {'*': 'deny'}}}};
    else {
      const quote = text => `'${text.replaceAll("'", "'\\''")}'`;
      config = `curl ${quote(`${base}/chat/completions`)} -H ${quote(`Authorization: Bearer ${c.api_key}`)} -H 'Content-Type: application/json' --data ${quote(JSON.stringify({model: c.model, messages: [{role: 'user', content: 'Hello'}], max_tokens: 512}))}`;
    }
    await copyText(typeof config === 'string' ? config : JSON.stringify(config, null, 2));
    uiText($('connection-result'), 'Copied. The configuration contains your private local key.');
  } catch (error) { uiText($('connection-result'), error.message); }
});
$('test-connection').addEventListener('click', async () => {
  connectionTesting = true; buttonStates(); uiText($('connection-result'), 'Asking the loaded model through the editor endpoint…');
  try {
    const result = await (await api('/v1/chat/completions', {model: state.active_id, messages: [{role: 'user', content: 'Say hello in one sentence.'}], max_tokens: 512})).json();
    if (!result.choices?.[0]?.message?.content || !(result.usage?.completion_tokens > 0)) throw new Error('The model completed without text. Try another model.');
    uiText($('connection-result'), `Connected. The shared model returned ${result.usage.completion_tokens} tokens. Now test the configuration in your chosen client.`);
  } catch (error) { uiText($('connection-result'), error.message); }
  finally { connectionTesting = false; buttonStates(); }
});
renderModelLegend();
// #51: focusing or clicking a card's icons spells out their meaning (not hover-only).
$('models').addEventListener('focusin', event => event.target.closest('.model-badges')?.classList.add('show-meaning'));
$('models').addEventListener('focusout', event => event.target.closest('.model-badges')?.classList.remove('show-meaning'));
if (!/^[a-f0-9]{64}$/.test(token)) message('Open geisten using the private link from the app or Pi launcher. The link contains your private local API key.');
else { loadTasks().catch(error => message(error.message)); poll(); timer = setInterval(poll, statusPollInterval); }

function showPage(id) {
  if (!['models-page', 'connect-page', 'settings-page', 'test-page'].includes(id)) return false;
  const quickTest = id === 'test-page';
  if (quickTest) id = 'models-page'; // Native quick-test shortcut focuses the shared pane.
  document.querySelectorAll('.page').forEach(page => { page.hidden = page.id !== id; });
  document.querySelectorAll('nav [data-page]').forEach(button => {
    if (button.dataset.page === id) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  $('chat-help').open = false; closeMeasurements();
  chatLayout();
  if (quickTest && !$('workspace').hidden) $('prompt').focus({preventScroll: true});
  else { const heading = $(id).querySelector('h1, h2'); heading.tabIndex = -1; heading.focus({preventScroll:true}); }
  window.scrollTo(0, 0);
  return true;
}
// Native menus route only to these fixed pages, without reloading the document.
window.geistNavigate = showPage;
// #125: ⌘1-3 (Ctrl on Linux) for the areas; arrow keys walk the model list.
document.addEventListener('keydown', event => {
  if (!(event.metaKey || event.ctrlKey) || event.altKey || event.shiftKey) return;
  const page = {'1': 'models-page', '2': 'connect-page', '3': 'settings-page'}[event.key];
  if (page) { event.preventDefault(); showPage(page); }
});
$('model-sidebar').addEventListener('keydown', event => {
  if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
  const picks = [...$('model-sidebar').querySelectorAll('.model-pick')].filter(b => !b.disabled && b.offsetParent);
  if (!picks.length) return;
  const at = picks.indexOf(document.activeElement);
  const next = picks[Math.max(0, Math.min(picks.length - 1, at < 0 ? 0 : at + (event.key === 'ArrowDown' ? 1 : -1)))];
  event.preventDefault(); next.focus();
});
$('toggle-models').addEventListener('click', () => {
  modelsFoldedByUser = !document.body.classList.contains('models-folded');
  document.body.classList.toggle('models-folded', modelsFoldedByUser);
  $('toggle-models').setAttribute('aria-expanded', String(!modelsFoldedByUser));
});
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
  document.querySelectorAll('.reply-copy').forEach(renderReplyCopy);
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
  if (!confirm(warning + t(`Remove ${modelLabel(model)} from this computer? You can download it again later.`))) return;
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
      // #125: focus returns to the list, so a folded list opens again.
      if (document.body.classList.contains('models-folded')) { modelsFoldedByUser = false; document.body.classList.remove('models-folded'); $('toggle-models').setAttribute('aria-expanded', 'true'); }
      if (pick && !pick.disabled) pick.focus({preventScroll:true});
    }
  }
}

// Native dialog supplies top-layer modality and an inert background.
let measurementReturn = null, measurementDraft = null;
function openMeasurements() {
  if ($('performance').open) return;
  if ($('activity-dialog').open) $('activity-dialog').close();
  measurementReturn = document.activeElement;
  measurementDraft = {value:$('prompt').value, selection:[$('prompt').selectionStart, $('prompt').selectionEnd, $('prompt').selectionDirection]};
  $('chat-help').open = false;
  $('performance').showModal();
  $('close-measurements').focus({preventScroll:true});
}
function closeMeasurements() {
  if ($('performance').open) $('performance').close();
}
$('open-measurements').addEventListener('click', openMeasurements);
$('close-measurements').addEventListener('click', closeMeasurements);
$('performance').addEventListener('close', () => {
  if (measurementDraft && $('prompt').value===measurementDraft.value) $('prompt').setSelectionRange(...measurementDraft.selection);
  if (measurementReturn?.isConnected && (document.activeElement===document.body || document.activeElement===measurementReturn || $('performance').contains(document.activeElement))) measurementReturn.focus({preventScroll:true});
  measurementReturn = null; measurementDraft = null;
});
document.addEventListener('keydown', event => {
  const modal = document.querySelector('dialog[open]');
  if (event.key === 'Tab' && modal) {
    const controls = [...modal.querySelectorAll('button,[tabindex="0"]')]
      .filter(node => !node.disabled && node.getClientRects().length && !node.closest('[hidden]'));
    const first = controls[0], last = controls.at(-1);
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
    return;
  }
  if (event.key !== 'Escape') return;
  if ($('activity-dialog').open) { event.preventDefault(); $('activity-dialog').close(); }
  else if ($('performance').open) { event.preventDefault(); closeMeasurements(); }
  else if ($('chat-help').open) {
    event.preventDefault(); $('chat-help').open = false;
    $('chat-help').querySelector('summary').focus({preventScroll:true});
  }
});
document.addEventListener('pointerdown', event => {
  if (!$('chat-help').contains(event.target)) $('chat-help').open = false;
});

// Native radio semantics preserve keyboard navigation and checked state.
function renderExecution() {
  const execution = state?.execution;
  const loading = executionLoading();
  const choice = pendingExecution || execution?.mode || 'auto';
  const active = !loading && state?.ready ? execution?.active : '';
  const target = choice === 'auto' ? execution?.recommended : choice;
  const disabled = !state?.ready || state.busy || state.loading || requesting || !!controller || connectionTesting;
  for (const input of document.querySelectorAll('[name="execution"]')) {
    input.checked = input.value === choice;
    input.disabled = disabled || (input.value === 'gpu' && !execution?.gpu_available);
    const label = input.closest('label'), mark = label.querySelector('.recommended-mark');
    const running = input.value === active;
    const pending = loading && input.value === target;
    label.classList.toggle('is-active', running);
    label.classList.toggle('is-loading', pending);
    if (mark) {
      mark.hidden = input.value !== execution?.recommended;
      mark.title = t('Default processor');
      mark.setAttribute('aria-label', t('Default processor'));
    }
    const backend = input.value === 'gpu' ? backendName(execution?.gpu_backend || (execution?.active === 'gpu' ? execution?.backend : '')) : '';
    const detail = label.querySelector('.processor-backend');
    if (detail) { detail.textContent = backend; detail.hidden = !backend; }
    const description = [input.value === 'auto' ? 'Auto' : input.value.toUpperCase(), backend,
      input.value !== 'auto' ? `${$(`history-${input.value}-rate`).textContent} · ${$(`summary-${input.value}-first`).getAttribute('aria-label')} · ${$(`summary-${input.value}-count`).textContent}` : '',
      running ? t('Active processor') : pending ? t('Switching processor…') : '',
      mark && !mark.hidden ? t('Recommended') : ''].filter(Boolean).join(' · ');
    input.setAttribute('aria-label', description);
    label.title = `${description}. ${t(input.value === 'auto' ? 'Uses the recommended processor. Changing execution reloads the model without downloading it again.' : execution?.reason || 'Load a model first.')}`;
  }
  $('execution-description').textContent = t(execution?.reason || 'Load a model first.');
  const status = loading ? t(pendingExecution !== null ? 'Switching processor…' : 'Loading model') : active ? `${active.toUpperCase()}${active === 'gpu' ? ` · ${backendName(execution.backend)}` : ''}` : t('No model loaded');
  if ($('execution-current').textContent !== status) $('execution-current').textContent = status;
  $('execution-current').title = t('Active processor');
  $('execution-notice').textContent = t(execution?.notice || '');
  $('execution-notice').hidden = !execution?.notice || !(state?.ready || state?.loading); /* #82 */
  const measured = execution?.performance;
  const slow = !!active && measured?.below_target === true && knownNumber(measured.rate) && measured.rate > 0 && knownNumber(measured.target_tps);
  const warning = $('execution-performance');
  warning.hidden = !slow;
  $('execution-performance-text').textContent = slow ? `${active.toUpperCase()} · ${rateText(measured.rate)} · ${t('Below target')}` : '';
  warning.title = slow ? `${t('Last completed reply')}. ${t('Interactive target')}: ${rateText(measured.target_tps)}. ${t('Different prompts are not a controlled benchmark.')}` : '';
}
for (const input of document.querySelectorAll('[name="execution"]')) input.addEventListener('change', async () => {
  if (!input.checked || requesting) return;
  requesting = true; pendingExecution = input.value; buttonStates(); visibleModels().forEach(modelCard);
  try { await api('/app/execution', {mode:pendingExecution}); message('', false); }
  catch (error) { message(error.message); }
  finally {
    while (polling) await new Promise(resolve => setTimeout(resolve, 40));
    await poll(); requesting = false; pendingExecution = null;
    if (state) render(state); else buttonStates();
  }
});
// #54: WebKit draws the native file control in the bundle's language, not the page's;
// a translated button opens it and the chosen name is shown beside it.
// #124: light, dark or like the system; a per-device convenience, so localStorage.
function applyAppearance(value) {
  if (value === 'light' || value === 'dark') document.documentElement.dataset.theme = value;
  else delete document.documentElement.dataset.theme;
}
{
  let saved = 'system';
  try { saved = localStorage.getItem('geisten-appearance') || 'system'; } catch {}
  $('ui-appearance').value = ['system', 'light', 'dark'].includes(saved) ? saved : 'system';
  applyAppearance($('ui-appearance').value);
}
$('ui-appearance').addEventListener('change', () => {
  applyAppearance($('ui-appearance').value);
  try { localStorage.setItem('geisten-appearance', $('ui-appearance').value); } catch {}
});
$('catalog-choose').addEventListener('click', () => $('catalog-file').click());
$('catalog-file').addEventListener('change', async () => {
  const file = $('catalog-file').files[0];
  if (file) { $('catalog-file-name').textContent = file.name; $('catalog-file-name').hidden = false; }
  if (!file || requesting) return;
  requesting = true; buttonStates(); $('catalog-file').disabled = $('catalog-choose').disabled = true;
  try {
    if (file.size > 24576) throw new Error('The catalog must be at most 24 KiB.');
    const catalog = await file.text();
    try { JSON.parse(catalog); }
    catch { throw new Error('Choose a valid JSON file.'); }
    await api('/app/catalog', catalog);
    uiText($('catalog-result'), 'Catalog updated.');
  } catch (error) { uiText($('catalog-result'), error.message); }
  finally {
    $('catalog-file').value = '';
    while (polling) await new Promise(resolve => setTimeout(resolve, 40));
    await poll(); requesting = false;
    if (state) render(state); else buttonStates();
  }
});


// Polling restores the current snapshot; it never starts or replays work.
const activityLabels={receipt:'Validating local artifact',hash:'Checking model',download:'Downloading model',stopping:'Stopping',starting:'Starting runtime',loading:'Loading model',backend:'Initializing processor',model:'Loading weights',metadata:'Reading model metadata',warmup:'Warming up runtime',ready:'Ready',connect:'Connecting',open:'Opening session',tokenize:'Reading input',prefill:'Processing input',generate:'Generating',preparing:'Preparing answer',answer:'Answering'};
function acceptActivity(snapshot) {
  if (!snapshot?.instance) { activitySnapshot=null; activitySequences.clear(); return; }
  if (activityInstance!==snapshot.instance) { activitySequences.clear(); activityInstance=snapshot.instance; requestAfter=0; }
  const accepted={...snapshot};
  for (const kind of ['load','request','download']) {
    const next=snapshot[kind], previous=activitySequences.get(kind);
    if (previous && (!next || next.id<previous.id || next.generation<previous.generation ||
        (next.id===previous.id && (next.sequence<previous.sequence ||
         (next.sequence===previous.sequence && next.event_age_ms<previous.event_age_ms))))) accepted[kind]=previous;
    else if (next) { accepted[kind]={...next,receivedAt:performance.now()}; activitySequences.set(kind,accepted[kind]); }
  }
  activitySnapshot=accepted;activityAt=performance.now();
}
function currentActivity() {
  const a=activitySnapshot;
  if (a?.request && !a.request.outcome && (!controller || a.request.id>requestAfter)) return a.request;
  if (a?.load && !a.load.outcome) return a.load;
  if (controller || runtimeRequest()) return null;
  return [a?.request,a?.load].filter(Boolean).sort((x,y)=>y.id-x.id)[0] || null;
}
function renderActivity() {
  const a=currentActivity(), pending=!a&&(!!controller||runtimeRequest());
  const running=!!a&&!a.outcome;
  const age=performance.now()-(a?.receivedAt??activityAt), stale=!!a&&(!state||age>6000);
  const elapsed=a ? (a.stage_elapsed_ms+(running&&!stale?age:0))/1000 : 0;
  const phase=pending?'Waiting for service':!a?'Ready':a.outcome==='failed'?'Failed':a.outcome==='cancelled'?'Stopped':a.outcome?'Ready':activityLabels[a.stage] || 'Working';
  const label=stale?'Status unavailable':phase;
  if ($('activity-label').dataset.uiText!==label) uiText($('activity-label'),label);
  $('activity-time').textContent=running&&!stale ? `${a.backend?.startsWith('cpu')?'CPU':backendName(a.backend)||t('Unknown')} · ${formatNumber(elapsed,0)} s` : '';
  $('activity-stop').hidden=!running&&!pending;
  $('activity-stop').disabled=cancellingActivity||(!a&&!controller)||a?.stage==='stopping'||stale;
  $('runtime-state').classList.toggle('working',running||pending);
  if (activeTurn && !activeTurn.answerSeen && running && a===activitySnapshot?.request) uiText(activeTurn.status,label);
  const longCPU=running&&!stale&&elapsed>=15&&a.backend?.startsWith('cpu');
  $('activity-cpu-hint').hidden=!longCPU;
  if (longCPU) { uiText($('activity-cpu-hint'),'Large models can take time on CPU.'); $('activity-cpu-hint').title=$('activity-cpu-hint').textContent; }
  if (!$('activity-dialog').open) return;
  // #53: nothing to report is one sentence, not a table of dashes; the footnote only
  // explains a live operation; "Ready" is never shown next to a stopped runtime.
  const text=stale?'Status is stale. Reconnect to see current activity.':a?.outcome==='failed'?'The operation failed. Retry the model or choose another processor.':running&&elapsed>=15&&a.backend?.startsWith('cpu')?'Large models can take time on CPU.':!a?'Nothing is running right now.':'No progress report available';
  $('activity-hint').textContent=t(text);
  $('activity-details').hidden=!a;
  $('activity-footnote').hidden=!running;
  const summary=[['Operation',a?.id ? `${activityInstance} / ${a.id}` : t('Not available')],['Stage',t(a&&!a.runtime_alive&&!running&&phase==='Ready'?'No model loaded':activityLabels[a?.stage]||phase)],['Phase elapsed',timeText(a?elapsed:null)],['Total elapsed',timeText(a?(a.elapsed_ms+(running&&!stale?age:0))/1000:null)],['Last progress report',a?timeText((a.event_age_ms+age)/1000):'—'],['Runtime',t(a?.runtime_alive?'Process alive':'Not running')],['Processor',a?.backend||'—'],['Error code',a?.error_code||'—']];
  $('activity-summary').replaceChildren(...summary.flatMap(([key,value])=>{const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=t(key);dd.textContent=value;return [dt,dd];}));
  const phases=a?.phases||[];
  $('activity-phases').replaceChildren(...phases.map(p=>{const row=document.createElement('li');row.textContent=`${t(activityLabels[p.stage]||'Working')} · ${timeText(p.duration_ms/1000)}`;return row;}));
  const engineLine=engineText(a?.engine?.geistlib?.version ? a.engine : state?.engine);
  $('activity-engine').textContent=engineLine; $('activity-engine').hidden=!engineLine;
}
$('open-activity').addEventListener('click',()=>{activityReturn=document.activeElement;closeMeasurements();$('activity-dialog').showModal();$('close-activity').focus({preventScroll:true});renderActivity();});
$('close-activity').addEventListener('click',()=>$('activity-dialog').close());
// #122: the recommendation's action starts or downloads it, the normal (verified) way.
for (const id of ['best-action', 'recommend-action']) $(id).addEventListener('click', () => { if ($(id).dataset.id) choose($(id).dataset.id); });
$('open-compare').addEventListener('click', () => { uiText($('verdict-result'), ''); $('compare-dialog').showModal(); $('close-compare').focus({preventScroll: true}); renderCompare(); });
$('close-compare').addEventListener('click', () => $('compare-dialog').close());
// A point leads to its row: the row has the values and what they rest on.
$('compare-chart').addEventListener('click', event => {
  const id = event.target.closest('.compare-point')?.dataset.id;
  const row = id && [...$('compare-table').tBodies[0].rows].find(r => r.dataset.id === id);
  if (!row) return;
  for (const r of $('compare-table').tBodies[0].rows) r.classList.toggle('highlight', r === row);
  row.scrollIntoView({block: 'nearest'}); row.focus({preventScroll: true});
});
for (const id of ['verdict-intent', 'verdict-fast', 'verdict-usable', 'verdict-reliable']) $(id).addEventListener('change', () => saveVerdictSettings({
  intent: $('verdict-intent').value, fast_s: Number($('verdict-fast').value), usable_s: Number($('verdict-usable').value), reliable: Number($('verdict-reliable').value) / 100}));
$('verdict-reset').addEventListener('click', () => saveVerdictSettings({reset: true}));
$('activity-dialog').addEventListener('close',()=>{if(activityReturn?.isConnected&&(document.activeElement===document.body||$('activity-dialog').contains(document.activeElement)||document.activeElement===activityReturn))activityReturn.focus({preventScroll:true});activityReturn=null;});
$('activity-stop').addEventListener('click',async()=>{
  const a=currentActivity();if(!a){controller?.abort();return;}if(a.outcome||cancellingActivity)return;
  cancellingActivity=true;renderActivity();
  try {await api('/app/activity/cancel',{id:a.id,generation:a.generation,instance:activityInstance});controller?.abort();await poll();}
  catch(error){message(error.message);}
  finally {cancellingActivity=false;renderActivity();}
});
setInterval(()=>{renderActivity();renderMemory();},1000);
