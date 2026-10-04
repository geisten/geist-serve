// Runs in the real native WebView, with deterministic transport faults only.
// The host also runs separate real-model, clipboard and native-dialog checks.
window.chatChecksDone = false;
window.chatChecksError = null;
window.chatChecksStage = 'locale';
async function checkDownloadRingMotion(assert) {
  const ring = document.createElement('span'); ring.className = 'model-ring';
  ring.style.cssText = 'position:fixed;top:0;right:0;pointer-events:none';
  document.body.append(ring);
  const model = {id:'ring-motion-fixture', name:'Progress fixture', bytes:10000, installed:false, partial:0};
  const transfer = received => renderRing(ring, model, {phase:'downloading', job_model:model.id, received});
  const pause = received => renderRing(ring, {...model, partial:received}, {phase:'', job_model:''});
  const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
  const fill = () => ring.querySelector('.ring-fill');
  const value = () => {
    const style = getComputedStyle(fill());
    // WebKit versions differ in whether CSS zoom scales computed dasharray and
    // dashoffset together. Calibrate the same property in the same SVG context.
    const probe = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    probe.style.strokeDashoffset = '100';
    ring.querySelector('svg').append(probe);
    // WebKit computes "90px", Chrome "calc(90px)": read the number either way.
    const length = text => parseFloat(String(text).replace(/^calc\(/, ''));
    const full = length(getComputedStyle(probe).strokeDashoffset);
    const offset = length(style.strokeDashoffset); probe.remove();
    return 100 * (1 - offset / full);
  };
  const near = (a, b) => Math.abs(a - b) < .02;
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const report = {reducedMotion:reduced, frames:[], playback:'wall-clock'};
  const advance = async milliseconds => {
    // A headless WebKit can report visible while its timeline is suspended. Seek
    // its actual CSS animations instead; this still checks rendered interpolation,
    // but is explicitly not evidence of wall-clock frame pacing on this host.
    report.playback = 'native-interpolation-with-controlled-time';
    for (const animation of fill().getAnimations()) {
      const time = Number(animation.currentTime || 0) + milliseconds;
      animation.pause();
      if (time >= animation.effect.getComputedTiming().endTime) animation.finish();
      else animation.currentTime = time;
    }
    await wait(0);
  };
  window.downloadRingMotionEvidence = report;
  try {
    transfer(1000);
    report.initial = {progress:value(), offset:getComputedStyle(fill()).strokeDashoffset, dash:getComputedStyle(fill()).strokeDasharray};
    assert(near(value(), 10), `initial transfer shows actual progress immediately: ${JSON.stringify(report.initial)}`);
    const stableFill = fill();
    transfer(5001);
    value(); // Commit the new target before timing native rendered frames.
    assert(ring.getAttribute('aria-valuenow') === '50', 'accessible percentage stays readable');
    await advance(200);
    const first = value(); report.frames.push(first);
    report.visibility = document.visibilityState;
    report.animations = fill().getAnimations().map(item => ({time:item.currentTime, start:item.startTime, state:item.playState, property:item.transitionProperty}));
    if (!reduced) {
      assert(first > 10 && first < 50.01, `ring has a real intermediate frame, not a step: ${first}`);
      const animation = fill().getAnimations().find(item => item.transitionProperty === 'stroke-dashoffset');
      assert(animation, 'native renderer interpolates the arc');
      const elapsed = animation.currentTime;
      transfer(5001); // A repeated status/focus render must not reset the transition.
      assert(fill() === stableFill && fill().getAnimations().includes(animation), 'repeated render retains SVG and running transition');
      await advance(200);
      assert(animation.currentTime >= elapsed, 'repeated status does not reset the animation clock');
      assert(value() > first && value() <= 50.01, 'animation advances only towards confirmed bytes');
    }
    // A transfer that stops reporting data must settle, never creep towards 100%.
    await advance(1900);
    report.frames.push(value());
    assert(near(value(), 50.01), 'fractional progress reaches the precise measured target');
    await advance(120);
    assert(near(value(), 50.01) && !fill().getAnimations().length, 'stalled transfer rests at measured bytes');
    transfer(7500); value(); await advance(90);
    pause(7500);
    assert(near(value(), 75) && !fill().getAnimations().length, 'pause immediately shows confirmed progress and stops motion');
    transfer(7500);
    assert(near(value(), 75), 'resume keeps the paused position');
    transfer(8000); value(); await advance(90);
    transfer(100);
    assert(near(value(), 1) && !fill().getAnimations().length, 'retry with fewer bytes resets immediately');
    transfer(10000); value();
    assert(getComputedStyle(ring.querySelector('.ring-check')).display === 'none', '100 percent received is not a verified green check');
    renderRing(ring, model, {phase:'verifying', job_model:model.id, received:10000}); value();
    assert(!ring.hasAttribute('aria-valuenow') && getComputedStyle(ring.querySelector('.ring-check')).display === 'none', 'verification is indeterminate, without a success claim');
    if (!reduced) {
      const spin = fill().getAnimations().find(item => item.animationName === 'ring-loading');
      assert(spin, 'verification visibly spins');
      const period = spin.effect.getTiming().duration;
      spin.currentTime = period - 1;
      const end = new DOMMatrixReadOnly(getComputedStyle(fill()).transform);
      spin.currentTime = period + 1;
      const start = new DOMMatrixReadOnly(getComputedStyle(fill()).transform);
      report.loopDelta = Math.hypot(start.a-end.a, start.b-end.b, start.c-end.c, start.d-end.d);
      assert(report.loopDelta < .03, 'spinner loop has no angle jump at its boundary');
      spin.currentTime = 0;
      await advance(100);
      const before = getComputedStyle(fill()).transform, elapsed = spin.currentTime;
      renderRing(ring, model, {phase:'verifying', job_model:model.id, received:10000}); value();
      await advance(100);
      assert(fill().getAnimations().includes(spin) && spin.currentTime > elapsed && getComputedStyle(fill()).transform !== before, 'verification spinner advances without restarting on status refresh');
    }
    renderRing(ring, {...model, installed:true}, {phase:'', job_model:''});
    assert(near(value(), 100) && !fill().getAnimations().length && getComputedStyle(ring.querySelector('.ring-check')).display !== 'none', 'verified completion closes immediately with a check');
    renderRing(ring, model, {phase:'', job_model:''});
    assert(getComputedStyle(ring.querySelector('.ring-download')).display !== 'none', 'deletion restores the download action');
    transfer(8000); value();
    renderRing(ring, {...model, id:'other-ring-fixture'}, {phase:'downloading', job_model:'other-ring-fixture', received:9000});
    assert(near(value(), 90) && !fill().getAnimations().length, 'different model never animates from previous artifact');
    // Activate the shipped reduced-motion rule in this test document only.
    // No OS accessibility preference is changed.
    const rule = [...document.styleSheets].flatMap(sheet => [...sheet.cssRules]).find(rule =>
      rule.media?.mediaText.includes('prefers-reduced-motion') &&
      [...rule.cssRules].some(child => child.selectorText === '.model-ring .ring-fill'));
    assert(rule, 'shipped CSS has a reduced-motion rule for every ring phase');
    const media = rule.media.mediaText;
    try {
      rule.media.mediaText = 'all';
      transfer(2000); value(); transfer(6000);
      assert(near(value(), 60) && !fill().getAnimations().length, 'reduced motion updates progress without interpolation');
      for (const phase of ['verifying', 'preparing']) {
        renderRing(ring, model, {phase, job_model:model.id}); value();
        assert(!fill().getAnimations().length && !ring.hasAttribute('aria-valuenow'), 'reduced motion leaves loading/verification static and indeterminate');
      }
    } finally { rule.media.mediaText = media; }
    window.downloadRingMotionEvidence = report;
  } finally { ring.remove(); }
}
async function checkActivityUX(assert, tick) {
  while(polling) await tick();
  const saved=state, savedAPI=api, savedDraft=$('prompt').value;
  let fixture=structuredClone(saved), cancels=[];
  const operation=(id,stage,extra={})=>({id,generation:8,sequence:2,stage,elapsed_ms:21000,stage_elapsed_ms:19000,event_age_ms:19000,runtime_alive:true,progress_events:0,outcome:'',backend:'cpu_neon',model:saved.active_id,phases:[{stage:'connect',offset_ms:0,duration_ms:2000},{stage,offset_ms:2000,duration_ms:19000}],...extra});
  const update=(request,load=null,download=null)=>{
    fixture={...saved,activity:{instance:'numeric-activity-fixture',request,load,download}};
    render(structuredClone(fixture));
  };
  try {
    api=async(path,body,signal)=>{
      if(path==='/app/status') return new Response(JSON.stringify(fixture));
      if(path==='/app/activity/cancel') {cancels.push(body.id);fixture.activity.load={...fixture.activity.load,outcome:'cancelled',sequence:5,stage:'stopping'};return new Response('{}',{status:202});}
      return savedAPI(path,body,signal);
    };
    $('prompt').value='Unsent multiline\ndraft'; $('prompt').setSelectionRange(2,9);resizeComposer();chatLayout();
    const rect=()=>[$('transcript'),$('task-form')].map(x=>{const r=x.getBoundingClientRect();return [r.top,r.height];});
    const before=rect();
    const started=performance.now();requesting=true;acceptActivity(null);renderActivity();
    assert($('activity-label').textContent===t('Waiting for service') && performance.now()-started<200,'pending acknowledges interaction without claiming admission');requesting=false;
    update(operation(100,'prefill'),null,operation(101,'download'));
    assert(currentActivity().id===100 && $('activity-label').textContent===t('Processing input'),'background download cannot replace request activity');
    assert(!$('activity-stop').hidden && !$('activity-stop').disabled && $('runtime-state').classList.contains('working'),'blocked phase has Stop and a non-ready activity indicator');
    assert(!$('activity-cpu-hint').hidden,'long CPU wait explained in the ordinary view');
    assert($('activity-label').getAttribute('aria-live')==='polite' && $('activity-time').getAttribute('aria-hidden')==='true','timer does not announce every second');
    assert(rect().flat().every((value,i)=>Math.abs(value-before.flat()[i])<=1),`activity keeps transcript and composer geometry: ${JSON.stringify({before,after:rect()})}`);
    // #49: the conversation gets the height. Activity shares the model-name row, each
    // processor choice is one line, and the chrome above the transcript stays within budget.
    assert($('activity-lane').closest('.chat-heading'),'activity lane shares the model-name row');
    {const heading=document.querySelector('.chat-heading'),h0=heading.getBoundingClientRect().height,name=$('runtime-name').textContent;
     $('runtime-name').textContent='A model with a very long name '.repeat(8);
     assert(Math.abs(heading.getBoundingClientRect().height-h0)<=0.5,'a long model name cannot grow the model-name row');
     $('runtime-name').textContent=name;}
    assert([...document.querySelectorAll('.execution-choice label')].every(l=>l.getBoundingClientRect().height<=34),'each processor choice is a single line');
    if (innerHeight>=600) assert($('transcript').getBoundingClientRect().top-document.querySelector('.runtime-panel').getBoundingClientRect().top<=124,`chat chrome within budget: ${$('transcript').getBoundingClientRect().top-document.querySelector('.runtime-panel').getBoundingClientRect().top}px`);
    const labelNode=$('activity-label').firstChild;renderActivity();assert($('activity-label').firstChild===labelNode,'unchanged stage does not repeat the live announcement');
    const previous=activitySnapshot.request;
    acceptActivity({...fixture.activity,request:operation(99,'answer',{generation:7})});renderActivity();
    assert(currentActivity()===previous,'older generation and operation rejected');
    acceptActivity({...fixture.activity,request:operation(100,'answer',{sequence:1})});renderActivity();
    assert(currentActivity()===previous,'out-of-order phase rejected');
    update(operation(100,'prefill',{event_age_ms:20000}));
    assert(currentActivity().progress_events===0 && currentActivity().event_age_ms===20000,'heartbeat does not erase no-progress age');
    activitySnapshot.request.receivedAt=performance.now()-7001;renderActivity();
    assert($('activity-label').textContent===t('Status unavailable') && $('activity-stop').disabled,'stale telemetry differs from live blocked operation');
    update(operation(100,'preparing',{sequence:3,progress_events:5}));
    assert($('activity-label').textContent===t('Preparing answer'),'preparation requires recognized data');
    $('open-activity').focus();$('open-activity').click();await tick();
    assert($('activity-dialog').open && document.activeElement===$('close-activity'),'inspector uses modal focus');
    assert($('activity-summary').textContent.includes(t('Last progress report')) && !$('activity-summary').textContent.includes('Unsent'),'inspector is numeric and private');
    assert(!$('activity-details').hidden && !$('activity-footnote').hidden,'#53: a live operation shows its table and the progress footnote');
    document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));await tick();
    assert(!$('activity-dialog').open && document.activeElement===$('open-activity'),'Escape returns focus');
    update(operation(100,'prefill',{sequence:4,outcome:'failed',error_code:504}));
    $('open-activity').click();renderActivity();
    assert($('activity-summary').textContent.includes(t('Processing input')) && $('activity-summary').textContent.includes('504'),'failure retains actual failed phase');
    assert($('activity-footnote').hidden,'#53: no progress footnote once an operation has ended');
    update(operation(200,'generating',{sequence:1,outcome:'completed',runtime_alive:false}));renderActivity();
    assert(!$('activity-summary').textContent.includes(t('Ready')) && $('activity-summary').textContent.includes(t('No model loaded')),'#53: a stopped runtime is never "Ready"');
    // A missing operation keeps the last known one (by design); true idle is a fresh service instance.
    fixture={...saved,activity:{instance:'idle-activity-fixture',request:null,load:null,download:null}};render(structuredClone(fixture));renderActivity();
    assert($('activity-details').hidden && $('activity-footnote').hidden && $('activity-hint').textContent===t('Nothing is running right now.'),'#53: nothing to report is one sentence, not a table of dashes');
    $('close-activity').click();await tick();
    update(operation(100,'prefill',{sequence:5,outcome:'cancelled'}),operation(102,'loading',{generation:9}));
    $('activity-stop').click();for(let i=0;i<30&&cancellingActivity;i++)await tick();
    assert(cancels.length===1&&cancels[0]===102&&currentActivity().outcome==='cancelled','Stop addresses current owned load exactly once');
    assert($('prompt').value==='Unsent multiline\ndraft' && $('prompt').selectionStart===2,'activity and cancellation preserve draft selection');
    // These are presentation fixtures, not CUDA/Vulkan inference certification.
    let nextOperation=103;
    for (const [backend,label] of [['metal','Metal'],['cuda','CUDA'],['vulkan','Vulkan'],['',t('Unknown')]]) {
      update(operation(nextOperation++,'prefill',{generation:10,backend}));
      assert($('activity-time').textContent.startsWith(label+' · '), `${backend || 'unreported'} activity renders without losing service state`);
      assert($('activity-cpu-hint').hidden && !$('activity-stop').disabled, 'GPU or unreported backend keeps activity cancellable without a CPU hint');
    }
    const animation=getComputedStyle($('runtime-state')).animationName;
    if(matchMedia('(prefers-reduced-motion:reduce)').matches)assert(animation==='none','reduced motion has static status');
  } finally {
    if($('activity-dialog').open)$('activity-dialog').close();
    requesting=false;api=savedAPI;activityInstance='';acceptActivity(null);render(saved);$('prompt').value=savedDraft;
  }
}
(async () => {
  const originalAPI = api, originalConfirm = window.confirm;
  const assert = (ok, detail) => { if (!ok) throw new Error(detail); };
  // Native acceptance can supply its 100 ms host poll as the fixture scheduler.
  // Background WebViews coalesce JS timers; transport/geometry assertions do not
  // measure wall-clock latency. Real inference below uses the actual service.
  const tick = () => new Promise(resolve => window.geistTestClock
    ? (window.geistTestTicks ||= []).push(resolve) : setTimeout(resolve, 90));
  const idle = async () => { for (let i=0; i<200 && controller; i++) await tick(); assert(!controller, 'request completed'); };
  const key = options => { const event = new KeyboardEvent('keydown', {key:'Enter', bubbles:true, cancelable:true, ...options}); $('prompt').dispatchEvent(event); return event.defaultPrevented; };
  const input = text => { $('prompt').value = text; $('prompt').dispatchEvent(new Event('input')); };
  let stream, calls = [], fail = false;
  const emit = item => stream.enqueue(new TextEncoder().encode(JSON.stringify(item) + '\n'));
  const finish = limited => { emit({done:true, limited, eval_count:32, eval_duration:1e9}); stream.close(); };
  try {
    // Locale resolution does not depend on the developer or CI machine's language.
    for (const locale of ['de', 'de-DE', 'de_AT.UTF-8', 'DE-ch', 'de@euro']) assert(resolveLanguage('system', locale) === 'de', `German system locale: ${locale}`);
    for (const locale of ['en-US', 'fr-FR', 'debug', '', undefined]) assert(resolveLanguage('system', locale) === 'en', `English fallback: ${locale}`);
    assert(resolveLanguage('en', 'de-DE') === 'en' && resolveLanguage('de', 'en-US') === 'de', 'manual preference overrides OS');
    assert(resolveLanguage('invalid', 'de-DE') === 'de', 'invalid preference returns to system');
    const international = new Set(['geisten', 'geist', 'English', 'Deutsch', 'Home Assistant', 'Terminal', 'VS Code · Continue', '—', '— tokens/s', '— RAM']);
    const sources = [...staticTexts.map(([, text]) => text.trim()), ...staticAttributes.map(([, , text]) => text)];
    const missing = [...new Set(sources.filter(text => !international.has(text) && !Object.hasOwn(german, text)))];
    assert(!missing.length, `Missing German interface translations: ${missing.join(' | ')}`);
    assert($('new-chat').querySelector('svg') && !$('new-chat').textContent.trim(), 'clear test uses a labelled trash icon');
    assert($('new-chat').getAttribute('aria-label') === t('Clear chat'), 'trash accessible label follows locale');
    assert(!$('new-chat').hidden, 'clear chat remains discoverable even when disabled');
    assert($('summary-cpu-first').closest('.execution-choice') && $('test-size').closest('.model-metrics'), 'measurements stay under their processor/model');
    assert($('performance').tagName==='DIALOG' && !$('performance').closest('#workspace'), 'measurements use a separate modal, not a chat disclosure');
    assert(!$('chat-speed') && !$('chat-memory') && !$('performance-rss') && !$('performance-speed'), 'no duplicate performance summaries or live counters');
    assert($('open-measurements').getAttribute('aria-haspopup')==='dialog', 'measurement inspection is an explicit accessible action');
    assert(new Set([...document.querySelectorAll('[id]')].map(el => el.id)).size === document.querySelectorAll('[id]').length, 'unique IDs preserve control bindings');
    api = async (path, body, signal) => {
      if (path !== '/app/generate') return originalAPI(path, body, signal);
      calls.push(body);
      if (fail) throw new Error("The prompt does not fit this model's context. Try a shorter text.");
      return new Response(new ReadableStream({start(c) { stream=c; signal.addEventListener('abort', () => c.error(new DOMException('Stopped','AbortError')), {once:true}); }}));
    };
    const ring = document.createElement('span'); ring.className='model-ring';
    const downloadModel = {...state.models[0], installed:false, partial:0};
    renderRing(ring, downloadModel, {phase:'', job_model:''});
    assert(ring.dataset.stage === 'missing' && ring.getAttribute('role') === 'img', 'missing download has a labelled download icon');
    renderRing(ring, downloadModel, {phase:'downloading', job_model:downloadModel.id, received:downloadModel.bytes / 2});
    assert(ring.dataset.stage === 'downloading' && ring.getAttribute('aria-valuenow') === '50' && ring.style.getPropertyValue('--ring-progress') === '50', 'download ring exposes real 50 percent progress');
    renderRing(ring, {...downloadModel, partial:downloadModel.bytes / 4}, {phase:'',job_model:''});
    assert(ring.dataset.stage === 'paused' && ring.getAttribute('aria-valuenow') === '25', 'paused download retains progress');
    renderRing(ring, downloadModel, {phase:'verifying', job_model:downloadModel.id, received:downloadModel.bytes});
    assert(ring.dataset.stage === 'verifying' && !ring.hasAttribute('aria-valuenow'), '100 percent transfer does not imply a verified completed download');
    assert(!ring.getAttribute('aria-label').includes('Download'), 'existing-file verification does not imply a new download');
    renderRing(ring, downloadModel, {phase:'preparing', job_model:downloadModel.id});
    assert(ring.dataset.stage === 'loading' && !ring.hasAttribute('aria-valuenow'), 'cached model preparation is loading, not download progress');
    renderRing(ring, {...downloadModel, installed:true}, {phase:'',job_model:''});
    assert(ring.dataset.stage === 'downloaded' && ring.style.getPropertyValue('--ring-progress') === '100' && !ring.hasAttribute('aria-valuenow'), 'complete download closes the ring independent of active model');
    renderRing(ring, downloadModel, {phase:'',job_model:''});
    assert(ring.dataset.stage === 'missing' && ring.style.getPropertyValue('--ring-progress') === '0', 'removed or failed download clears completed state');
    window.chatChecksStage = 'download-ring-motion';
    await checkDownloadRingMotion(assert);
    showPage('models-page');
    assert(!$('models-page').hidden && !$('test-page').hidden && !document.querySelector('nav [data-page="test-page"]'), 'models and quick test share one view');
    assert([...document.querySelectorAll('.model .model-ring')].length === state.models.length, 'each model exposes download state before its name');
    assert(document.documentElement.scrollWidth <= innerWidth, 'model picker fits a small or zoomed window');
    assert($('model-chooser').tagName === 'SECTION', 'model list is visible without a disclosure');
    assert(!$('recommendation-reason') && !$('preferences'), 'no recommendation explanation or nested settings section');
    assert(!$('new-chat').closest('.chat-heading') && $('runtime-state').parentElement === $('runtime-model') && !$('runtime-state').textContent, 'status dot precedes model name without redundant text or header trash');
    assert(state.models.every(model => [...$('models').querySelectorAll('.model')].some(card => card.dataset.id === model.id)), 'main list includes every catalog model, including missing downloads');
    assert(!$('add-model') && !$('model-catalog'), 'downloads require no separate catalog dialog');
    input('Keep this draft through settings');
    $('open-preferences').click();
    assert(!$('settings-page').hidden && $('models-page').hidden && $('open-preferences').getAttribute('aria-current') === 'page', 'settings is a separate navigation destination');
    assert($('ui-language').closest('#settings-page') && $('language-choice').closest('#settings-page') && !$('unload') && !$('quit'), 'settings holds language preferences without unload or service-stop actions');
    window.geistNavigate('models-page');
    assert($('prompt').value === 'Keep this draft through settings', 'settings navigation keeps the draft'); input('');
    const panes = [$('model-sidebar').getBoundingClientRect(), $('test-page').getBoundingClientRect()];
    assert(innerWidth < 700 ? document.body.classList.contains('models-folded') || panes[0].bottom <= panes[1].top : panes[0].right <= panes[1].left, '#125: the list sits in the sidebar on wide windows and above the test (or folded) on small ones');
    { // #125: the sidebar stays with every area; ⌘1-3 switch areas; arrow keys walk the list; the list button folds it.
      const key = (k, extra = {}) => document.dispatchEvent(new KeyboardEvent('keydown', {key: k, metaKey: true, bubbles: true, cancelable: true, ...extra}));
      key('3'); assert(!$('settings-page').hidden && $('model-sidebar').closest('.app-shell') && !$('model-sidebar').closest('.page'), '#125: ⌘3 opens Settings; the list is not part of a page');
      if (innerWidth >= 700) assert($('model-sidebar').getBoundingClientRect().width > 0, '#125: the sidebar stays visible next to Settings');
      key('2'); assert(!$('connect-page').hidden, '#125: ⌘2 opens Connect');
      key('1'); assert(!$('models-page').hidden, '#125: ⌘1 returns to the models');
      const picks = [...$('model-sidebar').querySelectorAll('.model-pick')].filter(b => !b.disabled && b.offsetParent);
      if (picks.length > 1) {
        picks[0].focus(); $('model-sidebar').dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowDown', bubbles: true, cancelable: true}));
        assert(document.activeElement === picks[1], '#125: ArrowDown moves to the next model');
      }
      const before = document.body.classList.contains('models-folded'), saved = modelsFoldedByUser;
      $('toggle-models').click();
      assert(document.body.classList.contains('models-folded') === !before && $('toggle-models').getAttribute('aria-expanded') === String(before), '#125: the list button folds and unfolds the list');
      $('toggle-models').click(); modelsFoldedByUser = saved;
    }
    // Model-row interactions use deterministic transport only; no test fetches a
    // catalog-sized model. The host separately loads a real installed GGUF.
    window.chatChecksStage = 'runtime-activity';
    await checkActivityUX(assert,tick);
    window.chatChecksStage = 'model-actions';
    const chatAPI = api;
    while (polling) await tick();
    const realState = state;
    let fixture = JSON.parse(JSON.stringify(realState)), modelCalls = [], fault = '', release;
    const alternative = fixture.models.find(m => m.id !== fixture.active_id && m.resource_fit !== 2);
    assert(alternative, 'catalog offers an alternative for direct-action tests');
    const modelID = alternative.id;
    const fixtureModel = () => fixture.models.find(m => m.id === modelID);
    const pick = () => document.querySelector(`[data-id="${modelID}"] .model-pick`);
    const settle = async () => {
      for (let i=0; i<200 && (requesting || polling); i++) await tick();
      assert(!requesting && !polling, 'model action settles');
    };
    const resetFixture = () => {
      fixture = JSON.parse(JSON.stringify(realState));
      // This is a controlled suitability fixture, independent of runner RAM.
      Object.assign(fixtureModel(), {installed:false, partial:0, preview_accepted:false, resource_fit:0});
      modelCalls = []; render(JSON.parse(JSON.stringify(fixture)));
    };
    try {
      api = async (path, body, signal) => {
        if (path === '/app/status') return new Response(JSON.stringify(fixture));
        if (!['/app/preview','/app/download','/app/select','/app/cancel','/app/remove'].includes(path)) return chatAPI(path, body, signal);
        modelCalls.push([path, body]);
        if (fault === path) throw new Error('Controlled model action failure');
        if (path === '/app/preview') fixtureModel().preview_accepted = true;
        if (path === '/app/download') {
          if (fault === 'hold') await new Promise(resolve => { release=resolve; });
          Object.assign(fixture, {busy:true, phase:'downloading', job_model:modelID, received:fixtureModel().bytes / 2});
        }
        if (path === '/app/cancel') {
          fixtureModel().partial = fixture.received;
          Object.assign(fixture, {busy:false, phase:'', job_model:''});
        }
        if (path === '/app/remove') { fixtureModel().installed=false; fixtureModel().partial=0; if (fixture.active_id === modelID) Object.assign(fixture, {active_id:'', active:'', ready:false}); }
        if (path === '/app/select') Object.assign(fixture, {active_id:modelID, active:fixtureModel().name, ready:true, busy:false, phase:'', job_model:''});
        return new Response('{}');
      };
      resetFixture(); input('Draft survives model changes');
      const originalRow = pick().closest('.model');
      assert(pick().closest('#models') && !pick().disabled, 'undownloaded model is directly available in the main list');
      assert(!$('setup-start') && !$('setup') && !$('go-setup'), 'no second setup or start control');
      assert(pick().querySelector('.download-state').hidden && pick().getAttribute('aria-label').includes(t('Not downloaded')), 'missing state uses symbols while retaining an accessible explanation');
      // #51: a multi-GB download says its size first; cancelling starts nothing.
      {let asked=''; window.confirm=text=>{asked=text;return false;};
       const size=fixtureModel().bytes; fixtureModel().bytes=3e9; render(JSON.parse(JSON.stringify(fixture)));
       pick().querySelector('.model-name').click(); await settle();
       assert(!modelCalls.length && asked.includes(bytes(3e9)),'#51: a large download asks with its size, and cancel starts nothing');
       fixtureModel().bytes=size; render(JSON.parse(JSON.stringify(fixture)));}
      window.confirm = () => true;
      pick().querySelector('.model-name').click(); await settle();
      assert(JSON.stringify(modelCalls.map(([path]) => path)) === JSON.stringify(['/app/preview','/app/download']), 'one name click grants preview and starts exactly one download');
      assert(modelCalls.every(([, body]) => body.id === modelID), 'action keeps the explicitly requested model');
      assert(pick().closest('#models') && pick().closest('.model') === originalRow, 'download keeps progress in the same visible model row');
      assert(getComputedStyle(pick().querySelector('.ring-download')).display === 'none' && getComputedStyle(pick().querySelector('.ring-track')).display !== 'none' && getComputedStyle(pick().querySelector('.ring-pause')).display !== 'none', 'progress ring replaces the arrow and contains its pause control');
      assert(pick().getAttribute('aria-label').includes(t('Pause download')) && pick().querySelector('.model-ring').getAttribute('aria-valuenow') === '50', 'same row exposes real progress and pause');
      const count = modelCalls.length;
      pick().dispatchEvent(new MouseEvent('click', {bubbles:true, detail:2})); await tick();
      assert(modelCalls.length === count, 'second click of a double click cannot pause a new download');
      const repeated = new KeyboardEvent('keydown', {key:'Enter', repeat:true, cancelable:true}); pick().dispatchEvent(repeated);
      assert(repeated.defaultPrevented, 'held Enter cannot repeatedly toggle the model');
      pick().querySelector('.model-ring').click(); await settle();
      assert(modelCalls.at(-1)[0] === '/app/cancel' && pick().querySelector('.model-ring').dataset.stage === 'paused', 'pause icon retains the partial download');
      pick().querySelector('.model-ring').click(); await settle();
      assert(modelCalls.at(-1)[0] === '/app/download' && modelCalls.filter(([path]) => path === '/app/preview').length === 1, 'ring resumes directly without another preview step');
      fixture.phase='verifying'; render(JSON.parse(JSON.stringify(fixture)));
      assert(pick().disabled && pick().querySelector('.model-ring').dataset.stage === 'verifying', 'verification is not presented as completed or pausable');
      Object.assign(fixture, {busy:false, phase:'', job_model:''}); fixtureModel().installed=true;
      render(JSON.parse(JSON.stringify(fixture))); modelCalls=[];
      pick().querySelector('.model-name').click(); await settle();
      assert(modelCalls.length === 1 && modelCalls[0][0] === '/app/select', 'installed model starts with one click and no download');
      assert(getComputedStyle(pick().querySelector('.ring-active')).display !== 'none' && getComputedStyle(pick().querySelector('.ring-check')).display === 'none' && getComputedStyle(pick().querySelector('.ring-download')).display === 'none', '#121: the active model shows one dot, not a check next to its verdict');
      assert(pick().disabled && !$('workspace').hidden && $('prompt').value === 'Draft survives model changes', 'active model has no redundant action and retains the draft');
      // #125: in a narrow window the list folds once a model is ready; a user opens it to delete.
      if (document.body.classList.contains('models-folded')) $('toggle-models').click();
      const remove = pick().closest('.model').querySelector('.remove');
      assert(!remove.hidden && !remove.disabled && !remove.closest('details'), 'each local model has a direct delete action, including the active model');
      window.confirm = () => false; remove.click(); await settle();
      assert(fixtureModel().installed, 'cancelled deletion keeps the file');
      window.confirm = () => true; remove.focus(); remove.click(); await settle();
      assert(modelCalls.at(-1)[0] === '/app/remove' && pick().closest('#models') && pick().closest('.model') === originalRow && !fixtureModel().installed, 'deletion keeps the same model row in the main list');
      assert(document.activeElement === pick() && !pick().disabled && pick().getAttribute('aria-label').includes(t('Download and start')) && remove.hidden, 'deletion restores keyboard focus to the available download action');
      modelCalls=[]; pick().click(); await settle();
      assert(modelCalls.at(-1)[0] === '/app/download', 'a deleted model can be downloaded again directly');
      window.confirm = () => true;
      resetFixture();
      const badges = pick().closest('.model').querySelector('.model-badges');
      assert(!pick().closest('.model').querySelector('details') && !$('catalog-preview') && !document.querySelector('.model-action'), 'no preview banner, detail blocks or second action icon');
      assert(getComputedStyle(pick().querySelector('.ring-download')).display !== 'none' && getComputedStyle(pick().querySelector('.ring-track')).display === 'none', 'missing model shows only the leading download arrow');
      badges.focus(); badges.click(); await tick();
      assert(document.activeElement === badges && !modelCalls.length && badges.getAttribute('aria-label').includes(t('Fits this computer')), 'suitability information is keyboard-accessible without starting the model');
      assert(badges.querySelector('[data-capability="chat"]') && !badges.querySelector('[data-capability="vision"]') && !badges.querySelector('[data-capability="speech_recognition"]'), 'current service advertises text only');
      Object.assign(fixtureModel(), {resource_fit:1, capabilities:{chat:true, vision:true, speech_recognition:true, unsupported:true}}); render(JSON.parse(JSON.stringify(fixture)));
      assert(badges.querySelector('.model-fit').dataset.fit === '1' && badges.querySelectorAll('[data-capability]').length === 3, 'fixture: conditional fit and allowlisted future capabilities have separate symbols');
      fixtureModel().capabilities = {chat:true, vision:'true', speech_recognition:false}; render(JSON.parse(JSON.stringify(fixture)));
      assert(badges.querySelectorAll('[data-capability]').length === 1, 'unverified or nonboolean capability metadata cannot advertise support');
      fixtureModel().resource_fit=2; render(JSON.parse(JSON.stringify(fixture)));
      pick().click(); await choose(modelID);
      assert(pick().disabled && !modelCalls.length && badges.querySelector('.model-fit').dataset.fit === '2' && badges.getAttribute('aria-label').includes(t('Unavailable on this computer')), 'unsuitable models retain warning and cannot start');
      resetFixture(); fixture.busy=true; fixtureModel().installed=true; render(JSON.parse(JSON.stringify(fixture)));
      pick().click(); await choose(modelID);
      assert(!modelCalls.length, 'another busy operation prevents switching');
      resetFixture(); fault='/app/preview'; pick().click(); await settle();
      assert(modelCalls.length === 1 && modelCalls[0][0] === '/app/preview' && $('notice').textContent.includes('Controlled model action failure'), 'failed preview prevents download and explains failure');
      resetFixture(); fault='/app/download'; pick().click(); await settle();
      assert(!pick().disabled && $('notice').textContent.includes('Controlled model action failure') && $('prompt').value === 'Draft survives model changes', 'download failure leaves a retryable row and keeps the draft');
      resetFixture(); fault='hold'; pick().click();
      for (let i=0; i<50 && !release; i++) await tick();
      assert(release && pick().disabled, 'pending request locks its row');
      await choose(modelID); pick().click();
      assert(modelCalls.filter(([path]) => path === '/app/download').length === 1, 'pending request cannot create duplicate downloads');
      release(); release=null; await settle(); fault='';
    } finally {
      if (release) { release(); await settle(); }
      api=chatAPI; window.confirm=originalConfirm; render(realState); message('', false); input('');
    }
    window.chatChecksStage = 'model-variants';
    window.confirm=()=>true; // #51 size prompt for multi-GB variants; restored in this section's finally
    while (polling) await tick();
    const variantRealState = state, variantAPI = api;
    const vf = JSON.parse(JSON.stringify(state));
    const q4 = vf.models.find(m => m.id === 'qwen38-27b-q4'), q8 = vf.models.find(m => m.id === 'qwen38-27b-q8');
    const row = m => cards.get(m.id), action = m => row(m).querySelector('.model-pick');
    let variantCalls = [], variantFault = false;
    const variantIdle = async () => { for (let i=0;i<200 && (requesting||polling);i++) await tick(); assert(!requesting&&!polling, 'variant action settles'); };
    const paint = () => render(JSON.parse(JSON.stringify(vf)));
    try {
      assert(q4 && q8 && q4.group_id === q8.group_id, 'Qwen quantizations have explicit matching group IDs');
      Object.assign(q4, {installed:true, partial:0, preview_accepted:true, resource_fit:0});
      Object.assign(q8, {installed:false, partial:0, preview_accepted:true, resource_fit:1, reason:'Available RAM is tight now. Close other apps before loading this model.'});
      Object.assign(vf, {active_id:q4.id, active:q4.name, ready:true, loading:false, busy:false, phase:'', job_model:''});
      api = async (path, body, signal) => {
        if (path === '/app/status') return new Response(JSON.stringify(vf));
        if (!['/app/download','/app/select','/app/remove','/app/cancel'].includes(path)) return variantAPI(path,body,signal);
        variantCalls.push([path,body]);
        if (variantFault) throw new Error('Controlled variant failure');
        const m = vf.models.find(m => m.id === body.id);
        if (path === '/app/download') Object.assign(vf, {busy:true, phase:'downloading', job_model:m.id, received:m.bytes/2});
        if (path === '/app/cancel') { q8.partial=vf.received; Object.assign(vf,{busy:false,phase:'',job_model:''}); }
        if (path === '/app/select') Object.assign(vf, {active_id:m.id,active:m.name,ready:false,loading:true});
        if (path === '/app/remove') Object.assign(m,{installed:false,partial:0});
        return new Response('{}');
      };
      paint(); input('Draft across quantizations');
      const group = row(q4).closest('.model-group'), sameRow = row(q8);
      assert(group === row(q8).closest('.model-group') && group.querySelectorAll('.model').length === 2, 'one heading owns both visible Qwen variants');
      assert(group.querySelector('h3').textContent === 'Qwen3.8 27B' && !group.querySelector('select,details,[role=listbox]'), 'variant comparison has no dropdown, disclosure or interactive listbox options');
      assert(row(vf.models.find(m=>m.id==='bonsai2-27b-pq2')).closest('.model-group') !== group, 'Bonsai remains a separate derivative');
      assert(!row(q4).querySelector('.variant-active').hidden && row(q8).querySelector('.variant-active').hidden, 'active is separate from suitability and download state');
      assert(action(q4).getAttribute('aria-label').includes('Q4_0') && action(q8).getAttribute('aria-label').includes('Q8_0'), 'action labels identify the exact variant');
      assert($('runtime-name').textContent === `Qwen3.8 27B · ${t('4-bit')}` && $('runtime-name').title === 'Qwen3.8 27B · Q4_0', 'loaded header names the variant plainly; the format is its tooltip');
      assert(!row(q8).querySelector('.variant-warning').hidden && row(q8).querySelector('.variant-warning').textContent.startsWith(t('Available RAM is tight')) && row(q8).querySelector('.variant-warning').textContent.includes(t(`${vf.models.find(m=>m.id==='qwen38-27b-q8').ram_gib} GiB RAM recommended`)), 'warning reason and RAM need are visible without hover');
      assert(row(q8).querySelector('.variant-size').textContent.endsWith(bytes(q8.bytes).replace(' ','\u00a0')), 'each variant exposes its own download size');
      for (const lang of ['de','en']) {
        $('ui-language').value=lang; $('ui-language').dispatchEvent(new Event('change')); await tick();
        assert(row(q4).querySelector('.variant-active').textContent === t('Active') && action(q8).getAttribute('aria-label').includes(t('Download model')), 'variant states translate without changing identity');
      }
      action(q8).focus(); paint();
      assert(document.activeElement === action(q8) && row(q8) === sameRow, 'status polling keeps focused variant and DOM identity');
      action(q8).click(); await variantIdle();
      assert(variantCalls.at(-1)[1].id === q8.id && action(q8).querySelector('.model-ring').getAttribute('aria-valuenow') === '50', 'only requested variant downloads with percentage');
      assert(row(q4).classList.contains('active') && !row(q8).classList.contains('active'), 'download does not change the running variant');
      action(q8).click(); await variantIdle();
      assert(action(q8).querySelector('.model-ring').dataset.stage === 'paused', 'variant download pauses in place');
      action(q8).click(); await variantIdle();
      vf.phase='verifying'; paint();
      assert(action(q8).disabled && !row(q8).classList.contains('active'), 'verification is not activation');
      Object.assign(vf, {phase:'',job_model:'',busy:false}); q8.installed=true; paint();
      assert(vf.active_id===q4.id && !action(q8).disabled, 'completed background variant waits for explicit activation');
      action(q8).click(); await variantIdle();
      assert(!row(q8).classList.contains('active') && row(q8).querySelector('.variant-active').hidden && action(q8).querySelector('.model-ring').dataset.stage==='loading', 'loading variant stays pending even after active_id changes');
      assert(!$('workspace').hidden && $('prompt').value==='Draft across quantizations', 'variant loading retains the usable layout and draft');
      Object.assign(vf, {ready:true,loading:false}); paint();
      assert(row(q8).classList.contains('active') && !row(q4).classList.contains('active') && $('runtime-name').title.endsWith('Q8_0'), 'only successful load activates new variant');
      window.confirm=() => true;
      const deleteQ4=row(q4).querySelector('.remove');deleteQ4.focus();deleteQ4.click();await variantIdle();
      assert(variantCalls.at(-1)[0]==='/app/remove' && variantCalls.at(-1)[1].id===q4.id && q8.installed && vf.active_id===q8.id, 'remove targets only one variant without stopping its sibling');
      assert(group.querySelectorAll('.model').length===2 && row(q8)===sameRow && !action(q4).disabled && document.activeElement===action(q4), 'deleted variant remains downloadable and receives focus');
      q4.installed=true; paint();variantFault=true;action(q4).click();await variantIdle();
      assert(vf.active_id===q8.id && !row(q4).classList.contains('active') && $('notice').textContent.includes('Controlled variant failure'), 'failed switch cannot falsely activate the requested variant');
      assert($('prompt').value==='Draft across quantizations', 'failed variant switch retains draft');
      // Imported display text is never markup. Layout checks use long names too.
      q4.group_name=q8.group_name='<img src=x onerror=alert(1)> Model with a very long name';paint();
      assert(group.querySelector('h3').textContent===q4.group_name && !group.querySelector('img'), 'group names render as inert text');
      for (const m of [q4,q8]) {
        const pick=action(m), rect=pick.getBoundingClientRect(), label=pick.querySelector('.model-name').getBoundingClientRect();
        assert(rect.height>=44 && label.left>=rect.left && label.right<=rect.right+1, 'variant labels wrap inside an adequate touch target');
      }
    } finally {
      api=variantAPI; window.confirm=originalConfirm; render(variantRealState); message('',false);input('');
    }
    window.chatChecksStage = 'background-download';
    while (polling) await tick();
    const backgroundAPI=api, backgroundRealState=state;
    const bf=JSON.parse(JSON.stringify(state));
    const bm=bf.models.find(m=>m.id!==bf.active_id && m.resource_fit!==2);
    Object.assign(bm,{installed:false,partial:0,preview_accepted:false,resource_fit:0});
    const backgroundCalls=[]; let backgroundStream, backgroundRelease;
    const backgroundPick=()=>cards.get(bm.id).querySelector('.model-pick');
    window.confirm=()=>true; // #51 size prompt; restored in this section's finally
    const backgroundPaint=()=>{
      bf.inference_busy=!!controller; bf.busy=!!controller||!!bf.phase;
      bf.background_download=!!bf.phase;
      render(JSON.parse(JSON.stringify(bf)));
    };
    const backgroundIdle=async()=>{for(let i=0;i<200&&(requesting||polling);i++)await tick();assert(!requesting&&!polling,'background row request settles');};
    const backgroundEmit=item=>backgroundStream.enqueue(new TextEncoder().encode(JSON.stringify(item)+'\n'));
    const backgroundFinish=()=>{backgroundEmit({done:true,eval_count:16,eval_duration:1e9});backgroundStream.close();};
    try {
      api=async(path,body,signal)=>{
        if(path==='/app/status') {bf.inference_busy=!!controller;bf.busy=!!controller||!!bf.phase;bf.background_download=!!bf.phase;return new Response(JSON.stringify(bf));}
        if(path==='/app/generate') {
          backgroundCalls.push([path,body]);
          return new Response(new ReadableStream({start(c){backgroundStream=c;signal.addEventListener('abort',()=>c.error(new DOMException('Stopped','AbortError')),{once:true});}}));
        }
        if(path==='/app/preview') {bm.preview_accepted=true;return new Response('{}');}
        if(path==='/app/download') {
          backgroundCalls.push([path,body]);
          if(!bm.partial) await new Promise(resolve=>{backgroundRelease=resolve;});
          Object.assign(bf,{phase:'downloading',job_model:bm.id,received:bm.bytes/2}); return new Response('{}');
        }
        if(path==='/app/cancel') {backgroundCalls.push([path,body]); bm.partial=bf.received;Object.assign(bf,{phase:'',job_model:''});return new Response('{}');}
        return backgroundAPI(path,body,signal);
      };
      backgroundPaint();input('Send while the download request is pending');$('prompt').focus();
      const activeID=bf.active_id, activeLabel=$('runtime-name').textContent;
      backgroundPick().click();for(let i=0;i<100&&!backgroundRelease;i++)await tick();
      assert(backgroundRelease && ! $('run').disabled && requesting && downloadRequest, 'pending download HTTP request leaves Send usable');
      key();await tick();backgroundEmit({response:'The current model keeps answering while another model downloads.'});await tick();
      input('My next draft');$('prompt').focus();backgroundRelease();await backgroundIdle();
      const oldOutput=$('output'), oldRow=cards.get(bm.id);
      assert(controller && !$('stop').hidden && !backgroundPick().disabled && $('runtime-name').textContent===activeLabel,'streaming, Stop and pause work together without changing the loaded header');
      assert($('prompt').value==='My next draft' && document.activeElement===$('prompt') && !$('workspace').hidden,'download progress preserves draft, focus and the visible chat');
      assert($('stop').getBoundingClientRect().bottom<=innerHeight && document.documentElement.scrollWidth<=innerWidth,`parallel activity keeps Stop in the viewport without horizontal clipping: ${JSON.stringify({stop:$('stop').getBoundingClientRect().toJSON(),height:innerHeight,workspace:$('workspace').getBoundingClientRect().toJSON(),panel:document.querySelector('.runtime-panel').getBoundingClientRect().toJSON()})}`);
      // Optional host checkpoint for an actual WKWebView screenshot, fixture-labelled.
      window.backgroundUXReady=true;
      for(let i=0;window.captureBackgroundUX&&i<200;i++)await tick();
      assert(!window.captureBackgroundUX,'native background UX screenshot checkpoint released');
      backgroundPick().click();await backgroundIdle();
      assert(controller && backgroundPick().querySelector('.model-ring').dataset.stage==='paused','pausing a transfer leaves the response streaming');
      backgroundPick().click();await backgroundIdle();
      assert(controller && bf.phase==='downloading' && backgroundCalls.filter(([path])=>path==='/app/download').length===2,'resume while generating issues exactly one new transfer');
      backgroundFinish();await idle();
      assert(! $('run').disabled && ! $('test-connection').disabled && $('prompt').value==='My next draft','finished response allows the next prompt and editor test during transfer');
      // #58/#59: Connect explains a missing model and every disabled cause; #60: the Mac hint is Mac-only.
      assert($('connection-disabled').hidden && !$('connection-model').classList.contains('is-empty'),'ready service shows the model and no disabled reason');
      {const saved=state;
       state={...saved,ready:false,active_id:''};buttonStates();
       assert(!$('connection-disabled').hidden && $('connection-disabled-text').dataset.uiText==='Load a model first to copy or test the connection.' && !$('connection-choose').hidden && $('connection-model').classList.contains('is-empty') && getComputedStyle($('connection-model')).fontFamily!==getComputedStyle($('connection-endpoint')).fontFamily,'no model: hint in body font, reason and a way to Models');
       assert($('copy-connection').getAttribute('aria-describedby')==='connection-disabled' && $('test-connection').getAttribute('aria-describedby')==='connection-disabled','disabled reason is announced with the buttons');
       state=null;buttonStates();
       assert($('connection-disabled-text').dataset.uiText==='Service unavailable. Reopen geisten to reconnect.' && $('connection-choose').hidden,'service down has its own reason');
       state=saved;buttonStates();}
      if (window.geistDesktop==='mac') assert(!/Ubuntu/.test(connectionHelp.terminal) && /geist-cli/.test(connectionHelp.terminal),'Mac terminal hint names only the Mac command line tool');
      key();await tick();backgroundEmit({response:'A second answer during checksum verification.'});await tick();
      bf.phase='verifying';backgroundPaint();
      assert(controller && backgroundPick().disabled && backgroundPick().querySelector('.model-ring').dataset.stage==='verifying' && !$('workspace').hidden,'verification locks only the transfer row');
      backgroundFinish();await idle();input('A third message during verification');
      assert(!$('run').disabled,'checksum verification does not disable new inference');key();await tick();
      backgroundEmit({response:'Completion must not interrupt this answer.'});await tick();
      Object.assign(bf,{phase:'',job_model:'',message:'Download complete.'});bm.installed=true;bm.partial=0;backgroundPaint();
      assert(controller && bf.active_id===activeID && $('runtime-name').textContent===activeLabel && cards.get(bm.id)===oldRow && backgroundPick().disabled,'download completion preserves runtime and disables unsafe activation during an answer');
      assert($('result').contains(oldOutput) && conversation.length===4,'prior transcript and session context survive the completed download');
      backgroundFinish();await idle();
      assert(!backgroundPick().disabled && backgroundPick().getAttribute('aria-label').includes(t('Start model')),'completed download offers explicit activation once inference finishes');
      // Opposite ordering: start a new transfer while an answer is already streaming.
      bm.installed=false;bm.partial=1;bf.message='';backgroundPaint();
      input('Already streaming before download');key();await tick();backgroundEmit({response:'Still answering.'});await tick();
      assert(!backgroundPick().disabled,'a missing model is downloadable during an existing response');
      backgroundPick().click();await backgroundIdle();
      assert(controller && bf.phase==='downloading','download can begin after streaming has started');
      $('stop').click();await idle();
      assert(bf.phase==='downloading' && backgroundCalls.filter(([path])=>path==='/app/cancel').length===1,'Stop chat does not pause or cancel its independent download');
      // A transfer failure remains visible after streaming; retry leaves the draft intact.
      input('Draft after transfer failure');Object.assign(bf,{phase:'',job_model:'',message:'Download paused: controlled disconnect.'});backgroundPaint();
      assert(!$('run').disabled && !backgroundPick().disabled && $('notice').textContent.includes('controlled disconnect') && $('prompt').value==='Draft after transfer failure','failed transfer is retryable and leaves chat usable');
      // An old service without the separate status fields retains its safe boundary.
      const legacy={...bf,busy:true};delete legacy.inference_busy;delete legacy.background_download;render(legacy);
      assert($('run').disabled && backgroundPick().disabled,'legacy global busy remains conservative');
    } finally {
      if(controller){controller.abort();await idle();}
      api=backgroundAPI;render(backgroundRealState);window.confirm=()=>true;$('new-chat').click();window.confirm=originalConfirm;message('',false);input('');
    }
    window.chatChecksStage = 'execution-catalog';
    while (polling) await tick();
    const executionRealState = state;
    let executionFixture = JSON.parse(JSON.stringify(state)), executionCalls = [], executionFault = false;
    const beforeExecutionAPI = api;
    const executionIdle = async () => { for (let i=0;i<100 && (requesting || polling);i++) await tick(); assert(!requesting,'execution action settles'); };
    try {
      executionFixture.execution = {mode:'auto',active:'cpu',backend:'cpu_neon',gpu_backend:'metal',recommended:'gpu',gpu_available:true,basis:'hardware',notice:'',reason:'GPU is the default for models of 1 GiB and larger; not a measured comparison.'};
      api = async (path, body, signal) => {
        if (path === '/app/status') return new Response(JSON.stringify(executionFixture));
        if (path === '/app/execution') {
          executionCalls.push(body);
          if (executionFault) throw new Error('Controlled processor failure');
          executionFixture.execution.mode=body.mode;
          executionFixture.ready=false;executionFixture.loading=true;
          executionFixture.execution.active='';executionFixture.execution.backend='';
          return new Response('{}');
        }
        if (path === '/app/catalog') {
          const imported=JSON.parse(body);
          if (imported.schema!==2) throw new Error('Invalid model catalog. Check schema, entries and unique IDs/files.');
          executionFixture.models=imported.models;
          executionFixture.catalog_revision=imported.revision;
          return new Response('{}');
        }
        return beforeExecutionAPI(path,body,signal);
      };
      executionFixture.performance_history=[{processor:'cpu',rate:32,first:.5,total:2,tokens:64,rss_bytes:2**30,recorded_at:1700000000},{processor:'gpu',rate:64,first:.2,total:1,tokens:64,rss_bytes:2**31,recorded_at:1700000100}];
      executionFixture.performance_profile={...executionFixture.performance_profile, artifact:executionFixture.models.find(m=>m.id===executionFixture.active_id)?.sha256,group:{input:0,output:1,cached:false,cold:false,contention:false,controlled:false},cpu:{...executionFixture.performance_history[0],first_answer:.7,first_answer_count:4,count:5,q25:30,q75:34},gpu:{...executionFixture.performance_history[1],first_answer:.3,first_answer_count:7,count:7,q25:60,q75:68},recent:[]};
      executionFixture.execution.performance={target_tps:8,below_target:true,rate:3};
      const fixtureModel=executionFixture.models.find(m=>m.id===executionFixture.active_id);
      if (fixtureModel) fixtureModel.resource_fit=0;
      render(executionFixture); input('Keep draft across processors');
      assert(!$('execution-performance').hidden && $('execution-performance').textContent.includes('CPU') && $('execution-performance').textContent.includes(rateText(3)), 'slow CPU warning belongs beside the loaded model');
      assert($('execution-performance').closest('#workspace') && !$('models').contains($('execution-performance')), 'execution warning never becomes a catalog warning');
      const warningBounds=$('execution-performance').getBoundingClientRect();
      assert(warningBounds.left>=0 && warningBounds.right<=innerWidth && warningBounds.bottom<=innerHeight && $('transcript').clientHeight>0, 'warning fits the window without removing the conversation viewport');
      const warningLanguage=interfaceLanguage;interfaceLanguage='de';renderExecution();
      assert($('execution-performance').textContent.includes('Unter Richtwert'), 'execution warning is localized');
      interfaceLanguage=warningLanguage;renderExecution();
      if (fixtureModel) assert(cards.get(fixtureModel.id).querySelector('.model-fit').dataset.fit==='0', 'adequate GPU keeps the whole-device badge suitable despite slow CPU');
      const retainedTranscript=$('result'), retainedContent=$('result').innerHTML;
      openMeasurements();await tick();
      const cpu = document.querySelector('[name="execution"][value="cpu"]'), gpu=document.querySelector('[name="execution"][value="gpu"]');
      assert(document.querySelector('[name="execution"]:checked').value==='auto' && !gpu.closest('label').querySelector('.recommended-mark').hidden, 'recommendation and selected mode remain distinct');
      assert(gpu.closest('label').querySelector('.recommended-mark').getAttribute('aria-label')===t('Default processor') && document.querySelector('.recommended-legend'),'#52: the star has a name and a visible legend');
      assert(cpu.closest('label').classList.contains('is-active') && !gpu.closest('label').classList.contains('is-active'), 'Auto exposes the actual processor inside its option');
      assert(cpu.getAttribute('aria-label').includes(t('Active processor')) && gpu.closest('label').querySelector('.processor-backend').textContent==='Metal', 'active processor is accessible and GPU backend is an option sublabel');
      assert($('execution-current').classList.contains('sr-only'), 'no duplicate visible processor label outside the choices');
      assert($('execution-choice').closest('#workspace') && !$('settings-page').contains($('execution-choice')), 'processor choice lives with active model only');
      gpu.click(); await executionIdle();
      assert(executionCalls.length===1 && executionCalls[0].mode==='gpu' && gpu.checked, 'GPU click targets real execution endpoint');
      assert(gpu.closest('label').classList.contains('is-loading') && !document.querySelector('.execution-choice .is-active'), 'pending choice spins without claiming an active backend');
      assert($('execution-performance').hidden, 'loading never repeats the previous processor warning');
      assert(!$('workspace').hidden && $('test-unavailable').hidden && $('runtime-state').classList.contains('loading'), 'asynchronous reload keeps the workspace and shows a status ring');
      assert(getComputedStyle($('runtime-state')).animationName === (matchMedia('(prefers-reduced-motion:reduce)').matches ? 'none' : 'processor-loading'), 'loading animation respects reduced motion');
      assert($('result')===retainedTranscript && $('result').innerHTML===retainedContent && $('performance').open, 'reload preserves transcript nodes and expanded metrics');
      assert($('history-cpu-rate').textContent===rateText(32) && $('history-gpu-rate').textContent===rateText(64), 'CPU/GPU measurements remain visible together during reload');
      assert($('test-memory').textContent==='—', 'reload clears live RSS while preserving historical samples');
      assert($('run').disabled && cpu.disabled && gpu.disabled && !$('prompt').disabled, 'reload locks send and switching but permits drafting');
      input('Edited during reload');
      executionFixture.ready=true;executionFixture.loading=false;executionFixture.execution.active='gpu';executionFixture.execution.backend='metal';
      executionFixture.execution.performance={target_tps:8,below_target:false,rate:64};render(executionFixture);
      assert($('execution-performance').hidden, 'adequate GPU execution has no slow warning');
      executionFixture.execution.performance={target_tps:8,below_target:false,rate:null};render(executionFixture);
      assert($('execution-performance').hidden, 'unmeasured processor is not reported slow');
      assert(!$('runtime-state').classList.contains('loading') && $('execution-current').textContent==='GPU · Metal', 'ready status removes spinner and reports actual backend');
      assert(gpu.closest('label').classList.contains('is-active') && !gpu.closest('label').classList.contains('is-loading'), 'completed GPU option gets the active check');
      for (const [backend, display] of [['vulkan','Vulkan'],['cuda','CUDA'],['metal','Metal']]) {
        executionFixture.execution.backend=backend;executionFixture.execution.gpu_backend=backend;render(executionFixture);
        assert(gpu.closest('label').querySelector('.processor-backend').textContent===display, 'backend metadata appears within GPU choice');
      }
      assert($('prompt').value==='Edited during reload', 'draft changes during reload survive');
      input('Keep draft across processors');
      assert($('prompt').value==='Keep draft across processors', 'backend switch preserves draft');
      executionFault=true;cpu.click();await executionIdle();
      assert(gpu.checked && $('notice').textContent.includes('Controlled processor failure'), 'failed change restores reported selection');
      executionFault=false;executionFixture.busy=true;render(executionFixture);
      assert(cpu.disabled && gpu.disabled, 'busy model disables backend changes');
      executionFixture.busy=false;executionFixture.execution.gpu_available=false;render(executionFixture);
      assert(gpu.disabled && !cpu.disabled && $('execution-choice').getAttribute('aria-describedby'), 'unavailable GPU is disabled and described');
      showPage('settings-page');
      const upload = async object => {
        const transfer = new DataTransfer();transfer.items.add(new File([JSON.stringify(object)],'models.json',{type:'application/json'}));
        $('catalog-file').files=transfer.files;$('catalog-file').dispatchEvent(new Event('change'));await executionIdle();
      };
      const catalogFixture={schema:2,revision:42,models:[...executionFixture.models,{...executionFixture.models[0],id:'imported-model',name:'Imported model',group_id:'imported-model',group_name:'Imported model',installed:false}]};
      await upload(catalogFixture);
      assert(cards.has('imported-model') && $('catalog-revision').textContent===`${t('Version')} 42` && $('catalog-result').textContent===t('Catalog updated.'), 'JSON file import updates visible list and revision');
      {const input=$('catalog-file'),box=input.getBoundingClientRect(),button=$('catalog-choose');
       assert(box.width<=1 && getComputedStyle(input).clipPath.includes('inset(50%)') && input.tabIndex===-1 && !button.hidden && button.getBoundingClientRect().width>1,'#54: the native file control is hidden; a page button opens it');
       assert(/\.json$/.test($('catalog-file-name').textContent) && button.getAttribute('aria-describedby')==='catalog-file-name','#54: the chosen file name is shown and described');
       assert(button.textContent===t('Import catalog…'),'#54: the picker button follows the interface language');}
      await upload({...catalogFixture,schema:0});
      assert(cards.has('imported-model') && $('catalog-result').textContent===t('Invalid model catalog. Check schema, entries and unique IDs/files.'), 'invalid import keeps current catalog and explains error');
    } finally {
      api=beforeExecutionAPI;render(executionRealState);message('',false);input('');showPage('models-page');
    }
    {// #57: one order for one catalog, whatever is installed, active or recommended by selection.
     const base=structuredClone(state), groupsOf=()=>[...$('models').children].map(g=>g.dataset.group);
     const groupIds=[];for(const m of base.models){const g=m.group_id||m.id;if(!groupIds.includes(g))groupIds.push(g);}
     const preferredModel=base.models.find(m=>m.id===base.recommendation?.preferred_id), preferredGroup=preferredModel&&(preferredModel.group_id||preferredModel.id);
     const expected=preferredGroup?[preferredGroup,...groupIds.filter(g=>g!==preferredGroup)]:groupIds;
     const last=base.models.at(-1).id;
     const fresh={...base,ready:false,active_id:'',recommendation:{...base.recommendation,id:''},models:base.models.map(m=>({...m,installed:false}))};
     const installed={...fresh,models:fresh.models.map(m=>m.id===last?{...m,installed:true}:m)};
     const active={...installed,ready:true,active_id:last,recommendation:{...base.recommendation,id:last,source:'saved'}};
     const orders=[fresh,installed,active].map(s=>{render(structuredClone(s));return groupsOf();});
     render(base);
     assert(orders.every(o=>JSON.stringify(o)===JSON.stringify(expected)),`#57: platform default first, then catalog order, in every state: ${JSON.stringify({expected,orders})}`);}

    {// #51: first launch names one recommendation, explains the icons and speaks plainly.
     const base=structuredClone(state), pickModel=base.models.find(m=>m.resource_fit!==2);
     const fresh={...base,ready:false,active_id:'',recommendation:{...base.recommendation,id:pickModel.id,source:'default',eligible:true},best_choice:{id:pickModel.id,verdict:'unknown',action:'download',processor:null,basis:null},
       models:base.models.map(m=>({...m,installed:false}))};
     fresh.models.find(m=>m.id!==pickModel.id).reason='RAM is smaller than the model file, before context and OS memory.';
     const tight=fresh.models.find(m=>m.id!==pickModel.id); tight.resource_fit=1; tight.ram_gib=16;
     render(structuredClone(fresh));
     const tags=[...document.querySelectorAll('.variant-recommended')].filter(e=>!e.hidden);
     assert(tags.length===1 && tags[0].closest('.model').dataset.id===pickModel.id,'#51: exactly one visible recommendation');
     assert(!$('model-prompt-hint').hidden && $('model-prompt-hint').textContent.includes(pickModel.group_name||pickModel.name),'#51: the empty state names the recommended model');
     assert(!$('model-legend').hidden && $('model-legend').textContent.includes(t('Fits this computer')),'#51: the icon legend is visible on first launch');
     assert(cards.get(pickModel.id).textContent.includes(t(plainVariant(pickModel))) && cards.get(pickModel.id).querySelector('.variant-size').textContent.includes(variantLabel(pickModel)),'#51: plain label first, technical label second');
     assert(cards.get(tight.id).querySelector('.variant-warning').textContent.includes(t('16 GiB RAM recommended')),'#51/#80: RAM guidance as a recommendation, in GiB');
     // #80: no unverified quality claim anywhere in the model list; the weight format instead, and one honest note.
     const listText=$('model-chooser').textContent;
     assert(!/high quality|hohe qualität|balanced|ausgewogen|very compact|sehr kompakt/i.test(listText),'#80: no quality ranking derived from the quantization');
     assert(['8-bit','4-bit','Ternary (native)'].map(x=>t(x)).some(x=>cards.get(pickModel.id).textContent.includes(x)),'#80: the row names the weight format');
     assert(!$('quality-note').hidden && $('quality-note').textContent===t('Answer quality not tested yet. Check answers.'),'#80: the list says that answer quality is not tested');
     assert(tags[0].textContent===t('Suggested start') && $('model-prompt-hint').textContent===recommendation(fresh).text,'#80/#122: one recommendation, the same sentence in the empty pane');
     const badges=cards.get(pickModel.id).querySelector('.model-badges'); badges.focus();
     assert(badges.classList.contains('show-meaning') && getComputedStyle(badges,'::after').content.includes(t('Fits this computer')),'#51: focusing the icons shows their meaning');badges.blur();
     render(base);
     assert($('model-legend').hidden===!!base.active_id,'#51: the legend steps back once a model is active');}
    assert(window.geistNavigate('invalid') === false && !$('models-page').hidden, 'native routing is allowlisted');
    showPage('test-page');
    window.chatChecksStage = 'markdown';
    const sample = document.createElement('div'); sample.className = 'message-text markdown';
    $('result').hidden = false; $('result').append(sample);
    const markdown = '# Clear heading\n\nA **strong** and *gentle* paragraph &amp; text.\n\n## Details\n\n- One\n  - Nested\n- Two\n\n3. Third\n4. Fourth\n\n- [x] Done\n- [ ] Pending\n\n> A quote\n\n---\n\nInline `x < 2` and ~~old~~.\n\n```js\nconst text = "<script>unsafe</script>";\n' + 'x'.repeat(1600) + '\n```\n\n| Name | Value |\n| :--- | ---: |\n| Alpha | **42** |\n\n[Example](https://example.com/a?q=1&b=2)\n\n![remote](https://example.com/image.png)\n\n<script>window.markdownXSS = true</script>\n<img src=x onerror="window.markdownXSS=true">\n\n[bad](javascript:alert(1)) [file](file:///tmp/test) [data](data:text/html,evil) [credentials](https://u:p@example.com)';
    chatMarkdown.render(sample, markdown);
    assert(sample.querySelector('h3')?.textContent === 'Clear heading' && sample.querySelector('h4')?.textContent === 'Details', 'semantic Markdown headings');
    assert(sample.querySelectorAll('strong').length === 2 && sample.querySelector('em') && sample.querySelector('del'), 'inline formatting');
    assert(sample.querySelector('ul ul') && sample.querySelector('ol').start === 3 && sample.querySelectorAll('.task-check').length === 2, 'nested, numbered and task lists');
    assert(sample.querySelector('blockquote') && sample.querySelector('hr') && sample.textContent.includes('paragraph & text'), 'quotes, separator and entities');
    assert(sample.querySelector('pre code').textContent.includes('<script>unsafe</script>') && sample.querySelector('table th').scope === 'col', 'code and accessible table');
    assert(!sample.querySelector('script,img,iframe,object,a,style,input') && !window.markdownXSS, 'model HTML and URLs cannot execute, navigate or fetch');
    assert(sample.querySelectorAll('.reference-address').length === 2, 'only safe HTTP(S) addresses are offered for copying');
    const pre = sample.querySelector('pre'), codeCopy = sample.querySelector('.code-toolbar button');
    assert(pre.scrollWidth > pre.clientWidth && document.documentElement.scrollWidth <= innerWidth, 'long code scrolls inside its block');
    assert(codeCopy._markdownCopyText === pre.textContent, 'code copy retains literal contents');
    const literalCode = pre.textContent;
    $('ui-language').value='de'; $('ui-language').dispatchEvent(new Event('change')); await tick();
    assert(codeCopy.title === 'Code kopieren' && codeCopy.ariaLabel === 'Code kopieren' && sample.querySelector('.table-scroll').ariaLabel === 'Tabelle', 'existing Markdown icon labels switch language');
    assert(sample.querySelector('.task-check').ariaLabel === 'Abgehakt' && [...sample.querySelectorAll('.markdown-reference')].some(node => node.textContent.includes('Bild: remote')), 'Markdown wrapper labels switch without translating generated content');
    assert(pre.textContent === literalCode && sample.querySelector('pre') === pre, 'locale changes preserve literal code and DOM nodes');
    $('ui-language').value='en'; $('ui-language').dispatchEvent(new Event('change')); await tick();
    pre.scrollLeft = 45; codeCopy.focus();
    const heading = sample.firstChild;
    chatMarkdown.render(sample, markdown + '\n\nA final paragraph.');
    assert(sample.firstChild === heading && sample.querySelector('pre') === pre && pre.scrollLeft === 45 && document.activeElement === codeCopy, 'unchanged blocks preserve nodes, scroll and focus');
    chatMarkdown.render(sample, '```txt\nfirst');
    chatMarkdown.render(sample, '```txt\nfirst second\n```');
    assert(sample.querySelector('pre code').textContent === 'first second' && sample.querySelector('button')._markdownCopyText === 'first second', 'unfinished fence and streamed copy are updated');
    const malformed = '> '.repeat(80) + 'deep but preserved';
    chatMarkdown.render(sample, malformed);
    assert(sample.textContent.includes('deep but preserved'), 'deep input remains readable');
    window.chatChecksStage = 'math';
    const workflow = String.raw`**$\rightarrow$ Anforderungen $\rightarrow$ Design/Konzept $\rightarrow$ Implementierung.**`;
    chatMarkdown.render(sample, workflow);
    assert(sample.querySelector('strong') && sample.querySelectorAll('math').length === 3 && [...sample.querySelectorAll('mo')].every(n => n.textContent === '→'), 'reported bold workflow renders real arrows inside Markdown');
    const formulas = String.raw`Inline $E=mc^2$, \(\frac{a_1}{\sqrt{b}}\) and $\alpha \leq \beta$.

$$
\sum_{i=1}^{n} i = \frac{n(n+1)}{2}
$$

\[
\begin{pmatrix}1 & 2 \\ 3 & 4\end{pmatrix}
\]

- In a list: $x_i$.

| Quantity | Formula |
| --- | --- |
| Ratio | $\frac{1}{2}$ |`;
    chatMarkdown.render(sample, formulas);
    assert(sample.querySelectorAll('math').length === 7 && sample.querySelector('mfrac') && sample.querySelector('msqrt') && sample.querySelector('msup') && sample.querySelector('msub') && sample.querySelector('mtable'), 'inline/display delimiters, fractions, roots, powers, matrices, lists and table cells');
    assert(sample.querySelectorAll('.math-display').length === 2 && sample.querySelector('.math-display').tabIndex === 0 && sample.querySelector('math').namespaceURI === 'http://www.w3.org/1998/Math/MathML', 'display formulas are keyboard reachable and use native accessible MathML');
    const plain = String.raw`Prices: $5 and $10. Escaped: \$20. Code: `;
    chatMarkdown.render(sample, plain + '`$x$ \\(y\\)`\n\n```latex\n$$\\frac{1}{2}$$\n```');
    assert(!sample.querySelector('math') && sample.textContent.includes('$5 and $10') && sample.textContent.includes('$20') && sample.querySelector('pre code').textContent === '$$\\frac{1}{2}$$', 'currency, escaped dollars and literal code are never converted to math');
    chatMarkdown.render(sample, String.raw`Before \[\frac{1}`);
    assert(!sample.querySelector('math') && sample.textContent.includes(String.raw`\[\frac{1}`), 'unfinished streamed formula stays literal');
    chatMarkdown.render(sample, String.raw`Before \[\frac{1}{2}\] after.`);
    assert(sample.querySelector('mfrac') && sample.textContent.includes('after.'), 'stream completion turns the same expression into a formula');
    const broken = String.raw`$\unknowncommand{<img src=x onerror=alert(1)>}$`;
    chatMarkdown.render(sample, broken);
    assert(sample.textContent === broken && sample.querySelector('.math-source') && !sample.querySelector('img'), 'invalid expressions preserve literal source without inserting parser error HTML');
    const attacks = [String.raw`\href{javascript:alert(1)}{click}`, String.raw`\url{https://example.invalid}`, String.raw`\includegraphics{https://example.invalid/image.png}`, String.raw`\htmlStyle{position:fixed}{x}`, String.raw`\htmlId{prompt}{x}`, String.raw`\htmlData{evil=yes}{x}`, String.raw`\def\a{\a}\a`, String.raw`\gdef\shared{leak}`];
    for (const expression of attacks) {
      chatMarkdown.render(sample, '$'+expression+'$');
      assert(!sample.querySelector('a,img,script,iframe,style,[href],[src],[id],[data-evil]'), 'untrusted math cannot inject DOM, navigate, fetch or grow recursive macros');
    }
    chatMarkdown.render(sample, String.raw`$\shared$`);
    assert(sample.querySelector('.math-source'), 'macro definitions cannot leak between formulas');
    chatMarkdown.render(sample, '$' + '{'.repeat(65) + 'x' + '}'.repeat(65) + '$');
    assert(sample.querySelector('.math-source'), 'excessive nesting remains literal');
    chatMarkdown.render(sample, '$' + 'x'.repeat(8200) + '$');
    assert(sample.textContent.length === 8202 && !sample.querySelector('math'), 'oversized math is preserved without parsing');
    chatMarkdown.render(sample, '$x$ '.repeat(70));
    assert(sample.querySelectorAll('math').length === 64 && sample.querySelectorAll('.math-source').length === 6, 'per-message formula budget preserves excess source');
    let parses = 0; const realMathRender = katex.render;
    try {
      katex.render = (...args) => { parses++; return realMathRender(...args); };
      const stable = String.raw`$$\int_0^1 x^2\,dx$$`;
      chatMarkdown.render(sample, stable);
      const preserved = sample.querySelector('math'), region = sample.firstChild;
      region.focus();
      chatMarkdown.render(sample, stable+'\n\nMore streaming text.');
      assert(parses === 1 && sample.querySelector('math') === preserved && document.activeElement === region, 'completed math is cached and its DOM/focus survives streaming');
      $('ui-language').value='de'; $('ui-language').dispatchEvent(new Event('change')); await tick();
      assert(region.ariaLabel === 'Formel', 'formula region accessible label switches to German');
      $('ui-language').value='en'; $('ui-language').dispatchEvent(new Event('change')); await tick();
    } finally { katex.render = realMathRender; }
    chatMarkdown.render(sample, '$$'+'x + '.repeat(180)+'x$$');
    const wideMath = sample.querySelector('.math-display');
    assert(wideMath.scrollWidth > wideMath.clientWidth && document.documentElement.scrollWidth <= innerWidth, 'wide equations scroll inside the answer without widening the window');
    wideMath.scrollLeft=60; wideMath.focus();
    chatMarkdown.render(sample, '$$'+'x + '.repeat(180)+'x$$\n\nMore text.');
    assert(wideMath.scrollLeft === 60 && document.activeElement === wideMath, 'streaming preserves a wide formula scroll position and keyboard focus');
    chatMarkdown.render(sample, '$'+'x + '.repeat(180)+'x$');
    const wideInline=sample.querySelector('.math-inline');
    assert(wideInline.scrollWidth > wideInline.clientWidth && wideInline.tabIndex === 0 && document.documentElement.scrollWidth <= innerWidth, 'wide inline math also scrolls and is keyboard reachable');
    chatMarkdown.render(sample, String.raw`$\rightarrow$`);
    assert(sample.querySelector('.math-inline').tabIndex === -1, 'short inline symbols add no unnecessary tab stop');
    sample.remove(); $('result').hidden = true;
    showPage('test-page'); assert(document.activeElement === $('prompt'), 'entering chat focuses composer');
    $('chat-help').querySelector('summary').focus(); render(state);
    assert(document.activeElement === $('chat-help').querySelector('summary'), 'status poll never steals focus');
    assert(!document.querySelector('[data-task]'), 'task presets removed');
    assert($('run').getAttribute('aria-label') === t('Send message'), 'send has an accessible name');
    window.chatChecksStage = 'composer';
    input(''); assert($('run').disabled, 'empty send disabled');
    input('  \n  '); key(); assert(calls.length === 0, 'whitespace not submitted');
    const long = 'Very long draft line with umlauts äöü\n'.repeat(200) + 'x'.repeat(1800);
    input(long); assert($('prompt').value === long, 'long pasted text preserved');
    assert($('prompt').scrollHeight > $('prompt').clientHeight, 'long draft scrollable');
    assert($('run').getBoundingClientRect().bottom <= innerHeight, `send visible with long draft: ${JSON.stringify({run:$('run').getBoundingClientRect().toJSON(),height:innerHeight,workspace:$('workspace').getBoundingClientRect().toJSON(),metrics:$('open-measurements').getBoundingClientRect().toJSON()})}`);
    assert(document.documentElement.scrollWidth <= innerWidth, 'draft does not overflow horizontally');
    input('Remember lighthouse.');
    assert(!key({shiftKey:true}) && !key({isComposing:true}) && !key({keyCode:229}), 'newline and IME not intercepted');
    $('prompt').dispatchEvent(new CompositionEvent('compositionstart'));
    key(); assert(calls.length === 0, 'composition fallback not sent');
    $('prompt').dispatchEvent(new CompositionEvent('compositionend'));
    key({repeat:true}); assert(calls.length === 0, 'held Enter not sent');
    key(); assert(calls.length === 1 && $('prompt').value === '', 'Enter sends once and clears submitted draft');
    input('My next draft'); key(); assert(calls.length === 1 && $('prompt').value === 'My next draft', 'no duplicate during generation; next draft preserved');
    showPage('connect-page'); showPage('test-page');
    assert(!$('test-page').hidden && controller && $('prompt').value === 'My next draft', 'returning to a running quick test preserves the draft and keeps Stop reachable');
    await tick();
    const answer = 'A complete visible line.\n'.repeat(90) + '<script>never executed</script>' + 'z'.repeat(1400);
    emit({response:answer});
    for (let i=0; i<100 && (!$('output').textContent.includes('z'.repeat(1400))); i++) await tick();
    assert($('output').markdownSource === answer && $('output').textContent.includes('z'.repeat(1400)) && !$('output').querySelector('script'), `full safe Markdown rendering: ${JSON.stringify({source:$('output').markdownSource?.length,expected:answer.length,text:$('output').textContent.length,focus:document.activeElement?.tagName,pending:pendingMarkdown.size})}`);
    assert($('transcript').scrollHeight > $('transcript').clientHeight, 'history scrollable');
    assert(document.documentElement.scrollWidth <= innerWidth, 'long reply wraps');
    $('transcript').scrollTop = 0; $('transcript').dispatchEvent(new Event('scroll'));
    const firstText = $('output').querySelector('p').firstChild;
    const selection = window.getSelection(), range = document.createRange();
    range.setStart(firstText, 0); range.setEnd(firstText, 10); selection.removeAllRanges(); selection.addRange(range);
    const selectedText = selection.toString();
    emit({response:'\nLast line.'}); await tick();
    assert(selection.toString() === selectedText, 'streaming does not destroy a text selection');
    selection.removeAllRanges(); await tick();
    assert($('transcript').scrollTop === 0 && !$('latest').hidden, 'reading older text is not interrupted');
    $('latest').click(); assert($('latest').hidden && $('transcript').scrollTop > 0, 'latest returns to bottom');
    finish(false); await idle();
    assert(conversation.length === 2 && conversation[1].content === answer+'\nLast line.', 'full reply kept in session context');
    assert($('prompt').value === 'My next draft', 'generation does not overwrite next draft');
    assert(lastReply.tokens === 32 && lastReply.rate === 32 && $('result').querySelector('.reply-metrics').title.includes(rateText(32)), 'speed uses backend tokens, not streamed chunks');
    assert(document.querySelector('.reply-metrics').getAttribute('aria-label').includes('32') && !/tok\/s|Token\/s/.test(document.querySelector('.reply-metrics').textContent), '#123: the reply shows its time; rate and tokens are its tooltip and accessible name');
    const untranslatedAnswer = $('output').markdownSource;
    const replyCopy=$('copy'), originalCopyText=copyText;
    try {
      let copiedSource;
      copyText=async value=>{copiedSource=value;};
      assert(!replyCopy.disabled && replyCopy.querySelector('svg') && !replyCopy.textContent.trim() && replyCopy.ariaLabel===t('Copy response'), 'reply copy is an accessible icon without visible text');
      replyCopy.click();await tick();
      assert(copiedSource===untranslatedAnswer && replyCopy.dataset.copied==='true' && !replyCopy.textContent.trim() && $('chat-announcement').textContent===t('Response copied.'), 'copy preserves complete source and announces icon feedback');
      copyText=async()=>{throw new Error('Controlled clipboard failure');};
      replyCopy.click();await tick();
      assert($('notice').textContent===t('Copy is unavailable here. Select the result and copy it manually.') && $('output').markdownSource===untranslatedAnswer, 'clipboard failure retains output and offers manual copy');
    } finally {copyText=originalCopyText;}
    uiText($('connection-result'), 'Copied. The configuration contains your private local key.');
    $('ui-language').value='de'; $('ui-language').dispatchEvent(new Event('change')); await tick();
    assert(document.documentElement.lang === 'de' && $('new-chat').title === 'Chat löschen', 'language switch updates document and icon tooltips');
    assert(!replyCopy.textContent.trim() && replyCopy.querySelector('svg') && replyCopy.ariaLabel===t(replyCopy.dataset.copied==='true'?'Copied':'Copy response'), 'reply icon labels localize without restoring Copy text');
    assert(rateText(32.5) === '32,5 Token/s' && bytes(1500000000) === '1,50 GB', 'German numbers and units');
    assert($('connection-result').textContent === t('Copied. The configuration contains your private local key.'), 'existing interface statuses switch language');
    assert($('output').markdownSource === untranslatedAnswer && $('prompt').value === 'My next draft', 'language change leaves answer and draft untouched');
    $('ui-language').value='en'; $('ui-language').dispatchEvent(new Event('change')); await tick();
    assert(rateText(32.5) === '32.5 tokens/s' && $('new-chat').title === 'Clear chat', 'switching back restores English');
    window.chatChecksStage = 'metrics';
    assert($('history-cpu-rate').closest('.execution-choice') && $('history-gpu-rate').closest('.execution-choice'), 'typical speed belongs to each processor choice');
    { // #81: a processor without a reply says so; the GPU backend is matched by its reported name, Vulkan too.
      const saved = state, model = state.models.find(m => m.id === state.active_id) || state.models[0];
      render({...state, execution:{...state.execution, gpu_available:true, gpu_backend:'vulkan'},
        performance_profile:{...(state.performance_profile||{}), artifact:model.sha256, cpu:null, gpu:null, recent:[{id:'v',backend:'vulkan',source:'app',outcome:'completed',engine:null,timestamp:Date.now()/1000-3600,generation_ns:1e9,input:20,output:40,warmup:false,contention:false,historical:true}]}});
      assert($('history-cpu-rate').textContent === t('No CPU reply yet') && $('history-gpu-rate').textContent === t('Historical'), '#81: missing CPU reply named, Vulkan history recognised');
      render(saved);
    }
    { // #82: status and notices stay true during a switch, after a reply and after a lost connection.
      const saved = state, other = state.models.find(m => m.id !== state.active_id);
      render({...saved, job_model: other.id, phase: 'verifying', background_download: false, busy: true, inference_busy: true});
      assert(cards.get(saved.active_id).querySelector('.variant-active').hidden && $('runtime-state').getAttribute('aria-label') === t('Getting ready…'), '#82: the old model is not "Active"/"ready" while another is prepared');
      assert($('connection-disabled-text').textContent === t('The model is switching. Wait until it is ready.'), '#82: Connect explains the switch');
      render(saved);
      controller = {}; render({...saved, message: 'Download complete.'}); controller = null;
      render({...saved, message: 'Download complete.'});
      assert($('notice').textContent === t('Download complete.'), '#82: a notice that arrived during a reply is shown afterwards');
      message('Service unavailable. Reopen geisten to reconnect.', false); lastServerMessage = '';
      render({...saved, message: ''});
      assert($('notice').textContent === '', '#82: the first successful poll clears "Service unavailable"');
      render({...saved, ready: false, loading: false, active_id: '', execution: {...saved.execution, notice: 'GPU stopped or failed to load. Restored CPU.'}});
      assert($('execution-notice').hidden, '#82: no fallback notice without a model');
      render(saved);
    }
    { // #90: the fit reason states the measured numbers, or the earlier build's and that this one is unmeasured.
      const saved = state, id = saved.models[0].id, reason = "Fits this Mac's memory. Speed measured on this Mac.";
      const withModel = patch => ({...saved, models: saved.models.map(m => m.id === id ? {...m, resource_fit: 0, ...patch} : m)});
      render(withModel({reason, speed: {cpu: 13.2, gpu: 30.4}, earlier: null}));
      assert(cards.get(id).querySelector('.model-fit').title.endsWith(`${t("Fits this Mac's memory.")} CPU ${formatNumber(13)} t/s · GPU ${formatNumber(30)} t/s`), '#90: measured numbers instead of "measured"');
      render(withModel({reason: "Fits this Mac's memory. Speed measured with an earlier geisten version.", speed: {cpu: null, gpu: null}, earlier: {engine: '0.10.2', cpu: 12.6, gpu: 29.5}}));
      const title = cards.get(id).querySelector('.model-fit').title;
      assert(title.includes(`${t('geisten engine')} 0.10 · CPU ${formatNumber(13)} t/s · GPU ${formatNumber(30)} t/s`) && title.endsWith(t('Not measured with this version yet.')), '#90: earlier numbers with their engine, this version unmeasured');
      render(saved);
    }
    { // #102: reference test as a symbol with the share; words in the tooltip; older suites marked.
      const saved = state, id = saved.models[0].id;
      const evidence = {suite: 'aaaaaaaaaaaa', date: '2026-10-03', engine: '33db79d7764b', evidence: 'a'.repeat(64), tasks: {classify: {de: [19, 20], en: [18, 20]}, context: {de: [20, 20], en: [15, 20]}}};
      const withQuality = (q, suite) => ({...saved, quality_suite: suite, models: saved.models.map(m => m.id === id ? {...m, quality_evidence: q} : m)});
      render(withQuality(evidence, 'aaaaaaaaaaaa'));
      let badge = cards.get(id).querySelector('.model-quality');
      assert(badge.dataset.tested === 'current' && badge.textContent === '90%', '#102: the share of correct answers next to the symbol');
      assert(badge.title === `${t('Reference test')}: 72/80 ${t('correct')} (DE 39/40 · EN 33/40) · ${t('Classification')} 37/40 · ${t('Facts from context')} 35/40`, '#102: counts per language and task in the tooltip');
      assert(cards.get(id).querySelector('.model-badges').getAttribute('aria-label').includes(badge.title), '#102: the words reach assistive technology');
      render(withQuality(evidence, 'bbbbbbbbbbbb'));
      badge = cards.get(id).querySelector('.model-quality');
      assert(badge.dataset.tested === 'older' && badge.title.endsWith(t('older test version')), '#102: another suite version is an older test, not current');
      render(withQuality(null, 'aaaaaaaaaaaa'));
      badge = cards.get(id).querySelector('.model-quality');
      assert(badge.dataset.tested === 'none' && badge.textContent === '' && badge.title === t('Answer quality not tested yet'), '#102: no evidence, no number');
      render(saved);
    }
    { // #103: one verdict symbol per model, words in the tooltip; one recommendation sentence.
      const saved = state, first = saved.models[0], second = saved.models.find(m => m.id !== first.id && !cards.get(m.id).querySelector('.model-pick').disabled);
      assert(second, '#103: the fixture has a selectable model');
      const verdict = (value, reason, extra = {}) => ({value, reason, processor: null, seconds: {cpu: null, gpu: null}, passed: null, total: null, ...extra});
      const snapshot = (a, b, best) => ({...saved, best_choice: best, models: saved.models.map(m => m.id === first.id ? {...m, verdict: a} : m.id === second.id ? {...m, verdict: b} : m)});
      render(snapshot(verdict('good', 'good', {processor: 'gpu', seconds: {cpu: 9.6, gpu: 3.2}, passed: 146, total: 160}), verdict('not_recommended', 'unreliable', {passed: 31, total: 160}), {id: first.id, verdict: 'good', processor: 'gpu'}));
      const mark = id => cards.get(id).querySelector('.model-verdict');
      assert(mark(first.id).textContent === '✓' && mark(second.id).textContent === '✗', '#103: symbols, not words');
      assert(mark(first.id).title.split('\n')[0] === `${t('Good choice')} · ${t('about 3 s per answer on the graphics chip')} · 146/160 ${t('correct')}`, '#103: plain reason with seconds and processor');
      assert(mark(first.id).title.split('\n')[1] === verdictBasis({...first, verdict: verdict('good', 'good', {processor: 'gpu', seconds: {cpu: 9.6, gpu: 3.2}, passed: 146, total: 160})}), '#103: the tooltip names what the verdict rests on');
      assert(mark(second.id).title.split('\n')[0] === `${t('Not recommended here')} · ${t('Reference test')}: 31/160 ${t('correct')}`, '#103: the reason names the evidence');
      assert(!$('best-choice').hidden && $('best-choice').textContent.startsWith(`✓ ${t('Recommended here')}: `) && !/t\/s|Q4_K_M|Q8_0/.test($('best-choice').textContent), '#103: recommendation in plain words');
      assert(cards.get(second.id).querySelector('.model-pick').getAttribute('aria-label').includes(t('Not recommended here')) && !cards.get(second.id).querySelector('.model-pick').disabled, '#103: a ✗ explains and never blocks');
      render(snapshot(verdict('good', 'good', {basis: 'estimated', processor: 'cpu', seconds: {cpu: 4.4, gpu: null}, passed: 146, total: 160}), verdict('not_recommended', 'unreliable', {passed: 31, total: 160}), null));
      assert(mark(first.id).title.includes(`${t('about 4 s per answer on the processor')} (${t('estimated, download to measure')})`), '#103: an estimate is always labelled');
      render(snapshot(verdict('unknown', 'speed_unknown', {passed: 146, total: 160}), verdict('unknown', 'quality_unknown'), {id: first.id, verdict: 'unknown', processor: null}));
      assert(mark(first.id).textContent === '?' && $('best-choice').textContent.startsWith(`? ${t('Best option here')}: `), '#103: no "good" without a measurement');
      render({...saved, best_choice: null});
      assert($('best-choice').textContent === t('No model fits this computer well.') && $('best-action').hidden && $('recommend-action').hidden, '#122: nothing fits: said plainly, no action');
      render(saved);
    }
    { // #103: measure speed on demand: the active model directly, another one after starting it; cancel while running.
      const saved = state, originalApi = api, originalChoose = choose, originalPoll = poll, calls = [];
      // Only the measurement calls are stubbed; a background status poll goes to the service.
      api = async (path, body, signal) => { if (!path.startsWith('/app/performance/')) return originalApi(path, body, signal); calls.push(path); return {ok: true, json: async () => ({})}; };
      poll = async () => {};
      choose = async id => { calls.push(`choose:${id}`); };
      const unmeasured = {value: 'unknown', reason: 'speed_unknown', processor: null, seconds: {cpu: null, gpu: null}, passed: 146, total: 160};
      const [a, b] = saved.models.filter(m => m.resource_fit !== 2).slice(0, 2);
      const snap = extra => ({...saved, ready: true, loading: false, busy: false, phase: '', job_model: '', active_id: a.id, comparison: {running: false, step: 0, phase: '', result: ''},
        models: saved.models.map(m => m.id === a.id || m.id === b.id ? {...m, installed: true, verdict: unmeasured} : m), ...extra});
      try {
        render(snap());
        const button = id => cards.get(id).querySelector('.measure');
        assert(!button(a.id).hidden && !button(a.id).disabled && button(a.id).getAttribute('aria-label').startsWith(t('Measure speed on this computer')), '#103: a symbol button for unmeasured installed models');
        await measureSpeed(a.id);
        assert(calls.includes('/app/performance/compare'), '#103: the active model is measured directly');
        calls.length = 0; await measureSpeed(b.id);
        assert(calls[0] === `choose:${b.id}` && measuringModel === b.id, '#103: another model is started first');
        calls.length = 0; render(snap({active_id: b.id}));
        await new Promise(resolve => setTimeout(resolve, 0));
        assert(calls.includes('/app/performance/compare') && measuringModel === null, '#103: then measured once it is ready');
        render(snap({active_id: b.id, comparison: {running: true, step: 3, phase: 'measuring', result: ''}}));
        assert(button(b.id).dataset.running === 'true' && cards.get(b.id).querySelector('.download-state').textContent === `${t('Measuring speed')} · 3/${saved.execution?.gpu_available ? 8 : 4}`, '#103: progress on the card');
        assert(button(a.id).disabled, '#103: one measurement at a time');
        calls.length = 0; await measureSpeed(b.id);
        assert(calls[0] === '/app/performance/cancel', '#103: the same button cancels');
        render(snap({models: saved.models.map(m => m.id === a.id ? {...m, installed: true, verdict: {...unmeasured, processor: 'gpu', seconds: {cpu: null, gpu: 3}}} : m)}));
        assert(cards.get(a.id).querySelector('.measure').hidden, '#103: no button once measured');
      } finally { api = originalApi; choose = originalChoose; poll = originalPoll; measuringModel = null; render(saved); }
    }
    { // #103: compare view: chart and its table equivalent, thresholds and intent per computer.
      const saved = state, originalApi = api, originalPoll = poll, sent = [];
      // Only settings calls: a background status poll may arrive at any time.
      api = async (path, body, signal) => { if (path !== '/app/verdict-settings') return originalApi(path, body, signal); sent.push([path, body]); return {ok: true, json: async () => ({})}; };
      poll = async () => {};
      const [a, b] = saved.models.filter(m => m.resource_fit !== 2).slice(0, 2);
      const snapshot = {...saved, limits: {fast_s: 10, usable_s: 30, reliable: .9, intent: 'chat'}, models: saved.models.map(m =>
        m.id === a.id ? {...m, installed: true, verdict: {value: 'good', reason: 'good', basis: 'measured', processor: 'gpu', seconds: {cpu: 9.6, gpu: 3.2}, measured_at: {cpu: 1790000000, gpu: 1790000000}, estimated_from: 0, passed: 146, total: 160}}
        : m.id === b.id ? {...m, installed: true, verdict: {value: 'unknown', reason: 'speed_unknown', processor: null, seconds: {cpu: null, gpu: null}, passed: 31, total: 160}}
        : {...m, installed: false})};
      try {
        render(snapshot);
        $('open-compare').click();
        assert($('compare-dialog').open && $('compare-chart').querySelectorAll('.compare-point').length === 2, '#103: one point per measured processor');
        assert($('compare-chart').querySelector('.compare-zone') && $('compare-chart').getAttribute('role') === 'img' && $('compare-chart').getAttribute('aria-labelledby') === 'compare-caption', '#103: the zone, and a labelled chart');
        const rows = [...$('compare-table').tBodies[0].rows];
        assert(rows.length === 2 && rows[0].cells[2].textContent === `${formatNumber(9.6, 1)} s` && rows[0].cells[4].textContent === '146/160' && rows[1].cells[2].textContent === '–', '#103: the table has the same values');
        assert($('verdict-fast').value === '10' && $('verdict-reliable').value === '90' && $('verdict-intent').value === 'chat', '#103: current thresholds shown');
        const basis = rows[0].querySelector('.compare-basis').title;
        assert(basis.startsWith(t('Speed measured on this computer')) && basis.includes('GPU') && rows[1].querySelector('.sr-only:last-child').textContent.includes(t('Answer quality not tested yet')) === !b.quality_evidence, '#103: each row names what it rests on');
        $('compare-chart').querySelector(`.compare-point[data-id="${a.id}"] circle`).dispatchEvent(new MouseEvent('click', {bubbles: true}));
        assert(rows[0].classList.contains('highlight') && document.activeElement === rows[0], '#103: a point leads to its row');
        $('verdict-fast').value = '15'; $('verdict-intent').value = 'context'; $('verdict-intent').dispatchEvent(new Event('change'));
        await new Promise(resolve => setTimeout(resolve, 0));
        assert(JSON.stringify(sent.at(-1)) === JSON.stringify(['/app/verdict-settings', {intent: 'context', fast_s: 15, usable_s: 30, reliable: .9}]), '#103: changes are saved per computer');
        $('verdict-reset').click(); await new Promise(resolve => setTimeout(resolve, 0));
        assert(JSON.stringify(sent.at(-1)) === JSON.stringify(['/app/verdict-settings', {reset: true}]), '#103: restore defaults');
        $('close-compare').click();
        assert(!$('compare-dialog').open, '#103: the view closes');
      } finally { api = originalApi; poll = originalPoll; if ($('compare-dialog').open) $('compare-dialog').close(); render(saved); }
    }
    { // #121: one row per model; the verdict as a chip; size · share · seconds below; no contradicting symbols.
      const saved = state, model = saved.models.find(m => m.resource_fit !== 2 && saved.models.filter(x => (x.group_id || x.id) === (m.group_id || m.id)).length === 1);
      const verdict = {value: 'not_recommended', reason: 'unreliable', basis: 'measured', processor: 'gpu', seconds: {cpu: null, gpu: 2.6}, measured_at: {cpu: null, gpu: 1790000000}, estimated_from: 0, passed: 31, total: 160};
      try {
        render({...saved, ready: true, loading: false, phase: '', active_id: model.id, models: saved.models.map(m => m.id === model.id ? {...m, installed: true, verdict} : m)});
        const card = cards.get(model.id), shown = selector => getComputedStyle(card.querySelector(selector)).display !== 'none';
        assert(card.classList.contains('active') && shown('.ring-active') && !shown('.ring-check') && card.querySelector('.model-verdict').textContent === '✗', '#121: active and ✗ without a second, green check');
        assert(card.querySelector('.model-title .model-verdict') && card.querySelector('.model-name').textContent === (model.group_name || model.name), '#121: name and verdict share the first row');
        assert(getComputedStyle(card.closest('.model-group').querySelector('h3')).display === 'none', '#121: no group heading for a single variant');
        const meta = card.querySelector('.model-badges');
        assert(meta.querySelector('.model-size').textContent === `${t(plainVariant(model))} · ${bytes(model.bytes)}` && meta.querySelector('.model-quality').textContent === (qualityCounts(model) ? `${Math.round(100 * qualityCounts(model).passed / qualityCounts(model).total)}%` : '') && meta.querySelector('.model-seconds').textContent === '~3 s', '#121: size · share · seconds in the second row');
        assert(!shown('.model-badges [data-capability="chat"]'), '#121: text chat is implied; only other capabilities get a symbol');
        assert(card.getBoundingClientRect().height <= 96, `#121: a compact card (${card.getBoundingClientRect().height}px)`);
        assert($('model-legend').tagName === 'DETAILS' && !$('model-legend').open && $('model-legend').textContent.includes(t('Good choice')), '#121: the legend is one click away and still names every symbol');
      } finally { render(saved); }
    }
    { // #123: layers 1-2 speak plainly: no t/s, weight formats, backend or RSS in the visible text of the main view.
      const visible = root => { const out = []; const walk = node => {
        if (node.nodeType === 3) { out.push(node.textContent); return; }
        if (node.nodeType !== 1 || node.matches('dialog, .sr-only, [hidden]')) return;
        const style = getComputedStyle(node);
        if (style.display === 'none' || style.visibility === 'hidden' || (style.position === 'absolute' && style.clip === 'rect(0px, 0px, 0px, 0px)')) return;
        node.childNodes.forEach(walk); };
        walk(root); return out.join(' '); };
      const text = visible($('models-page'));
      const jargon = text.match(/tok\/s|Token\/s|tokens\/s|Q4_K_M|Q4_0|Q8_0|I2_S|PQ2_0|\bMetal\b|\bRSS\b|\bCPU\b|\bGPU\b/g);
      assert(!jargon, `#123: technical terms in the first view: ${JSON.stringify(jargon)}`);
      assert($('runtime-name').title.includes(' · ') && $('history-cpu').closest('label').title.includes($('history-cpu-rate').textContent), '#123: the details remain as tooltips');
    }
    { // #126: one unit for speed; one divider between settings and a group; card actions in one column.
      assert(rateText(1).endsWith(` ${t('tokens/s')}`) && !document.body.innerHTML.includes('tok/s'), '#126: tokens/s (Token/s in German) everywhere');
      showPage('settings-page');
      const doubled = [...document.querySelectorAll('#settings-page .settings-row')].filter(row => row.nextElementSibling?.classList.contains('preference-group') && parseFloat(getComputedStyle(row).borderBottomWidth) > 0);
      assert(!doubled.length, '#126: no double divider before a settings group');
      showPage('models-page');
      const saved = state, model = saved.models.find(m => m.resource_fit !== 2);
      render({...saved, models: saved.models.map(m => m.id === model.id ? {...m, installed: true, verdict: {value: 'unknown', reason: 'speed_unknown', processor: null, seconds: {cpu: null, gpu: null}, passed: null, total: null}} : m)});
      const card = cards.get(model.id), remove = card.querySelector('.remove').getBoundingClientRect(), measure = card.querySelector('.measure').getBoundingClientRect(), box = card.getBoundingClientRect();
      assert(!card.querySelector('.measure').hidden && Math.abs(remove.left - measure.left) < 1 && measure.top >= remove.bottom - 1 && measure.right <= box.right + 1 && measure.width >= 44 && measure.height >= 44, '#126: remove and measure share one column inside the card');
      render(saved);
    }
    { // #122: one recommendation: the sentence, the card tag and both buttons name the same model; the action fits.
      const saved = state, originalChoose = choose, chosen = [];
      choose = async id => { chosen.push(id); };
      const model = saved.models.find(m => m.resource_fit !== 2);
      const snap = (action, verdict = 'good') => ({...saved, phase: '', loading: false, best_choice: {id: model.id, verdict, action, processor: null, basis: null},
        models: saved.models.map(m => m.id === model.id ? {...m, verdict: {value: verdict, reason: verdict === 'good' ? 'good' : 'speed_unknown', processor: null, seconds: {cpu: null, gpu: null}, passed: 146, total: 160}} : m)});
      try {
        render(snap('download'));
        const tag = [...document.querySelectorAll('.variant-recommended')].filter(e => !e.hidden).map(e => e.closest('.model').dataset.id);
        assert(JSON.stringify(tag) === JSON.stringify([model.id]) && $('best-action').dataset.id === model.id && $('recommend-action').dataset.id === model.id, '#122: one model in every place');
        assert($('best-action').textContent === `${t('Download')} (${bytes(model.bytes)})` && !$('recommend-action').hidden && $('model-prompt-hint').textContent.startsWith($('best-choice').textContent) && $('best-choice').title === $('model-prompt-hint').textContent, '#122/#125: download with its size; the empty pane gives the full sentence, the sidebar the short one with it as tooltip');
        $('recommend-action').click();
        assert(chosen[0] === model.id, '#122: the action starts the normal download/start');
        render(snap('start', 'usable'));
        assert($('best-action').textContent === t('Start') && $('best-choice').textContent.startsWith(`◐ ${t('Best option here')}: `), '#122: an installed option starts');
        render(snap('active'));
        assert($('best-action').hidden && $('recommend-action').hidden, '#122: no action for the active model');
        render({...snap('download'), phase: 'downloading'});
        assert($('best-action').hidden, '#122: no second action while a model is being prepared');
      } finally { choose = originalChoose; render(saved); }
    }
    { // #83: model actions are locked during a comparison; a pause says "Cancelling…" until the job ends.
      const saved = state, idle = saved.models.find(m => !m.installed && m.id !== saved.active_id && m.resource_fit !== 2);
      assert(idle, '#83: the fixture offers a model to download');
      {
        render({...saved, comparison: {...(saved.comparison || {}), running: true}});
        assert(cards.get(idle.id).querySelector('.model-pick').disabled, '#83: no download or start during a comparison');
        render(saved);
        cancellingModel = idle.id;
        render({...saved, job_model: idle.id, phase: 'downloading', background_download: true, received: 1});
        assert(cards.get(idle.id).querySelector('.download-state').textContent === t('Cancelling…') && cards.get(idle.id).querySelector('.model-pick').disabled, '#83: a pause is shown once and cannot be sent twice');
        render({...saved, job_model: '', phase: ''});
        assert(cancellingModel === null, '#83: the pause state ends with the job');
      }
      render(saved);
    }
    { // #82: a follow-up names the conversation's model, so the server's guard catches a model switch.
      const saved = state, original = api, keep = [conversation, conversationModel, [...$('result').children], $('result').hidden, $('chat-empty').hidden, $('prompt').value]; let sent = null;
      conversation = [{role: 'user', content: 'Hi'}, {role: 'assistant', content: 'Hello'}]; conversationModel = 'previous-model';
      api = async (path, body, signal) => {
        if (path !== '/app/generate') return original(path, body, signal);
        sent = body; throw new Error('The loaded model changed. Clear the chat to continue with the new model.');
      };
      try { await run('A follow-up question'); } finally { api = original; }
      assert(sent && sent.model === 'previous-model' && sent.messages.length === 3, '#82: the conversation is not sent silently to a different model');
      [conversation, conversationModel] = keep; $('result').replaceChildren(...keep[2]); $('result').hidden = keep[3]; $('chat-empty').hidden = keep[4]; $('prompt').value = keep[5];
      render(saved);
    }
    { // #93: a live status before the answer; the thinking is collapsed plain text, outside the answer.
      const saved = state, original = api, keep = [conversation, conversationModel, [...$('result').children], $('result').hidden, $('chat-empty').hidden, $('prompt').value];
      let push = null, finish = null;
      api = async (path, body, signal) => path !== '/app/generate' ? original(path, body, signal)
        : new Response(new ReadableStream({start(c) { push = item => c.enqueue(new TextEncoder().encode(JSON.stringify(item) + '\n')); finish = () => c.close(); }}));
      conversation = []; conversationModel = '';
      const finished = run('Think first, please');
      for (let i = 0; i < 100 && !push; i++) await tick();
      const status = () => $('result').lastElementChild.querySelector('.message-status').textContent;
      push({phase: 'prefill', tokens: 1240}); for (let i = 0; i < 5; i++) await tick();
      assert(status().startsWith(t('Reading your input')) && status().includes(formatNumber(1240)), '#93: the input read is shown with its size');
      push({phase: 'preparing', tokens: 0}); push({thinking: '<b>plan</b> step one. '}); push({phase: 'preparing', tokens: 12}); push({thinking: 'step two.'});
      for (let i = 0; i < 5; i++) await tick();
      assert(status().startsWith(t('Thinking')) && status().includes(`12 ${t('tokens')}`), '#93: thinking shows time and tokens');
      const details = $('result').lastElementChild.querySelector('details.thinking');
      assert(details && !details.open && details.querySelector('.thinking-text').textContent === '<b>plan</b> step one. step two.' && !details.querySelector('b'), '#93: the thinking is collapsed plain text');
      push({response: 'The answer.'}); push({done: true, eval_count: 20, eval_duration: 1e9}); finish(); await finished;
      assert(status() === '' && conversation.at(-1).content === 'The answer.' && $('output').markdownSource === 'The answer.', '#93: the thinking is not in the answer, its copy or the conversation');
      api = original;
      [conversation, conversationModel] = keep; $('result').replaceChildren(...keep[2]); $('result').hidden = keep[3]; $('chat-empty').hidden = keep[4]; $('prompt').value = keep[5];
      render(saved);
    }
    assert(document.querySelector('.profile-table caption') && document.querySelectorAll('.profile-table th[scope="row"]').length===12, 'profile uses a semantic comparison table');
    assert(!$('measurement-note') && !$('speed'), 'old nested measurements removed');
    assert($('history-enabled').closest('#settings-page') && $('history-export').closest('#settings-page'), 'collection and export belong to settings');
    if (!window.geistDesktop) { // #101 in a browser: a dated download of the service's export.
      const originalClick = HTMLAnchorElement.prototype.click, downloads = [];
      HTMLAnchorElement.prototype.click = function() { downloads.push([this.download, this.href]); };
      try {
        $('history-export').click(); for (let i = 0; i < 20; i++) await tick();
        assert(downloads.length === 1 && /^geisten-measurements-\d{4}-\d{2}-\d{2}\.jsonl$/.test(downloads[0][0]) && downloads[0][1].startsWith('blob:'), '#101: the browser downloads a dated file');
        assert($('history-result').textContent.includes(downloads[0][0]), '#101: the file name is shown');
      } finally { HTMLAnchorElement.prototype.click = originalClick; uiText($('history-result'), ''); }
    } else { // #101: the export asks the native window where to save; nothing goes into the data folder.
      const original = desktopMessage, calls = [];
      desktopMessage = async (action, value, wait) => { calls.push([action, value, wait]); return calls.length === 1 ? `/home/u/Downloads/${value}` : 'cancelled'; };
      try {
        $('history-export').click(); for (let i = 0; i < 20; i++) await tick();
        assert(calls[0][0] === 'export' && /^geisten-measurements-\d{4}-\d{2}-\d{2}\.jsonl$/.test(calls[0][1]) && calls[0][2] >= 60000, '#101: the window is asked to save, with a dated name and time for the dialog');
        assert($('history-result').textContent.includes('/home/u/Downloads/geisten-measurements-'), '#101: the saved place is shown');
        $('history-export').click(); for (let i = 0; i < 20; i++) await tick();
        assert($('history-result').textContent === '', '#101: a cancelled dialog leaves no message');
      } finally { desktopMessage = original; }
    }

    const measuredReply = lastReply;
    const savedState = state;
    state = {...savedState, memory:{process_rss_bytes:2**30,process_rss_sample_age_ms:0,status:2,gpu_unavailable_reason:'unsupported'}, resources:{scope:'geistd', rss_bytes:2**30, cpu_percent:0}}; renderPerformance();
    assert($('test-memory').textContent === '1.0 GiB' && $('performance-cpu').textContent === '0.0 %', 'real zero CPU differs from unknown');
    state = {...savedState, memory:{process_rss_bytes:null,status:0}, resources:{scope:'geistd', rss_bytes:null, cpu_percent:null}}; renderPerformance();
    assert($('test-memory').textContent === '—' && $('performance-cpu').textContent === '—', 'unknown resource counters are not zero');
    const beforeMemoryLayout=$('transcript').getBoundingClientRect().toJSON();
    stateReceivedAt=performance.now();
    state={...savedState,memory:{process_rss_bytes:.3*2**30,process_rss_sample_age_ms:0,status:1,gpu_allocated_bytes:10*2**30,gpu_sample_age_ms:0,gpu_source:'metal.MTLDevice.currentAllocatedSize',unified_memory:true},resources:{scope:'geistd',cpu_percent:0}};
    renderPerformance();
    assert($('test-memory').textContent==='0.3 GiB' && $('test-gpu-memory').textContent==='10.0 GiB', 'small RSS and large synthetic Metal allocation have separate scopes');
    const metricsBounds=document.querySelector('.model-metrics').getBoundingClientRect();
    for(const id of ['test-memory','test-gpu-memory','test-size']) {
      const bounds=$(id).parentElement.getBoundingClientRect();
      assert(bounds.top>=metricsBounds.top && bounds.bottom<=metricsBounds.bottom+1, `${id} fits inside the three-row summary without clipping`);
    }
    assert($('memory-live').textContent.includes('Process RSS') && $('memory-source').textContent.includes('values overlap'), 'scopes and shared-memory overlap are visible');
    // #52: figures live behind Measurements; sources are words; the main view keeps model, processor, speed.
    assert(!document.querySelector('.runtime-panel #test-memory, .runtime-panel #test-gpu-memory') && $('test-memory').closest('dialog'),'#52: RSS and Metal figures are in the Measurements dialog, not the main view');
    assert(!/proc_pid|MTLDevice|proc_pid_stat/.test($('memory-source').textContent + $('test-gpu-memory').title + $('test-memory').title) && $('memory-source').textContent.includes(t('Reported by Metal')),'#52: no raw API identifier is shown, a plain source is');
    assert($('open-measurements').closest('.runtime-panel') && $('open-measurements').title && $('open-measurements').textContent.trim(),'#52: Measurements stays one click away and keeps its name');
    state.memory.gpu_allocated_bytes=0;renderMemory();
    assert($('test-gpu-memory').textContent==='0.0 GiB', 'known zero Metal differs from unavailable');
    stateReceivedAt-=6100;renderMemory();
    assert($('test-gpu-memory').textContent==='—' && $('test-memory').textContent==='—' && $('test-gpu-memory').title==='Stale measurement','cached counters expire without a status response');
    assert(JSON.stringify($('transcript').getBoundingClientRect().toJSON())===JSON.stringify(beforeMemoryLayout),'memory updates do not move transcript');
    stateReceivedAt=performance.now();
    state = {...savedState, hardware:{...savedState.hardware, known:false, ram:0}}; renderPerformance();
    assert($('performance-ram').textContent === '—', 'failed system memory read is unknown, not zero');
    openMeasurements(); await tick();
    render({...savedState, active:'another model', performance_history:[],performance_profile:null});
    assert(lastReply === null && $('history-cpu-tokens').textContent === '—' && !$('performance').open, 'model change closes old details and invalidates last reply metrics');
    render(savedState); lastReply = measuredReply; renderPerformance();
    window.chatChecksStage = 'session';
    input('Which word?'); key(); await tick();
    assert(calls[1].messages.length === 3 && calls[1].messages[1].content === conversation[1].content, 'follow-up includes history without truncation');
    emit({response:'Lighthouse.'}); finish(true); await idle();
    assert($('result').querySelectorAll('article').length === 4 && $ ('result').querySelector('.assistant .markdown').markdownSource === answer+'\nLast line.', 'earlier messages retained');
    assert(!$('result').querySelector('[data-label="Continue response"]'), 'no artificial continuation action');
    assert($('result').textContent.includes(t('The model’s context limit was reached. Start a new chat or ask a shorter question.')), 'real context boundary explained');
    assert(!Object.hasOwn(calls[1], 'max_tokens'), 'app uses the remaining context rather than a 1024-token cap');
    input('Another question'); key(); await tick();
    emit({phase:'preparing'}); await tick();
    assert($('result').lastElementChild.querySelector('.message-status').textContent.startsWith(t('Thinking')) && !$('result').querySelector('details.thinking'), '#93: thinking status shown; no disclosure without thinking text');
    emit({heartbeat:true}); emit({response:'More detail.'}); finish(false); await idle();
    fail = true; input('Follow-up that does not fit'); key(); await idle();
    assert(conversation.length === 6 && $('prompt').value === 'Follow-up that does not fit', 'rejected request retains draft and previous context');
    assert($('result').textContent.includes(t('This test does not fit the model’s context. Shorten your draft or use Clear chat to start again. No earlier messages have been removed.')), 'context limit explained');
    fail = false; input('A response to stop'); key(); await tick(); emit({response:'Partial response'}); await tick(); $('stop').click(); await idle();
    assert(conversation.at(-1).content === 'Partial response' && $('result').textContent.includes(t('Stopped. Partial output is kept here.')), 'stop preserves marked partial response');
    assert(document.activeElement === $('prompt'), 'stopping returns focus to composer');
    assert(lastReply === null, 'aborted reply never reports final generation speed');
    const historyBeforeReasoning = JSON.stringify(conversation);
    input('Question with hidden preparation'); key(); await tick(); emit({phase:'preparing'}); finish(false); await idle();
    assert(JSON.stringify(conversation) === historyBeforeReasoning, 'no-answer response never enters assistant history');
    assert($('prompt').value === 'Question with hidden preparation' && $('result').textContent.includes(t('No answer was produced. Try again with a shorter question.')), 'no-answer restores draft and gives a concrete retry');
    assert($('copy').disabled, 'no answer cannot be copied');
    const beforeRetry = calls.length;
    const retry = [...$('result').querySelectorAll('article:last-child button')].find(button => button.textContent === t('Retry'));
    assert(retry, 'reasoning-only completion offers Retry'); retry.click(); await tick();
    assert(calls.length === beforeRetry + 1 && calls.at(-1).messages.length === conversation.length + 1, 'retry sends one user turn without an empty or duplicate assistant');
    emit({phase:'preparing'}); finish(false); await idle();
    input('Preparation to stop'); key(); await tick(); emit({phase:'preparing'}); await tick(); $('stop').click(); await idle();
    assert(JSON.stringify(conversation) === historyBeforeReasoning && $('result').textContent.includes(t('Stopped before an answer was produced.')), 'stop during preparation preserves history and draft');
    window.chatChecksStage = 'clear';
    const clearState=JSON.stringify([state.active_id,state.execution,state.performance_history,state.models]);
    input('Discard this draft');
    window.confirm = () => false; $('new-chat').click(); assert(conversation.length === 8, 'cancel new chat keeps text');
    assert($('prompt').value==='Discard this draft', 'cancel clear keeps the draft');
    window.confirm = () => true; $('new-chat').click(); assert(conversation.length === 0 && !$('chat-empty').hidden && !$('prompt').value, 'new chat clears only this session');
    assert(!pendingMarkdown.size && $('new-chat').disabled && !$('new-chat').hidden && document.activeElement===$('prompt'), 'clearing resets pending render, returns focus and keeps disabled icon discoverable');
    assert(JSON.stringify([state.active_id,state.execution,state.performance_history,state.models])===clearState, 'clear chat preserves model files, active execution and saved performance');
    await tick();
    const empty = $('chat-empty').querySelector('h3').getBoundingClientRect(), transcript = $('transcript').getBoundingClientRect();
    assert(empty.top >= transcript.top && empty.bottom <= transcript.bottom, `empty-state label is fully visible: ${JSON.stringify({empty:empty.toJSON(), transcript:transcript.toJSON(), scroll:$('transcript').scrollTop})}`);
    $('chat-help').open = true;
    const help = document.querySelector('.help-content').getBoundingClientRect();
    assert(help.top >= 0 && help.bottom <= innerHeight && help.left >= 0 && help.right <= innerWidth, 'help readable inside window');
    document.dispatchEvent(new KeyboardEvent('keydown', {key:'Escape', bubbles:true, cancelable:true}));
    assert(!$('chat-help').open && document.activeElement === $('chat-help').querySelector('summary'), 'Escape closes help and returns focus');
    $('chat-help').open = true;
    $('prompt').dispatchEvent(new Event('pointerdown', {bubbles:true}));
    assert(!$('chat-help').open, 'outside click dismisses help');
    showPage('models-page');
    window.chatChecksStage = 'model-details';
    closeMeasurements(); await tick();
    const draft=$('prompt'); draft.value='A multiline\ndraft with selection';resizeComposer();draft.focus();draft.setSelectionRange(2,12);
    const filler=Array.from({length:50},(_,i)=>{const p=document.createElement('p');p.textContent=`Retained message ${i}`;return p;});
    $('result').append(...filler);$('result').hidden=false;chatLayout();
    $('transcript').scrollTop=80;followLatest=false;
    const sheetTranscript=$('transcript'), composerBefore=$('task-form').getBoundingClientRect();
    const transcriptBefore=sheetTranscript.getBoundingClientRect(), scrollBefore=sheetTranscript.scrollTop;
    const summary=$('open-measurements'), footers=[...document.querySelectorAll('.reply-metrics')].map(n=>n.textContent);
    for(let iteration=0;iteration<10;iteration++) {
      window.chatChecksStage=`measurements-${iteration}-open`;
      summary.focus();summary.click();
      assert($('performance').open && document.activeElement===$('close-measurements'), 'sheet gives focus to its visible Close action');
      const panel=document.querySelector('.performance-content');panel.focus();panel.scrollTop=panel.scrollHeight;
      const bounds=$('performance').getBoundingClientRect();
      assert(panel.scrollTop>0 && panel.scrollHeight>panel.clientHeight, 'sheet content has an independent scroll region');
      assert(bounds.left>=-1 && bounds.right<=innerWidth+1 && bounds.top>=-1 && bounds.bottom<=innerHeight+1, 'sheet stays inside viewport');
      render(state);
      assert(document.activeElement===panel && $('performance').open, 'polling retains sheet and focus');
      assert(Math.abs($('task-form').getBoundingClientRect().top-composerBefore.top)<=1 && Math.abs(sheetTranscript.getBoundingClientRect().top-transcriptBefore.top)<=1, 'sheet and polling never move composer/transcript');
      window.chatChecksStage=`measurements-${iteration}-close`;
      const closed=new Promise(resolve=>$('performance').addEventListener('close',resolve,{once:true}));
      document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));await closed;
      assert(!$('performance').open && document.activeElement===summary, 'Escape returns focus to its invoker');
      assert(draft.value==='A multiline\ndraft with selection' && draft.selectionStart===2 && draft.selectionEnd===12, 'draft and selection survive inspection');
      assert(sheetTranscript.scrollTop===scrollBefore && !followLatest, 'inspection preserves the reader scroll anchor');
      assert(JSON.stringify([...document.querySelectorAll('.reply-metrics')].map(n=>n.textContent))===JSON.stringify(footers), 'answer footers are immutable');
    }
    filler.forEach(n=>n.remove());draft.value='';resizeComposer();
    showPage('test-page');
    $('prompt').focus();
    { // #124: light and dark both readable: text 7:1 on its background, symbols 3:1 on the model panel.
      const rgb = color => { const c = document.createElement('span'); c.style.color = color; document.body.append(c); const v = getComputedStyle(c).color.match(/[\d.]+/g).slice(0, 3).map(Number); c.remove(); return v; };
      const luminance = ([r, g, b]) => [r, g, b].map(v => { v /= 255; return v <= .03928 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4; }).reduce((sum, v, i) => sum + v * [.2126, .7152, .0722][i], 0);
      const contrast = (a, b) => { const [x, y] = [luminance(rgb(a)), luminance(rgb(b))].sort((p, q) => q - p); return (x + .05) / (y + .05); };
      const token = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
      const before = $('ui-appearance').value;
      for (const theme of ['light', 'dark']) {
        $('ui-appearance').value = theme; $('ui-appearance').dispatchEvent(new Event('change'));
        assert(document.documentElement.dataset.theme === theme, `#124: ${theme} applies`);
        const pairs = [['--ink', '--bg', 7], ['--ink', '--panel', 7], ['--muted', '--bg', 4.5], ['--muted', '--panel', 4.5], ['--accent', '--bg', 4.5],
          ['--good', '--panel', 3], ['--warning', '--panel', 3], ['--danger', '--panel', 3], ['--ink', '--selected', 7]];
        for (const [fg, bg, min] of pairs) assert(contrast(token(fg), token(bg)) >= min, `#124 ${theme}: ${fg} on ${bg} ${contrast(token(fg), token(bg)).toFixed(2)} < ${min}`);
      }
      assert(getComputedStyle(document.body).backgroundColor !== 'rgb(255, 255, 255)', '#124: dark is dark');
      $('ui-appearance').value = before; $('ui-appearance').dispatchEvent(new Event('change'));
      assert($('history-enabled').getAttribute('role') === 'switch' && $('history-enabled').type === 'checkbox', '#124: on/off settings are switches, still a checkbox underneath');
    }
    window.chatChecksStage = 'complete';
    window.chatChecksDone = true;
  } catch (error) { window.chatChecksError = error.message; window.chatChecksStack = error.stack; }
  finally { api = originalAPI; window.confirm = originalConfirm; }
})();
true;
