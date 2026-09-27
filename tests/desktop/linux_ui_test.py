#!/usr/bin/python3
"""Real GTK/WebKitGTK and C23 service. Run under a user D-Bus session and Xvfb."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('desktop', ROOT / 'desktop/geist_desktop.py')
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)


def spin(predicate, timeout=25):
    end = time.monotonic() + timeout
    context = d.GLib.MainContext.default()
    while time.monotonic() < end:
        while context.pending(): context.iteration(False)
        if predicate(): return
        time.sleep(.02)
    raise AssertionError('Desktop operation did not finish')


def evaluate(view, script):
    result = []
    def done(view, response, _):
        try: result.append(view.evaluate_javascript_finish(response).to_json(0))
        except d.GLib.Error as error: result.append(error)
    view.evaluate_javascript(script, -1, None, None, None, done, None)
    spin(lambda: result)
    if isinstance(result[0], Exception): raise result[0]
    return json.loads(result[0])


def wait_js(view, condition, timeout=30):
    try:
        spin(lambda: evaluate(view, condition) is True, timeout)
    except AssertionError as error:
        # Whitelist diagnostics: never print the URL, capability or connection.json.
        details = evaluate(view, "({ready: state?.ready, busy: state?.busy, generating: !!controller, connectionTesting, connectionDisabled: document.getElementById('test-connection').disabled, connectionResult: document.getElementById('connection-result').textContent, notice: document.getElementById('notice').textContent})")
        raise AssertionError(f'{condition}: {details}') from error


with tempfile.TemporaryDirectory(prefix='geist-desktop-') as temporary:
    os.environ['GEIST_HOME'] = temporary
    os.environ['GEIST_PORT'] = '0'
    model = os.environ.get('GEIST_TEST_MODEL')
    if model:
        import shutil
        models = Path(temporary) / 'models'; models.mkdir()
        shutil.copyfile(model, models / 'smollm2-360m-instruct-q8_0.gguf')
    desktop = d.Desktop(cli=ROOT / 'geist', application_id=f'com.geisten.Test{os.getpid()}', preferences=Path(temporary) / 'preferences.json')
    assert desktop.register(None)
    try:
        desktop.activate()
        spin(lambda: desktop.origin is not None)
        wait_js(desktop.view, "document.querySelectorAll('.model').length === 6")
        wait_js(desktop.view, "tasks.length === 5 && selectedTask?.id === 'freeform'")
        assert desktop.window.get_visible()
        assert desktop.view.get_network_session().is_ephemeral()
        assert evaluate(desktop.view, "document.getElementById('workspace').hidden && !document.getElementById('setup').hidden && !document.getElementById('experimental')")
        assert evaluate(desktop.view, "state.models.every(m => !m.preview_accepted)")
        connection = json.loads((Path(temporary) / 'connection.json').read_text())
        desktop.activate()
        spin(lambda: not desktop.working)
        assert json.loads((Path(temporary) / 'connection.json').read_text())['pid'] == connection['pid']
        evaluate(desktop.view, "document.getElementById('ui-language').value='de'; document.getElementById('ui-language').dispatchEvent(new Event('change')); true")
        wait_js(desktop.view, "document.documentElement.lang === 'de'")
        spin(lambda: desktop.preferences.exists())
        assert json.loads(desktop.preferences.read_text())['language'] == 'de'
        assert evaluate(desktop.view, "document.getElementById('task-title').textContent") == 'Was möchtest du ausprobieren?'
        evaluate(desktop.view, "document.querySelector('[data-page=\"test-page\"]').click(); document.getElementById('prompt').value='Keep my input'; document.getElementById('ui-language').value='en'; document.getElementById('ui-language').dispatchEvent(new Event('change')); true")
        assert evaluate(desktop.view, "document.getElementById('prompt').value") == 'Keep my input'
        evaluate(desktop.view, "window.copyDone=false; copyText('Geist desktop clipboard test').then(() => window.copyDone=true); true")
        wait_js(desktop.view, 'window.copyDone')
        clipboard = []
        def copied(source, result, _): clipboard.append(source.read_text_finish(result))
        desktop.view.get_clipboard().read_text_async(None, copied, None)
        spin(lambda: clipboard)
        assert clipboard == ['Geist desktop clipboard test']
        assert d.local_url('http://127.0.0.1:42/#test', 'http://127.0.0.1:42/')
        for uri in ('http://127.0.0.1:43/', 'http://localhost:42/', 'file:///etc/passwd', 'https://github.com/geisten/anything'):
            assert not d.local_url(uri, 'http://127.0.0.1:42/')
        evaluate(desktop.view, "location.href='https://example.com/'; true")
        spin(lambda: d.local_url(desktop.view.get_uri(), desktop.origin))
        desktop.window.set_default_size(540, 600)
        assert evaluate(desktop.view, 'document.documentElement.scrollWidth <= innerWidth')
        if model:
            evaluate(desktop.view, "document.getElementById('setup-start').click(); true")
            wait_js(desktop.view, "state?.ready === true && !document.getElementById('workspace').hidden", timeout=60)
            assert evaluate(desktop.view, "state.active_id === 'smollm2-360m' && state.models.find(m=>m.id===state.active_id).preview_accepted")
            evaluate(desktop.view, "document.querySelector('[data-task=ideas]').click(); true")
            assert evaluate(desktop.view, "selectedTask.id === 'ideas' && state.active_id === 'smollm2-360m' && !state.busy")
            evaluate(desktop.view, "document.querySelector('[data-task=ideas]').click(); true")
            assert evaluate(desktop.view, "selectedTask.id === 'freeform' && document.getElementById('run').getBoundingClientRect().bottom < innerHeight")
            evaluate(desktop.view, "document.getElementById('prompt').value='Say hello in one sentence.'; document.getElementById('task-form').requestSubmit(); true")
            wait_js(desktop.view, "document.getElementById('output').textContent.length > 0 && controller === null", timeout=90)
            assert '—' not in evaluate(desktop.view, "document.getElementById('speed').textContent")
            evaluate(desktop.view, "document.querySelector('[data-page=\"connect-page\"]').click(); true")
            # Stream completion may precede the next status poll clearing busy.
            # A click on a disabled button is discarded, so wait for the visible UI.
            wait_js(desktop.view, "!document.getElementById('test-connection').disabled")
            evaluate(desktop.view, "document.getElementById('test-connection').click(); true")
            wait_js(desktop.view, "connectionTesting || document.getElementById('connection-result').textContent.length > 0")
            wait_js(desktop.view, "!connectionTesting && document.getElementById('connection-result').textContent.startsWith('Connected.')", timeout=60)
        # Closing the UI leaves the service owned by its separate supervisor.
        desktop.window.set_visible(False)
        subprocess.run([str(ROOT / 'geist'), 'status'], check=True, stdout=subprocess.DEVNULL)
        desktop.window.present()
        subprocess.run([str(ROOT / 'geist'), 'stop'], check=True, stdout=subprocess.DEVNULL)
        desktop.check()
        spin(lambda: desktop.origin is None)
        assert desktop.stack.get_visible_child_name() == 'status'
        desktop.start()
        spin(lambda: desktop.origin is not None)
        wait_js(desktop.view, "document.querySelectorAll('.model').length === 6")
        if model:
            wait_js(desktop.view, "state?.ready && !document.getElementById('workspace').hidden && !document.getElementById('run').disabled", timeout=60)
            assert evaluate(desktop.view, "document.getElementById('prompt').value === ''")
        print('GTK/WebKit: real local UI, one-click setup, persistent preview consent, task chips without model switching, DE/EN, preferences, clipboard, navigation restrictions, resize, reactivation, shared-service reuse and stop/reconnect passed')
    finally:
        desktop.shutdown(desktop)
        if desktop.window: desktop.window.destroy()
        subprocess.run([str(ROOT / 'geist'), 'stop'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=35)
