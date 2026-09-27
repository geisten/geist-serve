#!/usr/bin/python3
"""Thin desktop host. Model policy and inference stay in the shared C23 service."""
import json
import os
from pathlib import Path
import re
import subprocess
import threading
from urllib.parse import urlsplit

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('WebKit', '6.0')
from gi.repository import Gio, GLib, Gtk, WebKit


def local_url(candidate, origin):
    try:
        a, b = urlsplit(candidate or ''), urlsplit(origin or '')
        return a.scheme == 'http' and a.hostname == '127.0.0.1' and a.port == b.port and b.port is not None and not a.username and not a.password and a.path == '/'
    except ValueError:
        return False


def connection_url(data):
    endpoint = urlsplit(data['base_url'])
    if not re.fullmatch('[a-f0-9]{64}', data['api_key']) or endpoint.scheme != 'http' or endpoint.hostname != '127.0.0.1' or not endpoint.port or endpoint.username or endpoint.password:
        raise ValueError('Invalid local connection')
    return f"http://127.0.0.1:{endpoint.port}/#{data['api_key']}"


def external_url(uri):
    value = urlsplit(uri)
    return value.scheme == 'https' and value.hostname == 'github.com' and value.path.startswith('/geisten/') and not value.username and not value.password and not value.fragment


class Desktop(Gtk.Application):
    def __init__(self, cli='/usr/bin/geist', application_id='com.geisten.Geist', preferences=None):
        super().__init__(application_id=application_id, flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.cli = str(cli)
        self.preferences = Path(preferences or Path(GLib.get_user_config_dir()) / 'geist' / 'interface.json')
        self.language = 'de' if GLib.get_language_names()[0].startswith('de') else 'en'
        try:
            saved = json.loads(self.preferences.read_text())['language']
            if saved in ('de', 'en'): self.language = saved
        except (OSError, ValueError, KeyError, TypeError):
            pass
        self.window = self.view = None
        self.origin = self.loaded = None
        self.working = False
        self.timer = None
        self.connect('activate', self.activate_window)
        self.connect('shutdown', self.shutdown)

    def text(self, en, de):
        return de if self.language == 'de' else en

    def activate_window(self, _):
        if self.window is None:
            self.window = Gtk.ApplicationWindow(application=self, title='Geist', default_width=1080, default_height=780)
            self.window.set_size_request(540, 500)
            self.stack = Gtk.Stack()
            self.window.set_child(self.stack)
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=20, halign=Gtk.Align.CENTER, valign=Gtk.Align.CENTER)
            box.set_margin_start(24); box.set_margin_end(24)
            self.status = Gtk.Label(wrap=True, max_width_chars=55, justify=Gtk.Justification.CENTER)
            self.retry = Gtk.Button(label=self.text('Start / reconnect', 'Starten / neu verbinden'))
            self.retry.connect('clicked', lambda _: self.start())
            box.append(self.status); box.append(self.retry)
            box.append(Gtk.Label(label=self.text('Closing the window keeps the model service running.', 'Beim Schließen läuft der Modelldienst weiter.'), wrap=True))
            self.stack.add_named(box, 'status')
            manager = WebKit.UserContentManager.new()
            manager.connect('script-message-received::desktop', self.message)
            if not manager.register_script_message_handler('desktop', None):
                raise RuntimeError('Cannot register desktop messages')
            manager.add_script(WebKit.UserScript.new(
                f"window.geistLanguage = '{self.language}'; window.geistDesktop = 'linux';",
                WebKit.UserContentInjectedFrames.TOP_FRAME, WebKit.UserScriptInjectionTime.START, None, None))
            session = WebKit.NetworkSession.new_ephemeral()
            session.connect('download-started', lambda _, download: download.cancel())
            self.view = WebKit.WebView(network_session=session, user_content_manager=manager)
            self.view.get_settings().set_enable_write_console_messages_to_stdout(False)
            self.view.get_settings().set_javascript_can_open_windows_automatically(False)
            self.view.connect('decide-policy', self.policy)
            self.view.connect('load-changed', self.loaded_page)
            self.view.connect('load-failed', self.load_failed)
            self.view.connect('web-process-terminated', lambda *_: self.failed())
            self.view.connect('permission-request', lambda _, request: (request.deny(), True)[1])
            self.view.connect('context-menu', lambda *_: True)
            self.stack.add_named(self.view, 'web')
            self.timer = GLib.timeout_add_seconds(4, self.check)
        self.window.present()
        self.start()

    def shutdown(self, _):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = None
        # The shared model service is intentionally not a child of the UI.

    def failed(self):
        self.loaded = None
        self.status.set_text(self.text('The interface could not be loaded. Reconnect to try again.', 'Die Oberfläche konnte nicht geladen werden. Bitte neu verbinden.'))
        self.retry.set_sensitive(True)
        self.stack.set_visible_child_name('status')

    def load_failed(self, *_):
        self.failed()
        return True

    def loaded_page(self, view, event):
        if event == WebKit.LoadEvent.FINISHED and local_url(view.get_uri(), self.origin):
            self.stack.set_visible_child_name('web')

    def service(self, arguments):
        return subprocess.run([self.cli, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=35, check=True).stdout

    def start(self):
        if self.working: return
        self.working = True
        self.retry.set_sensitive(False)
        self.status.set_text(self.text('Starting local service…', 'Lokaler Dienst wird gestartet…'))
        if not self.loaded: self.stack.set_visible_child_name('status')
        def work():
            try:
                self.service(['start'])
                url = connection_url(json.loads(self.service(['connection'])))
            except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError):
                url = None
            GLib.idle_add(self.started, url)
        threading.Thread(target=work, daemon=True).start()

    def started(self, url):
        self.working = False
        self.retry.set_sensitive(True)
        self.origin = url
        if not url:
            self.loaded = None
            self.status.set_text(self.text('Service unavailable. Check whether another program uses port 8766, then retry.', 'Dienst nicht erreichbar. Prüfe, ob ein anderes Programm Port 8766 belegt, und versuche es erneut.'))
            self.stack.set_visible_child_name('status')
        elif self.loaded != url:
            self.loaded = url
            self.view.load_uri(url)
        return False

    def check(self):
        if self.working or not self.origin: return True
        self.working = True
        def work():
            try:
                self.service(['status']); alive = True
            except (OSError, subprocess.SubprocessError):
                alive = False
            GLib.idle_add(self.checked, alive)
        threading.Thread(target=work, daemon=True).start()
        return True

    def checked(self, alive):
        self.working = False
        if not alive:
            self.origin = self.loaded = None
            self.view.stop_loading()
            self.status.set_text(self.text('Local service stopped. Start it again to reconnect.', 'Lokaler Dienst gestoppt. Starte ihn erneut, um dich zu verbinden.'))
            self.retry.set_sensitive(True)
            self.stack.set_visible_child_name('status')
        return False

    def policy(self, _, decision, kind):
        if kind not in (WebKit.PolicyDecisionType.NAVIGATION_ACTION, WebKit.PolicyDecisionType.NEW_WINDOW_ACTION):
            return False
        action = decision.get_navigation_action()
        uri = action.get_request().get_uri()
        if kind == WebKit.PolicyDecisionType.NAVIGATION_ACTION and local_url(uri, self.origin):
            decision.use()
        else:
            if action.is_user_gesture() and not action.is_redirect() and local_url(self.view.get_uri(), self.origin) and external_url(uri):
                Gio.AppInfo.launch_default_for_uri(uri, None)
            decision.ignore()
        return True

    def message(self, _, value):
        if not local_url(self.view.get_uri(), self.origin) or not value.is_string(): return
        raw = value.to_string()
        if len(raw.encode()) > 140000: return
        identifier = ''
        try:
            message = json.loads(raw)
            identifier = message.get('id', '')
            if not isinstance(identifier, str) or not re.fullmatch('[0-9]{1,12}', identifier): return
            action, text = message.get('action'), message.get('value')
            if not isinstance(text, str) or len(text.encode()) > 131072: raise ValueError('Invalid text')
            if action == 'copy': self.view.get_clipboard().set(text)
            elif action == 'language' and text in ('de', 'en'):
                self.preferences.parent.mkdir(parents=True, exist_ok=True)
                temporary = self.preferences.with_suffix('.tmp')
                fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, 'w') as target: json.dump({'language': text}, target)
                os.replace(temporary, self.preferences)
                self.language = text
            else: raise ValueError('Unknown desktop action')
            ok = True
        except (OSError, ValueError, TypeError, AttributeError):
            ok = False
        if not re.fullmatch('[0-9]{1,12}', identifier): return
        # ID is digits only, never interpolate user text or connection credentials.
        self.view.evaluate_javascript(f"window.geistDesktopReply('{identifier}', {'true' if ok else 'false'});", -1, None, None, None, None, None)


if __name__ == '__main__':
    raise SystemExit(Desktop().run())
