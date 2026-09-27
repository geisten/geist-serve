// Runs in the real native WebView, with deterministic transport faults only.
// The host also runs separate real-model, clipboard and native-dialog checks.
window.chatChecksDone = false;
window.chatChecksError = null;
(async () => {
  const originalAPI = api, originalConfirm = window.confirm;
  const assert = (ok, detail) => { if (!ok) throw new Error(detail); };
  const tick = () => new Promise(resolve => setTimeout(resolve, 20));
  const idle = async () => { for (let i=0; i<200 && controller; i++) await tick(); assert(!controller, 'request completed'); };
  const key = options => { const event = new KeyboardEvent('keydown', {key:'Enter', bubbles:true, cancelable:true, ...options}); $('prompt').dispatchEvent(event); return event.defaultPrevented; };
  const input = text => { $('prompt').value = text; $('prompt').dispatchEvent(new Event('input')); };
  let stream, calls = [], fail = false;
  const emit = item => stream.enqueue(new TextEncoder().encode(JSON.stringify(item) + '\n'));
  const finish = limited => { emit({done:true, limited, eval_count:32, eval_duration:1e9}); stream.close(); };
  try {
    api = async (path, body, signal) => {
      if (path !== '/app/generate') return originalAPI(path, body, signal);
      calls.push(body);
      if (fail) throw new Error("The prompt does not fit this model's context. Try a shorter text.");
      return new Response(new ReadableStream({start(c) { stream=c; signal.addEventListener('abort', () => c.error(new DOMException('Stopped','AbortError')), {once:true}); }}));
    };
    assert(!document.querySelector('[data-task]'), 'task presets removed');
    assert($('run').getAttribute('aria-label') === t('Send message'), 'send has an accessible name');
    input(''); assert($('run').disabled, 'empty send disabled');
    input('  \n  '); key(); assert(calls.length === 0, 'whitespace not submitted');
    const long = 'Very long draft line with umlauts äöü\n'.repeat(200) + 'x'.repeat(1800);
    input(long); assert($('prompt').value === long, 'long pasted text preserved');
    assert($('prompt').scrollHeight > $('prompt').clientHeight, 'long draft scrollable');
    assert($('run').getBoundingClientRect().bottom <= innerHeight, 'send visible with long draft');
    assert(document.documentElement.scrollWidth <= innerWidth, 'draft does not overflow horizontally');
    input('Remember lighthouse.');
    assert(!key({shiftKey:true}) && !key({isComposing:true}) && !key({keyCode:229}), 'newline and IME not intercepted');
    $('prompt').dispatchEvent(new CompositionEvent('compositionstart'));
    key(); assert(calls.length === 0, 'composition fallback not sent');
    $('prompt').dispatchEvent(new CompositionEvent('compositionend'));
    key({repeat:true}); assert(calls.length === 0, 'held Enter not sent');
    key(); assert(calls.length === 1 && $('prompt').value === '', 'Enter sends once and clears submitted draft');
    input('My next draft'); key(); assert(calls.length === 1 && $('prompt').value === 'My next draft', 'no duplicate during generation; next draft preserved');
    await tick();
    const answer = 'A complete visible line.\n'.repeat(90) + '<script>never executed</script>' + 'z'.repeat(1400);
    emit({response:answer}); await tick();
    assert($('output').textContent === answer && !$('output').querySelector('script'), 'full safe text rendering');
    assert($('transcript').scrollHeight > $('transcript').clientHeight, 'history scrollable');
    assert(document.documentElement.scrollWidth <= innerWidth, 'long reply wraps');
    $('transcript').scrollTop = 0; $('transcript').dispatchEvent(new Event('scroll'));
    emit({response:'\nLast line.'}); await tick();
    assert($('transcript').scrollTop === 0 && !$('latest').hidden, 'reading older text is not interrupted');
    $('latest').click(); assert($('latest').hidden && $('transcript').scrollTop > 0, 'latest returns to bottom');
    finish(false); await idle();
    assert(conversation.length === 2 && conversation[1].content === answer+'\nLast line.', 'full reply kept in session context');
    assert($('prompt').value === 'My next draft', 'generation does not overwrite next draft');
    input('Which word?'); key(); await tick();
    assert(calls[1].messages.length === 3 && calls[1].messages[1].content === conversation[1].content, 'follow-up includes history without truncation');
    emit({response:'Lighthouse.'}); finish(true); await idle();
    assert($('result').querySelectorAll('article').length === 4 && $('result').textContent.includes(answer), 'earlier messages retained');
    assert($('result').textContent.includes(t('Continue response')), 'token limit has explicit continuation');
    fail = true; input('Follow-up that does not fit'); key(); await idle();
    assert(conversation.length === 4 && $('prompt').value === 'Follow-up that does not fit', 'rejected request retains draft and previous context');
    assert($('result').textContent.includes(t('This conversation does not fit the model’s context. Shorten your draft or start a new chat. No earlier messages have been removed.')), 'context limit explained');
    fail = false; input('A response to stop'); key(); await tick(); emit({response:'Partial response'}); await tick(); $('stop').click(); await idle();
    assert(conversation.at(-1).content === 'Partial response' && $('result').textContent.includes(t('Stopped. Partial output is kept here.')), 'stop preserves marked partial response');
    window.confirm = () => false; $('new-chat').click(); assert(conversation.length === 6, 'cancel new chat keeps text');
    window.confirm = () => true; $('new-chat').click(); assert(conversation.length === 0 && !$('chat-empty').hidden && !$('prompt').value, 'new chat clears only this session');
    $('chat-help').open = true;
    const help = document.querySelector('.help-content').getBoundingClientRect();
    assert(help.top >= 0 && help.bottom <= innerHeight && help.left >= 0 && help.right <= innerWidth, 'help readable inside window');
    $('chat-help').open = false;
    window.chatChecksDone = true;
  } catch (error) { window.chatChecksError = error.message; }
  finally { api = originalAPI; window.confirm = originalConfirm; }
})();
true;
