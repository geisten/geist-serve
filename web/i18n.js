'use strict';
// Native hosts provide the OS locale separately from the saved user preference.
// Browser-only sessions use the browser locale. Unknown languages fall back to English.
function resolveLanguage(preference, systemLanguage) {
  if (preference === 'de' || preference === 'en') return preference;
  return /^de(?:[-_.@]|$)/i.test(systemLanguage || '') ? 'de' : 'en';
}
function languagePreference(value) { return ['de', 'en'].includes(value) ? value : 'system'; }
const systemLanguage = window.geistSystemLanguage || navigator.language || 'en';
let interfacePreference = languagePreference(window.geistLanguagePreference ?? window.geistLanguage ?? (() => {
  try { return localStorage.getItem('geist-language'); } catch { return null; }
})());
let interfaceLanguage = resolveLanguage(interfacePreference, systemLanguage);
const formatNumber = (value, digits = 0) => new Intl.NumberFormat(interfaceLanguage, {
  minimumFractionDigits: digits, maximumFractionDigits: digits
}).format(value);
const german = {
  'Model measurements': 'Modellmesswerte', 'Measurements': 'Messwerte', 'Close': 'Schließen', 'Historical': 'Historisch', 'Observed': 'Beobachtet', 'Known values': 'Bekannte Werte', 'Collection enabled': 'Messung aktiv', 'Collection disabled': 'Messung deaktiviert', 'Process RSS': 'Prozess-RSS', 'Process RSS · after reply': 'Prozess-RSS · nach Antwort', 'RSS sampled peak · 2 s': 'RSS-Stichprobenmaximum · 2 s', 'Current memory': 'Aktueller Speicher', 'File': 'Datei',
  'Unknown': 'Unbekannt', 'Modified build': 'Geänderter Build',
  "Preparing answer…": "Antwort wird vorbereitet …",
  "First answer": "Erste sichtbare Antwort",
  "Tokens and time include answer preparation.": "Token und Zeit enthalten die Antwortvorbereitung.",
  "No answer was produced. Try again with a shorter question.": "Es wurde keine Antwort erzeugt. Versuche es mit einer kürzeren Frage erneut.",
  "The model’s context limit was reached. Start a new chat or ask a shorter question.": "Die Kontextgrenze des Modells ist erreicht. Starte einen neuen Chat oder stelle eine kürzere Frage.",
  "Stopped before an answer was produced.": "Gestoppt, bevor eine Antwort erzeugt wurde.",
  "Input processing timed out after 10 minutes. Shorten the conversation or select GPU.": "Die Eingabeverarbeitung hat nach 10 Minuten das Zeitlimit erreicht. Kürze das Gespräch oder wähle GPU.",
  "The model stopped responding while generating. Try again or select GPU.": "Das Modell reagiert während der Ausgabe nicht mehr. Versuche es erneut oder wähle GPU.",
  "Could not contact the model service. Reload the model.": "Der Modelldienst ist nicht erreichbar. Lade das Modell erneut.",
  "Model execution failed while processing the input. Try a shorter conversation or select GPU.": "Bei der Eingabeverarbeitung ist ein Modellfehler aufgetreten. Kürze das Gespräch oder wähle GPU.",
  "Model execution failed while generating. Reload the model or select another processor.": "Bei der Ausgabe ist ein Modellfehler aufgetreten. Lade das Modell erneut oder wähle einen anderen Prozessor.",
  "The one-hour request limit was reached. Shorten the conversation or select GPU.": "Das Zeitlimit von einer Stunde ist erreicht. Kürze das Gespräch oder wähle GPU.",
  "Generation cancelled.": "Ausgabe abgebrochen.",
  "no_answer": "Keine Antwort",

  'Default': 'Standard', 'Ternary': 'Ternär',
  'Unsupported format': 'Format nicht unterstützt', 'Unsupported platform': 'Plattform nicht unterstützt',
  'Not enough disk space': 'Zu wenig Speicherplatz', 'Not enough RAM': 'Zu wenig RAM',
  'Below recommended RAM': 'Unter RAM-Empfehlung', 'Available RAM is tight': 'Verfügbarer RAM knapp',
  'Slow on available processors': 'Langsam auf verfügbaren Prozessoren',

  'Formula': 'Formel',
  'Formula shown as source': 'Formel als Quelltext angezeigt',
  'Performance could not be saved. Values remain available until quitting.': 'Messwerte konnten nicht gespeichert werden. Sie bleiben bis zum Beenden verfügbar.',
  'Switching processor…': 'Prozessor wird gewechselt…',
  'Not measured yet': 'Noch nicht gemessen',
  'Last completed reply per processor': 'Letzte abgeschlossene Antwort je Prozessor',
  'Model RAM · after reply': 'Modell-RAM · nach Antwort',
  'Measured': 'Gemessen',
  'Last successful measurements, saved locally without chat contents. Different prompts and contexts are not a controlled benchmark. First text and total time are measured by the service; RAM is a process snapshot after the reply, not peak or GPU memory.': 'Letzte erfolgreiche Messwerte, lokal ohne Chat-Inhalte gespeichert. Unterschiedliche Eingaben und Kontexte sind kein kontrollierter Benchmark. Erster Text und Gesamtzeit werden vom Dienst gemessen; RAM ist eine Prozess-Momentaufnahme nach der Antwort, weder Höchstwert noch GPU-Speicher.',

  'Text chat': 'Textchat',
  'Fits this computer': 'Für diesen Rechner geeignet',
  'Unavailable on this computer': 'Auf diesem Rechner nicht ausführbar',
  'Speech recognition': 'Spracherkennung', 'Image understanding': 'Bildverständnis',
  'Execution': 'Ausführung', 'Storage': 'Speicher',
  'CPU · bundled engine': 'CPU · mitgelieferte Engine',
  'GPU selection is not available in this app version.': 'GPU-Auswahl ist in dieser App-Version noch nicht verfügbar.',
  'Limited on this computer': 'Auf diesem Rechner eingeschränkt',
  'Model ready': 'Modell bereit', 'No model loaded': 'Kein Modell geladen',
  'The running model will stop. Connected programs will need another model.': 'Das laufende Modell wird beendet. Verbundene Programme benötigen anschließend ein anderes Modell.',
  'Selecting a model downloads and starts its preview. Check answers before using them.': 'Die Auswahl lädt das Modell herunter und startet seine Vorschau. Prüfe Antworten vor der Verwendung.',
  'Model performance details': 'Details zur Modellleistung',
  'Models & quick test': 'Modelle & Kurztest',
  'Model manager': 'Modellverwaltung',
  'Models for this computer': 'Modelle für diesen Rechner',
  'Set up a model to check its response and speed here.': 'Richte ein Modell ein, um hier die Antwort und Geschwindigkeit zu prüfen.',
  'Go to model setup': 'Zur Modelleinrichtung',
  'Suggested': 'Vorgeschlagen', 'Active': 'Aktiv', 'Selected': 'Ausgewählt',

  'This test stays in this window. Clear test, reloading or quitting clears it. Check answers before using them.': 'Dieser Test bleibt in diesem Fenster. Test leeren, Neuladen oder Beenden löscht ihn. Prüfe Antworten vor der Verwendung.',
  "Models": "Modelle",
  "Your model. Ready for your tools.": "Dein Modell. Für deine Programme.",
  "One local model for your programs.": "Ein lokales Modell für deine Programme.",
  "Ready for your programs.": "Für deine Programme bereit.",
  "Model in use by a program.": "Ein Programm verwendet das Modell.",
  "Connect a program": "Programm verbinden",
  "Quick test": "Kurz testen",
  "Change model": "Modell wechseln",
  "Settings": "Einstellungen",
  "Clear test": "Test leeren",
  "Load a model to try it.": "Lade ein Modell für den Kurztest.",
  "Try your model.": "Probiere dein Modell aus.",
  "Send a short message to check its response and speed.": "Prüfe mit einer kurzen Nachricht die Antwort und Geschwindigkeit.",
  "Connect your program.": "Verbinde dein Programm.",
  "Downloaded": "Heruntergeladen",
  "Not downloaded": "Nicht heruntergeladen",

  "Model RAM · now": "Modell-RAM · aktuell",
  "Model CPU · now": "Modell-CPU · aktuell",
  "System RAM": "System-RAM",
  "Available RAM": "Verfügbarer RAM",
  "Last completed reply": "Letzte abgeschlossene Antwort",
  "Generation": "Generierung",
  "Output tokens": "Ausgabetoken",
  "First text": "Erster Text",
  "Total time": "Gesamtzeit",
  "logical CPUs": "logische CPUs",
  "Not available": "Nicht verfügbar",
  "Last reply": "Letzte Antwort",
  "Model process RAM": "RAM des Modellprozesses",
  "tokens": "Token",
  "tok/s": "Token/s",
  "Measuring…": "Messung läuft…",
  "OS snapshots, refreshed about every two seconds. Model RAM is the resident memory of geistd, including shared pages; it excludes this window. CPU: 100% means all logical CPUs. Available RAM is an OS estimate, not an allocation guarantee. GPU and power use are not measured.": "OS-Momentaufnahmen, etwa alle zwei Sekunden aktualisiert. Modell-RAM ist der residente Speicher von geistd, einschließlich geteilter Speicherseiten und ohne dieses Fenster. CPU: 100 % bedeutet alle logischen CPUs. Verfügbarer RAM ist eine OS-Schätzung, keine Speicherzusage. GPU und Energieverbrauch werden nicht gemessen.",
  "Speed uses the model's generated tokens and generation time, including streaming. First text and total time also include input processing and the local connection. Values apply to this reply, not to answer quality.": "Die Geschwindigkeit verwendet die erzeugten Token und die Ausgabezeit des Modells einschließlich Streaming. Erster Text und Gesamtzeit enthalten auch Eingabeverarbeitung und lokale Verbindung. Die Werte gelten für diese Antwort und sagen nichts über deren Qualität aus.",

  'Code': 'Code', 'Copy code': 'Code kopieren', 'Copy link': 'Link kopieren', 'Table': 'Tabelle', 'Image': 'Bild',
  'Checked': 'Abgehakt', 'Unchecked': 'Nicht abgehakt',
  'Answers support Markdown. Copy keeps the original formatting. Web addresses can be copied; external images are not loaded.': 'Antworten unterstützen Markdown. Kopieren behält die Formatierung bei. Webadressen lassen sich kopieren; externe Bilder werden nicht geladen.',

  'Your models. Your computer.': 'Deine Modelle. Dein Rechner.', 'Quality and speed': 'Qualität und Geschwindigkeit',
  'Skip to content': 'Zum Inhalt', 'Interface': 'Oberfläche', 'Interface language': 'Sprache der Oberfläche', 'Setup': 'Einrichtung',
  '1. Models': '1. Modelle', '2. Test': '2. Testen', '3. Connect': '3. Verbinden',
  'Make room for your ideas.': 'Platz für deine Ideen.',
  'Choose a task and a model. Your text is processed on the computer running Geist.': 'Wähle eine Aufgabe und ein Modell. Dein Text wird auf dem Rechner verarbeitet, auf dem Geist läuft.',
  'Your task': 'Deine Aufgabe', 'Answer language': 'Antwortsprache', 'Loading tasks…': 'Aufgaben werden geladen…',
  'Allow experimental models. I will review their answers.': 'Experimentelle Modelle zulassen. Ich werde ihre Antworten prüfen.',
  'Open Home Assistant integration ↗': 'Home-Assistant-Integration öffnen ↗',
  'THIS COMPUTER': 'DIESER RECHNER', 'Checking this computer…': 'Rechner wird geprüft…', 'Find your fit.': 'Finde das passende Modell.',
  '6 models': '6 Modelle', 'Hardware fit and answer quality are separate. Experimental models need your explicit choice.': 'Hardware-Eignung und Antwortqualität sind getrennte Kriterien. Experimentelle Modelle musst du ausdrücklich freigeben.',
  'Available models': 'Verfügbare Modelle', 'Show more models': 'Weitere Modelle anzeigen', 'Show fewer models': 'Weniger Modelle anzeigen',
  'How recommendations work': 'So entstehen Empfehlungen',
  'Memory figures are planning estimates. A recommendation needs evidence for the model, task, language and device. Speed alone does not establish answer quality.': 'Speicherangaben sind Planungsschätzungen. Eine Empfehlung braucht Belege für Modell, Aufgabe, Sprache und Gerät. Geschwindigkeit allein belegt keine Antwortqualität.',
  'Downloads come from Hugging Face after you choose a model. Each completed download is verified with SHA-256. No account is needed.': 'Modelle werden nach deiner Auswahl von Hugging Face geladen und mit SHA-256 geprüft. Du brauchst kein Konto.',
  'Try it here.': 'Probiere es aus.', 'Choose a model': 'Modell auswählen', 'Choose a model to begin.': 'Wähle zuerst ein Modell.',
  'Download a suggested model to begin.': 'Wähle und lade zuerst ein Modell.', 'Your input': 'Deine Eingabe', 'Write here…': 'Hier schreiben…',
  'Use an example': 'Beispiel verwenden', 'Run locally': 'Lokal ausführen', 'Stop': 'Stoppen', 'Quick speed test': 'Kurzer Geschwindigkeitstest',
  'Each request starts fresh. Review the result before using it. ⌘ / Ctrl + Enter to send.': 'Jede Anfrage beginnt neu. Prüfe das Ergebnis vor der Verwendung. Senden mit ⌘ / Strg + Enter.',
  'Your result': 'Dein Ergebnis', 'Copy': 'Kopieren', 'Copied': 'Kopiert', 'Generation speed': 'Ausgabegeschwindigkeit', 'First text': 'Erster Text', 'Total time': 'Gesamtzeit',
  'Measurements appear after a run.': 'Messwerte erscheinen nach einem Durchlauf.', 'Bring your own tools.': 'Nutze deine Programme.',
  'Your editor and this window use the same model. Closing the window keeps the service running.': 'Dein Editor und dieses Fenster verwenden dasselbe Modell. Beim Schließen läuft der Dienst weiter.',
  'Local endpoint': 'Lokaler Endpunkt', 'Model': 'Modell', 'Use with': 'Verwenden mit', 'OpenCode · text chat': 'OpenCode · Textchat',
  'Copy configuration': 'Konfiguration kopieren', 'Test local connection': 'Lokale Verbindung testen',
  'Text chat is available after loading a model. Agent tools are not supported. Context: 4096 tokens. Copied configurations contain your private local key: keep them out of repositories.': 'Textchat ist nach dem Laden verfügbar. Agentenwerkzeuge werden nicht unterstützt. Kontext: 4096 Token. Kopierte Konfigurationen enthalten deinen privaten lokalen Schlüssel und gehören nicht in Repositories.',
  'Models stay on this computer. Prompts and results are not saved as chat history.': 'Modelle bleiben auf diesem Rechner. Eingaben und Ergebnisse werden nicht als Chatverlauf gespeichert.',
  'Unload model': 'Modell entladen', 'Stop model service': 'Modelldienst stoppen', 'Downloading': 'Wird heruntergeladen', 'Download progress': 'Download-Fortschritt', 'Cancel': 'Abbrechen',
  'Unavailable': 'Nicht verfügbar', 'Experimental': 'Experimentell', 'Recommended': 'Empfohlen', 'Conditional': 'Bedingt geeignet',
  'Running here': 'Läuft hier', 'Use this model': 'Modell verwenden', 'Resume download': 'Download fortsetzen', 'Remove download': 'Download löschen',
  'Task quality: unverified for this task, language and device.': 'Aufgabenqualität: Für diese Aufgabe, Sprache und dieses Gerät noch nicht bestätigt.',
  'Hardware information unavailable': 'Hardware-Informationen nicht verfügbar', 'Disk space could not be read': 'Freier Speicher konnte nicht ermittelt werden',
  'Verifying model': 'Modell wird geprüft', 'Downloading model': 'Modell wird heruntergeladen', 'Loading model': 'Modell wird geladen', 'Ready on this device': 'Auf diesem Gerät bereit',
  'Checking the complete file before it can run.': 'Die vollständige Datei wird vor dem Start geprüft.',
  'Short local test running. Results apply to this model and this workload.': 'Kurzer lokaler Test läuft. Die Messwerte gelten für dieses Modell und diese Aufgabe.',
  'Running on your device…': 'Läuft auf deinem Gerät…', 'Waiting for the first text…': 'Warte auf den ersten Text…', 'Generating locally…': 'Text wird lokal erzeugt…',
  'The model returned an error.': 'Das Modell hat einen Fehler gemeldet.', 'Output exceeded the display memory limit.': 'Die Ausgabe überschreitet das Speicherlimit der Anzeige.',
  'This browser does not support streamed responses.': 'Diese Oberfläche unterstützt keine gestreamten Antworten.', 'The response exceeded the stream buffer limit.': 'Die Antwort überschreitet das Pufferlimit.',
  'The connection ended before the model completed its response.': 'Die Verbindung endete vor Abschluss der Antwort.',
  'Output limit reached. The result may be incomplete; try a shorter input.': 'Ausgabelimit erreicht. Das Ergebnis kann unvollständig sein; versuche eine kürzere Eingabe.',
  'Complete. Your result stays on this device.': 'Fertig. Dein Ergebnis bleibt auf diesem Gerät.', 'The model completed without producing text. Try a different prompt.': 'Das Modell lieferte keinen Text. Versuche eine andere Eingabe.',
  'Stopped. Partial output is kept here.': 'Gestoppt. Die bisherige Ausgabe bleibt hier.', 'Run incomplete. No final generation speed is reported.': 'Durchlauf unvollständig. Es wird keine abschließende Geschwindigkeit angegeben.',
  'Cancelling…': 'Wird abgebrochen…', 'Stopping': 'Wird gestoppt', 'Geist is stopping. Reopen the app to start it again.': 'Geist wird gestoppt. Öffne die App erneut, um den Dienst zu starten.',
  'Stop the shared service? Terminal and editor connections will stop too. Downloaded models are kept.': 'Gemeinsamen Dienst stoppen? Auch Terminal und Editoren werden getrennt. Heruntergeladene Modelle bleiben erhalten.',
  'Copy is unavailable here. Select the result and copy it manually.': 'Kopieren ist hier nicht verfügbar. Markiere das Ergebnis und kopiere es manuell.',
  'Load a model first.': 'Lade zuerst ein Modell.', 'Copied. The configuration contains your private local key.': 'Kopiert. Die Konfiguration enthält deinen privaten lokalen Schlüssel.',
  'Asking the loaded model through the editor endpoint…': 'Das geladene Modell wird über den Editor-Endpunkt angesprochen…',
  'The model completed without text. Try another model.': 'Das Modell lieferte keinen Text. Versuche ein anderes Modell.',
  'Open Geist using the private link from the app or Pi launcher. The link contains your private local API key.': 'Öffne Geist über den privaten Link aus der App oder dem Pi-Starter. Der Link enthält deinen privaten lokalen API-Schlüssel.',
  'Service unavailable. Reopen Geist to reconnect.': 'Dienst nicht erreichbar. Öffne Geist erneut, um dich zu verbinden.',
  'Stop the current task first.': 'Stoppe zuerst die laufende Aufgabe.', 'Another task is active.': 'Eine andere Aufgabe läuft bereits.',
  'Unload this model before removing it.': 'Entlade das Modell, bevor du es löschst.', 'Cannot remove this download safely.': 'Dieser Download konnte nicht sicher gelöscht werden.',
  'Download this model first.': 'Lade dieses Modell zuerst herunter.', 'Write your input here…': 'Schreibe hier deine Eingabe…',
  'seconds': 'Sekunden'
};
Object.assign(german, {
  'Your space to think.': 'Raum für deine Ideen.',
  'Write, summarize, explore. Locally.': 'Schreiben, kürzen, weiterdenken. Lokal.',
  'One download. Then ready offline.': 'Einmal herunterladen. Danach offline bereit.',
  'Pause download': 'Download pausieren',
  'Measuring speed…': 'Geschwindigkeit wird ermittelt…',
  'Waiting for data…': 'Warte auf Daten…',
  'Less than a minute left': 'Noch weniger als eine Minute'
});
const germanPatterns = [
  [/^Downloading · (\d+)%$/, (_, n) => `Wird geladen · ${n}%`],
  [/^Paused · (\d+)%$/, (_, n) => `Pausiert · ${n}%`],
  [/^About (\d+) min left$/, (_, n) => `Noch etwa ${n} Min.`],
  [/^([\d.,\s]+ [MG]B) of ([\d.,\s]+ [MG]B)$/, (_, a, b) => `${a} von ${b}`],
  [/^(.+) download · (.+) GiB RAM guidance$/, (_, a, b) => `${a} Download · ${b} GiB RAM empfohlen`],
  [/^Download · (.+)$/, (_, a) => `Herunterladen · ${a}`],
  [/^(.+) disk space available$/, (_, a) => `${a} Speicherplatz verfügbar`],
  [/^(.+) RAM · (.+) compute cores · (.+)$/, (_, a, b, c) => `${a} RAM · ${b} Rechenkerne · ${c}`],
  [/^Show (\d+) more models$/, (_, a) => `${a} weitere Modelle anzeigen`],
  [/^(Verifying|Downloading) (.+)$/, (_, a, b) => `${b}: ${a === 'Verifying' ? 'wird geprüft' : 'wird heruntergeladen'}`],
  [/^(.+) of (.+) · partial downloads can be resumed$/, (_, a, b) => `${a} von ${b} · Teil-Downloads lassen sich fortsetzen`],
  [/^Remove (.+) from this computer\? You can download it again later\.$/, (_, a) => `${a} von diesem Rechner löschen? Du kannst es später erneut herunterladen.`],
  [/^Measured here: (.+) tokens\/s · (.+) tokens · this session$/, (_, a, b) => `Hier gemessen: ${a} Token/s · ${b} Token · diese Sitzung`],
  [/^Connected\. The shared model returned (.+) tokens\. Now test the configuration in your chosen client\.$/, (_, a) => `Verbunden. Das gemeinsame Modell hat ${a} Token geliefert. Teste nun die Konfiguration in deinem Programm.`],
  [/^Request failed \((\d+)\)\.$/, (_, a) => `Anfrage fehlgeschlagen (${a}).`],
  [/^Resources: (.+)$/, (_, a) => `Ressourcen: ${t(a)}`],
  [/^(\d+) generated tokens\..+$/, (_, a) => `${a} erzeugte Token. Die Geschwindigkeit verwendet die Ausgabezeit von geistd einschließlich Streaming. Erster Text und Gesamtzeit enthalten Verbindung und Eingabeverarbeitung.`]
];
function t(text) {
  if (interfaceLanguage !== 'de' || typeof text !== 'string') return text;
  if (german[text]) return german[text];
  for (const [pattern, replace] of germanPatterns) if (pattern.test(text)) return text.replace(pattern, replace);
  return text;
}
const staticTexts = [];
const staticAttributes = [];
const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
while (walker.nextNode()) {
  const node = walker.currentNode;
  if (node.textContent.trim() && !['SCRIPT', 'STYLE'].includes(node.parentElement.tagName)) staticTexts.push([node, node.textContent]);
}
for (const node of document.querySelectorAll('[aria-label], [placeholder], [title]')) for (const attr of ['aria-label', 'placeholder', 'title']) if (node.hasAttribute(attr)) staticAttributes.push([node, attr, node.getAttribute(attr)]);
function translateStatic() {
  document.documentElement.lang = interfaceLanguage;
  for (const [node, original] of staticTexts) if (node.isConnected) node.textContent = original.replace(original.trim(), t(original.trim()));
  for (const [node, attr, original] of staticAttributes) node.setAttribute(attr, t(original));
}
async function desktopMessage(action, value) {
  if (window.geistDesktop === 'mac') return window.webkit.messageHandlers.desktop.postMessage({action, value});
  if (window.geistDesktop === 'linux') {
    // Linux replies by resolving only this bounded, opaque request identifier.
    return new Promise((resolve, reject) => {
      const id = String(++desktopRequest);
      const timeout = setTimeout(() => { desktopReplies.delete(id); reject(new Error('Desktop request timed out.')); }, 5000);
      desktopReplies.set(id, {resolve, reject, timeout});
      window.webkit.messageHandlers.desktop.postMessage(JSON.stringify({id, action, value}));
    });
  }
}
let desktopRequest = 0;
const desktopReplies = new Map();
window.geistDesktopReply = (id, ok) => {
  const pending = desktopReplies.get(id); if (!pending) return;
  clearTimeout(pending.timeout); desktopReplies.delete(id);
  if (ok) pending.resolve(); else pending.reject(new Error('Desktop request denied.'));
};
async function copyText(value) {
  if (window.geistDesktop) await desktopMessage('copy', value);
  else await navigator.clipboard.writeText(value);
}
Object.assign(german, {
  'Evidence is specific to the model, language and device.': 'Belege gelten jeweils für Modell, Sprache und Gerät.',
  'Tests cover simple chats only, not arbitrary questions.': 'Die Tests decken einfache Chats ab, nicht beliebige Fragen.',
  'The language applies to this window but could not be saved.': 'Die Sprache gilt für dieses Fenster, konnte aber nicht gespeichert werden.',
  'Rewrite a message': 'Nachricht umformulieren', 'Summarize a note': 'Notiz zusammenfassen', 'Explore an idea': 'Ideen finden', 'Try your own request': 'Freie Anfrage testen', 'Control Home Assistant': 'Home Assistant steuern',
  'Turn a rough message into a clear, friendly draft.': 'Formuliere eine Nachricht klar und freundlich um.',
  'Extract the main point from a short note.': 'Fasse den wichtigsten Punkt einer kurzen Notiz zusammen.',
  'Get a short starting point to develop yourself.': 'Erhalte einen kurzen Ausgangspunkt zum Weiterentwickeln.',
  'An open experiment with the model you choose.': 'Ein freier Versuch mit dem gewählten Modell.',
  'Use the existing Home Assistant integration. Device permissions and execution stay in Home Assistant. Product validation is still in progress.': 'Nutze die bestehende Home-Assistant-Integration. Geräteberechtigungen und Ausführung bleiben in Home Assistant. Die Produktprüfung läuft noch.',
  'I cannot attend our meeting tomorrow. Could we move it to Friday morning?': 'Ich kann morgen nicht an unserem Treffen teilnehmen. Können wir es auf Freitagvormittag verschieben?',
  'The garden workshop takes place on Saturday at 10 am. Bring gloves and a small container. Seeds and tools will be provided. The event ends at noon.': 'Der Gartenworkshop findet am Samstag um 10 Uhr statt. Bring Handschuhe und einen kleinen Behälter mit. Saatgut und Werkzeuge werden gestellt. Die Veranstaltung endet um 12 Uhr.',
  'Useful things to do with a Raspberry Pi at home.': 'Nützliche Einsatzmöglichkeiten für einen Raspberry Pi zu Hause.',
  'Explain how a seed grows into a plant in three sentences.': 'Erkläre in drei Sätzen, wie aus einem Samen eine Pflanze wächst.',
  'Performance on this device is not measured yet.': 'Die Leistung auf diesem Gerät wurde noch nicht gemessen.',
  'Unknown on this device; measure after download.': 'Für dieses Gerät unbekannt; nach dem Download messen.',
  'Pi 5 reference: 17.8 tokens/s; your speed may differ.': 'Pi-5-Referenz: 17,8 Token/s; deine Geschwindigkeit kann abweichen.',
  'Apple Silicon profile; run a local test for actual speed.': 'Apple-Silicon-Profil; tatsächliche Geschwindigkeit lokal testen.',
  'This CPU instruction set or platform is not supported by the bundled engine.': 'Die enthaltene Engine unterstützt diesen CPU-Befehlssatz oder diese Plattform nicht.',
  'Not enough disk space for the download plus 256 MiB reserve.': 'Nicht genug Speicherplatz für den Download und 256 MiB Reserve.',
  'RAM is smaller than the model file, before context and OS memory.': 'Der RAM ist kleiner als die Modelldatei, noch ohne Kontext und Betriebssystem.',
  'Below the RAM recommendation; swapping or allocation failures are possible.': 'Unter der RAM-Empfehlung; Auslagerung oder Speicherfehler sind möglich.',
  'Available RAM is tight now. Close other apps before loading this model.': 'Der verfügbare RAM ist knapp. Schließe andere Programme vor dem Laden.',
  'Fits the Pi 5 memory profile and has a published speed reference.': 'Passt zum Pi-5-Speicherprofil und hat eine veröffentlichte Geschwindigkeitsreferenz.',
  'Fits the Apple Silicon hardware and RAM profile; speed is an estimate.': 'Passt zum Hardware- und RAM-Profil von Apple Silicon; Geschwindigkeit geschätzt.',
  "Measured below the app's interactive target of 8 tokens/s. Still usable for patient tasks.": 'Gemessene Geschwindigkeit unter dem Zielwert von 8 Token/s. Für Aufgaben mit Wartezeit weiterhin nutzbar.',
  "Memory fits and measured speed meets the app's interactive target of 8 tokens/s.": 'Speicher passt, gemessene Geschwindigkeit erreicht den Zielwert von 8 Token/s.',
  'Measured on this device in this app session; workload and temperature affect speed.': 'In dieser Sitzung auf diesem Gerät gemessen; Aufgabe und Temperatur beeinflussen die Geschwindigkeit.',
  'Paste the copied curl command into your terminal to try the loaded model. Ubuntu also installs geist test and geist chat. On Mac, the CLI is bundled at /Applications/Geist.app/Contents/MacOS/geist-cli.': 'Füge den kopierten curl-Befehl im Terminal ein. Unter Ubuntu gibt es auch geist test und geist chat. Auf dem Mac liegt die CLI unter /Applications/Geist.app/Contents/MacOS/geist-cli.',
  'In Continue, open your local config.yaml and add the model from this configuration. JSON is valid YAML. Select Geist and use Chat mode. Preserve your existing configuration.': 'Öffne in Continue deine lokale config.yaml und ergänze das Modell aus dieser Konfiguration. JSON ist gültiges YAML. Wähle Geist im Chat-Modus. Behalte deine bestehenden Einstellungen.',
  'Save as opencode.json in a private test folder. Run opencode there and choose geist-chat. This profile disables tools; it does not enable coding-agent workflows.': 'Speichere dies als opencode.json in einem privaten Testordner. Starte dort opencode und wähle geist-chat. Das Profil deaktiviert Werkzeuge und unterstützt keine Coding-Agenten.'
});

Object.assign(german, {
  'Geist home': 'Geist Startseite', 'Navigation': 'Navigation', 'Try it': 'Ausprobieren',
  'Connect': 'Verbinden', 'Customize': 'Anpassen', 'Optional tasks': 'Optionale Aufgaben',
  'A little more room for your ideas.': 'Mehr Platz für deine Ideen.',
  'Write, summarize, explore. Right on your computer.': 'Schreiben, zusammenfassen, weiterdenken. Direkt auf deinem Rechner.',
  'Preview: answers can be wrong. By starting, you agree to try this model and review its answers.': 'Vorschau: Antworten können falsch sein. Mit dem Start probierst du dieses Modell bewusst aus und prüfst seine Antworten.',
  'Set up and start': 'Einrichten und starten', 'No account. No cloud processing.': 'Ohne Konto. Ohne Verarbeitung in der Cloud.',
  'Getting ready…': 'Wird vorbereitet…', 'What would you like to try?': 'Was möchtest du ausprobieren?',
  'Rewrite': 'Umformulieren', 'Summarize': 'Zusammenfassen', 'Ideas': 'Ideen',
  'Each request starts fresh. ⌘ / Ctrl + Enter to send.': 'Jede Anfrage beginnt neu. Senden mit ⌘ / Strg + Enter.',
  'Make yourself at home.': 'So passt es zu dir.', 'This computer': 'Dieser Rechner', 'Model measurements': 'Modellmesswerte', 'Measurements': 'Messwerte',
  'Service': 'Dienst', 'Selected for setup': 'Für die Einrichtung gewählt', 'Available': 'Verfügbar', 'Details': 'Details',
  'Choose': 'Auswählen', 'Already on this computer': 'Bereits auf diesem Rechner', 'Download': 'Download',
  'No suitable model available right now.': 'Zurzeit ist kein geeignetes Modell verfügbar.',
  'Try preview': 'Vorschau ausprobieren', 'Start model': 'Modell starten', 'compute cores': 'Rechenkerne',
  'Starting the local service…': 'Lokaler Dienst wird gestartet…',
  'Cannot check available memory or disk space. Retry the platform check.': 'Verfügbarer Arbeits- oder Festplattenspeicher konnte nicht geprüft werden. Die Prüfung wird wiederholt.',
  "Not enough total RAM for this model's planning budget.": 'Der Arbeitsspeicher reicht für das geplante Speicherbudget dieses Modells nicht aus.',
  'Your model choice is kept. Select another model below.': 'Deine Modellauswahl bleibt erhalten. Hier kannst du ein anderes Modell wählen.',
  'Local performance is below the interactive setup target.': 'Die lokale Leistung liegt unter dem Zielwert für interaktive Nutzung.',
  'A smaller model fits the available resources better.': 'Ein kleineres Modell passt besser zu den verfügbaren Ressourcen.',
  'Platform default. Memory is estimated; answer quality is still unverified.': 'Standard für diese Plattform. Speicherbedarf geschätzt; Antwortqualität noch nicht bestätigt.',
  'The platform check changed. Review the setup suggestion and retry.': 'Die Plattformprüfung hat sich geändert. Prüfe den Vorschlag und starte erneut.',
  'Choose English or German.': 'Wähle Englisch oder Deutsch.', 'Cannot save language preference.': 'Die Sprache konnte nicht gespeichert werden.',
  'Preview consent must be explicit.': 'Die Vorschau muss ausdrücklich bestätigt werden.', 'Cannot save preview consent.': 'Die Vorschau-Bestätigung konnte nicht gespeichert werden.'
});
Object.assign(german, {
  "Chat": "Chat",
  "New chat": "Neuer Chat",
  "Conversation": "Gespräch",
  "You": "Du",
  "What would you like to explore?": "Was beschäftigt dich?",
  "Ask a question, improve a text or work through an idea.": "Stelle eine Frage, überarbeite einen Text oder entwickle eine Idee.",
  "Message Geist…": "Schreibe Geist…",
  "Send message": "Nachricht senden",
  "Stop response": "Antwort stoppen",
  "Tips": "Tipps",
  "What can I ask?": "Was kann ich fragen?",
  "Just describe what you need. No mode to choose.": "Beschreibe einfach, was du brauchst. Du musst keinen Modus wählen.",
  "Rewrite this email in a friendlier tone: …": "Formuliere diese E-Mail freundlicher: …",
  "Summarize this text in three points: …": "Fasse diesen Text in drei Punkten zusammen: …",
  "Suggest three ideas for …": "Schlage drei Ideen vor für …",
  "This conversation stays in this window. New chat, reloading or quitting clears it. Check answers before using them.": "Das Gespräch bleibt in diesem Fenster. Neuer Chat, Neuladen oder Beenden löscht es. Prüfe Antworten vor der Verwendung.",
  "Enter to send · Shift + Enter for a new line": "Enter zum Senden · Shift + Enter für einen Zeilenumbruch",
  "↓ Latest message": "↓ Neueste Nachricht",
  "Your message is too long. Shorten it before sending; your draft has been kept.": "Deine Nachricht ist zu lang. Kürze sie vor dem Senden; der Entwurf bleibt erhalten.",
  "This test is full. Use Clear chat to start again. The existing text has been kept.": "Dieser Test ist voll. Nutze „Chat löschen“ für einen neuen Versuch. Der bisherige Text bleibt erhalten.",
  "Response limit reached. You can ask Geist to continue.": "Antwortlimit erreicht. Du kannst Geist bitten, fortzufahren.",
  "Continue response": "Antwort fortsetzen",
  "Continue from where you stopped.": "Fahre dort fort, wo du aufgehört hast.",
  "Continue from the latest reply, or ask a new question.": "Setze die neueste Antwort fort oder stelle eine neue Frage.",
  "Response complete.": "Antwort vollständig.",
  "This test does not fit the model’s context. Shorten your draft or use Clear chat to start again. No earlier messages have been removed.": "Dieser Test passt nicht mehr in den Kontext des Modells. Kürze deinen Entwurf oder beginne mit „Chat löschen“ erneut. Frühere Nachrichten wurden nicht entfernt.",
  "Clear this conversation and draft? They are not saved.": "Gespräch und Entwurf löschen? Sie werden nicht gespeichert.",
  "Test cleared.": "Test geleert.",
  "The loaded model changed. Check the model and send again.": "Das geladene Modell hat sich geändert. Prüfe es und sende erneut."
});
Object.assign(german, {
  'System language': 'Systemsprache', 'Navigation': 'Navigation', 'Geist home': 'Geist Startseite',
  'Service': 'Dienst', 'Download size': 'Dateigröße', 'Test a message.': 'Teste eine Nachricht.',
  'Select to set up': 'Zum Einrichten auswählen', 'Select to load': 'Zum Laden auswählen',
  'Select to resume': 'Zum Fortsetzen auswählen',
  'Preview: check answers before using them. Start to accept this preview.': 'Vorschau: Prüfe die Antworten. Mit dem Start akzeptierst du diese Vorschau.',
  'Local processing. No account.': 'Lokale Verarbeitung. Ohne Konto.',
  '↵ Send · ⇧↵ New line': '↵ Senden · ⇧↵ Neue Zeile'
});
Object.assign(german, {
  'Preview · Check answers.': 'Vorschau · Antworten prüfen.',
  'Selecting a model starts its preview. Check answers before using them.': 'Mit der Modellauswahl startest du die Vorschau. Prüfe die Antworten vor der Verwendung.',
  'Download and start': 'Herunterladen und starten',
  'Download model': 'Modell herunterladen',
  'Download complete.': 'Download abgeschlossen.',
  'Local model': 'Lokales Modell'
});
Object.assign(german, {
  "Auto": "Auto",
  "CPU": "CPU",
  "GPU": "GPU",
  "Recommended": "Empfohlen",
  "Model catalog": "Modellkatalog",
  "Model catalog JSON file": "JSON-Datei für den Modellkatalog",
  "Import a newer JSON file from a source you trust. No automatic online updates.": "Neuere JSON-Datei aus einer vertrauenswürdigen Quelle importieren. Keine automatischen Online-Updates.",
  "GPU failed. Diagnostics could not be archived; the model remains stopped.": "GPU ausgefallen. Diagnose konnte nicht archiviert werden; das Modell bleibt gestoppt.",
  "The engine reported a different processor. Reload the model.": "Die Engine meldet einen anderen Prozessor. Lade das Modell erneut.",
  "Catalog updated.": "Katalog aktualisiert.",
  "The catalog must be at most 24 KiB.": "Der Katalog darf höchstens 24 KiB groß sein.",
  "Choose a valid JSON file.": "Wähle eine gültige JSON-Datei.",
  "Active processor": "Aktiver Prozessor",
  "Uses the recommended processor. Changing execution reloads the model without downloading it again.": "Verwendet den empfohlenen Prozessor. Ein Wechsel lädt das Modell neu in den Speicher, ohne erneuten Download.",
  "GPU is not supported by this model and packaged engine.": "GPU wird für dieses Modell mit der mitgelieferten Engine nicht unterstützt.",
  "GPU is suggested for this larger model. This is a hardware default, not a measured speed comparison.": "GPU wird für dieses größere Modell empfohlen. Die Empfehlung beruht auf der Hardware, nicht auf einem gemessenen Geschwindigkeitsvergleich.",
  "CPU is suggested for this small model. This is a hardware default, not a measured speed comparison.": "CPU wird für dieses kleine Modell empfohlen. Die Empfehlung beruht auf der Hardware, nicht auf einem gemessenen Geschwindigkeitsvergleich.",
  "Choose Auto, CPU or GPU.": "Wähle Auto, CPU oder GPU.",
  "Wait until the loaded model is idle before changing execution.": "Warte vor dem Wechsel, bis das geladene Modell nicht mehr beschäftigt ist.",
  "Checking model…": "Modell wird geprüft…",
  "Cannot finish verification. The model file changed.": "Prüfung nicht abgeschlossen. Die Modelldatei wurde verändert.",
  "Cannot save execution preference.": "Die Ausführungseinstellung konnte nicht gespeichert werden.",
  "Cannot change execution. Restoring CPU.": "Ausführung konnte nicht gewechselt werden. CPU wird wiederhergestellt.",
  "GPU stopped or failed to load. Restored CPU; diagnostics are kept in the app data folder.": "GPU beendet oder Start fehlgeschlagen. CPU wiederhergestellt; die Diagnose bleibt im App-Datenordner erhalten.",
  "The model is running, but its execution preference could not be saved.": "Das Modell läuft, aber die Ausführungseinstellung konnte nicht gespeichert werden.",
  "Invalid model catalog. Check schema, entries and unique IDs/files.": "Ungültiger Modellkatalog. Prüfe Schema, Einträge sowie eindeutige IDs und Dateinamen.",
  "Finish the current operation before importing a catalog.": "Beende den laufenden Vorgang vor dem Katalogimport.",
  "Import a catalog with a newer revision.": "Importiere einen Katalog mit einer neueren Revision.",
  "The running model must stay unchanged in this catalog.": "Das laufende Modell muss in diesem Katalog unverändert bleiben.",
  "Use a new filename when replacing an existing model hash.": "Verwende einen neuen Dateinamen, wenn du den Hash eines vorhandenen Modells ersetzt.",
  "Cannot save the catalog. The previous catalog is kept.": "Katalog konnte nicht gespeichert werden. Der bisherige Katalog bleibt erhalten.",
  "Saved catalog is invalid or older; using the bundled catalog.": "Gespeicherter Katalog ist ungültig oder älter; der mitgelieferte Katalog wird verwendet."
});
translateStatic();

Object.assign(german, {
  'Clear chat': 'Chat löschen',
  'Chat cleared.': 'Chat gelöscht.',
  'Copy response': 'Antwort kopieren',
  'Response copied.': 'Antwort kopiert.',
  'Below target': 'Unter Richtwert',
  'Interactive target': 'Richtwert für interaktive Nutzung',
  'Different prompts are not a controlled benchmark.': 'Unterschiedliche Eingaben sind kein kontrollierter Leistungsvergleich.',
  'This conversation stays in this window. Clear chat, reloading or quitting clears it. Check answers before using them.': 'Dieses Gespräch bleibt in diesem Fenster. Chat löschen, Neuladen oder Beenden löscht es. Prüfe Antworten vor der Nutzung.',
  'This model requires PQ2_0 and Hadamard support, unavailable in the bundled engine.': 'Dieses Modell benötigt PQ2_0- und Hadamard-Unterstützung. Die mitgelieferte Engine unterstützt das noch nicht.',
  'No known resource restriction. Speed has not been measured on this device.': 'Keine bekannte Ressourceneinschränkung. Die Geschwindigkeit wurde auf diesem Gerät noch nicht gemessen.',
  'Last replies were below 8 tokens/s on every available processor. Slower tasks remain possible.': 'Die letzten Antworten lagen auf allen verfügbaren Prozessoren unter 8 Token/s. Langsamere Aufgaben sind weiterhin möglich.'
});

Object.assign(german, {
  "now": "jetzt",
  "Profile": "Profil",
  "Local performance profile": "Lokales Leistungsprofil",
  "Measurement": "Messwert",
  "Typical speed": "Typische Geschwindigkeit",
  "Usual range": "Üblicher Bereich",
  "Sampled peak · 2 s": "Beobachteter Höchstwert · 2 s",
  "Observations": "Messungen",
  "About these measurements": "Über diese Messwerte",
  "Recent observations": "Letzte Messungen",
  "Measure comparison": "Vergleich messen",
  "Cancel comparison": "Vergleich abbrechen",
  "Local measurements": "Lokale Messungen",
  "Collect numeric performance": "Leistungswerte erfassen",
  "Keep history": "Verlauf behalten",
  "30 days": "30 Tage",
  "90 days": "90 Tage",
  "365 days": "365 Tage",
  "No prompts or replies. Stored only on this computer.": "Keine Eingaben oder Antworten. Nur auf diesem Rechner gespeichert.",
  "Export JSONL": "JSONL exportieren",
  "Delete history": "Verlauf löschen",
  "First observations": "Erste Messungen",
  "Typical": "Typisch",
  "Latest workload": "Letzte Vergleichsgruppe",
  "Input": "Eingabe",
  "Output": "Ausgabe",
  "Cache reused": "Cache wiederverwendet",
  "No cache reuse": "Ohne Cache-Wiederverwendung",
  "First reply after load": "Erste Antwort nach Laden",
  "Warm": "Warm",
  "Download overlap": "Während Download",
  "Controlled comparison": "Kontrollierter Vergleich",
  "Ordinary use": "Normale Nutzung",
  "First observations: fewer than 5 replies. No automatic processor changes.": "Erste Messungen: weniger als 5 Antworten. Kein automatischer Prozessorwechsel.",
  "observations retained": "Messungen gespeichert",
  "days": "Tage",
  "Up to 20 MiB / 8192 observations": "Bis 20 MiB / 8192 Messungen",
  "History could not be saved.": "Verlauf konnte nicht gespeichert werden.",
  "unsaved observations": "ungespeicherte Messungen",
  "invalid records skipped": "ungültige Einträge übersprungen",
  "completed": "Abgeschlossen",
  "interrupted": "Unterbrochen",
  "error": "Fehler",
  "cancelled": "Abgebrochen",
  "failed": "Fehlgeschlagen",
  "restore_failed": "Wiederherstellung fehlgeschlagen",
  "app": "App",
  "api": "Schnittstelle",
  "controlled_test": "Kontrollierter Test",
  "legacy_last_reply": "Älterer Einzelwert",
  "Warmup": "Aufwärmen",
  "loading": "Laden",
  "warmup": "Aufwärmen",
  "measuring": "Messen",
  "restoring": "Wiederherstellen",
  "Saved.": "Gespeichert.",
  "History deleted.": "Verlauf gelöscht.",
  "Export saved": "Export gespeichert",
  "Delete local measurement history? Models and this chat are kept.": "Lokalen Messverlauf löschen? Modelle und dieser Chat bleiben erhalten.",
  "Run a short CPU/GPU comparison? Each processor loads once, warms up, then answers three times. Your previous processor setting is restored.": "Kurzen CPU/GPU-Vergleich starten? Jeder Prozessor lädt einmal, wärmt auf und antwortet dann dreimal. Deine bisherige Prozessoreinstellung wird wiederhergestellt.",
  "Median of up to 30 comparable replies. Range: middle 50%. Different prompts are observations, not a controlled speed comparison. First text includes hidden preparation. RAM is process RSS; sampled peaks can miss brief spikes. GPU memory and power are not measured.": "Median aus bis zu 30 vergleichbaren Antworten. Bereich: mittlere 50 %. Unterschiedliche Eingaben sind Beobachtungen, kein kontrollierter Geschwindigkeitsvergleich. Erster Text enthält die ausgeblendete Vorbereitung. RAM ist Prozess-RSS; Stichproben können kurze Spitzen verpassen. GPU-Speicher und Energie werden nicht gemessen."
});

Object.assign(german, {'disconnected':'Verbindung getrennt'});

Object.assign(german, {'Earlier configuration':'Frühere Konfiguration'});

Object.assign(german, {
  'Activity':'Aktivität','Stop':'Stopp','Phases':'Phasen','Sending…':'Wird gesendet…',
  'A running service does not prove that the model is making progress. No input progress counter is available.':'Ein laufender Dienst belegt keinen Modellfortschritt. Für die Eingabeverarbeitung ist kein Fortschrittszähler verfügbar.',
  'Validating local artifact':'Lokale Datei prüfen','Checking model':'Modell prüfen','Downloading model':'Modell herunterladen',
  'Stopping':'Wird gestoppt','Starting runtime':'Laufzeit starten','Loading model':'Modell laden','Ready':'Bereit',
  'Connecting':'Verbindung herstellen','Opening session':'Sitzung öffnen','Reading input':'Eingabe einlesen',
  'Processing input':'Eingabe verarbeiten','Generating':'Antwort erzeugen','Preparing answer':'Antwort vorbereiten','Answering':'Antwort ausgeben',
  'Waiting for service':'Warte auf den Dienst','Failed':'Fehlgeschlagen','Stopped':'Gestoppt','Working':'In Arbeit',
  'Status unavailable':'Status nicht verfügbar','Status is stale. Reconnect to see current activity.':'Status veraltet. Erneut verbinden, um die aktuelle Aktivität zu sehen.',
  'The operation failed. Retry the model or choose another processor.':'Der Vorgang ist fehlgeschlagen. Modell erneut starten oder anderen Prozessor wählen.',
  'Large models can take time on CPU.':'Große Modelle können auf der CPU Zeit benötigen.',
  'No progress report available':'Keine Fortschrittsmeldung verfügbar','Operation':'Vorgang','Stage':'Phase','Phase elapsed':'Phasendauer',
  'Total elapsed':'Gesamtdauer','Last progress report':'Letzte Fortschrittsmeldung vor','Runtime':'Laufzeit','Process alive':'Prozess läuft',
  'Not running':'Gestoppt','Processor':'Prozessor','Error code':'Fehlercode'
});
translateStatic();
