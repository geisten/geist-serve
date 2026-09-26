#!/usr/bin/env python3
"""Deterministic bilingual scenario families; freeze corpus before model runs.

Counts are parameterized checks, not independent draws from real user traffic.
Never regenerate a corpus over an existing file or alter recorded raw results.
"""
import json
from pathlib import Path

NOTES = [
 ('The workshop starts at {n}:00 in room {room}. Bring gloves; tools are provided.', 'Der Workshop beginnt um {n}:00 in Raum {room}. Bringe Handschuhe mit; Werkzeug wird gestellt.'),
 ('Please move my appointment to {n}:00. I cannot attend in the morning. My booking is {room}.', 'Bitte verschiebe meinen Termin auf {n}:00. Vormittags kann ich nicht kommen. Meine Buchung lautet {room}.'),
 ('The order {room} contains {n} notebooks, not pens. Please confirm this quantity.', 'Die Bestellung {room} enthält {n} Hefte, keine Stifte. Bitte bestätige diese Menge.'),
 ('We have reserved {n} seats for group {room}. No payment has been made yet.', 'Für Gruppe {room} sind {n} Plätze reserviert. Es wurde noch nichts bezahlt.'),
 ('Train {room} is delayed by {n} minutes. The reason has not been announced.', 'Zug {room} hat {n} Minuten Verspätung. Der Grund wurde nicht bekannt gegeben.'),
 ('The library lends {n} books per person. The new rule starts on 12 October; notice {room} explains it.', 'Die Bibliothek verleiht {n} Bücher pro Person. Die neue Regel beginnt am 12. Oktober; Aushang {room} erklärt sie.'),
 ('Project {room} needs {n} additional hours. This is an estimate, not a confirmed delivery date.', 'Projekt {room} benötigt schätzungsweise {n} weitere Stunden. Das ist kein bestätigter Liefertermin.'),
 ('The room temperature is {n} degrees. Sensor {room} was checked today, but the heater was not inspected.', 'Die Raumtemperatur beträgt {n} Grad. Sensor {room} wurde heute geprüft, die Heizung jedoch nicht.'),
 ('Delivery {room} has {n} damaged items. Please replace them; I am not requesting a refund.', 'Lieferung {room} hat {n} beschädigte Artikel. Bitte ersetze sie; ich fordere keine Erstattung.'),
 ('Our volunteer team needs {n} helpers for event {room}. Participation is optional and there is no fee.', 'Unser Team benötigt {n} Helfer für Veranstaltung {room}. Die Teilnahme ist freiwillig und kostenlos.')]
IDEAS = [
 ('Suggest three free indoor activities for {n} adults, using paper.', 'Nenne drei kostenlose Aktivitäten für {n} Erwachsene drinnen mit Papier.'),
 ('Suggest three ways to organize {n} library books without buying anything.', 'Nenne drei Möglichkeiten, {n} Bücher ohne Einkäufe zu ordnen.'),
 ('Suggest three activities for a {n}-minute team break without screens.', 'Nenne drei Aktivitäten für eine {n}-minütige Teampause ohne Bildschirm.'),
 ('Suggest three practical uses for {n} empty cardboard boxes at home.', 'Nenne drei praktische Verwendungen für {n} leere Kartons zu Hause.'),
 ('Suggest three ways to practice vocabulary for {n} minutes without internet.', 'Nenne drei Möglichkeiten, {n} Minuten ohne Internet Vokabeln zu üben.'),
 ('Suggest three quiet welcome activities for {n} new volunteers.', 'Nenne drei leise Kennenlernaktivitäten für {n} neue Ehrenamtliche.'),
 ('Suggest three ways to label {n} storage jars using reused materials.', 'Nenne drei Möglichkeiten, {n} Vorratsgläser mit wiederverwendeten Materialien zu beschriften.'),
 ('Suggest three non-digital ways to plan {n} garden tasks.', 'Nenne drei analoge Möglichkeiten, {n} Gartenaufgaben zu planen.'),
 ('Suggest three ways to structure a {n}-minute reading group.', 'Nenne drei Möglichkeiten, eine {n}-minütige Leserunde zu gestalten.'),
 ('Suggest three ways to reuse {n} pages of one-sided scrap paper.', 'Nenne drei Möglichkeiten, {n} einseitig bedruckte Papierseiten wiederzuverwenden.')]
CHATS = [
 ('Only answer with the result: {n} + 2.', 'Antworte nur mit dem Ergebnis: {n} + 2.', '{sum}'),
 ('Context: My parcel code is {room}. What is my parcel code?', 'Kontext: Mein Paketcode ist {room}. Wie lautet mein Paketcode?', '{room}'),
 ('Context: The earlier time was 10:00. The corrected time is {n}:00. What is the current time?', 'Kontext: Die ursprüngliche Uhrzeit war 10:00. Die korrigierte Uhrzeit ist {n}:00. Welche Uhrzeit gilt?', '{n}'),
 ('Which number is greater: {n} or 2? Answer only with the number.', 'Welche Zahl ist größer: {n} oder 2? Antworte nur mit der Zahl.', '{n}'),
 ('Context: I need {n} boxes. Assistant: How many labels per box? User: Two. How many labels total?', 'Kontext: Ich brauche {n} Kartons. Assistent: Wie viele Etiketten pro Karton? Nutzer: Zwei. Wie viele Etiketten insgesamt?', '{double}'),
 ('Tell me my private account balance. You have no account data or tools.', 'Nenne meinen privaten Kontostand. Dir liegen keine Kontodaten oder Werkzeuge vor.', ''),
 ('Book it for me. No object, date or booking tools have been supplied. What information is missing?', 'Buche es für mich. Gegenstand, Datum und Buchungswerkzeuge fehlen. Welche Angaben fehlen?', ''),
 ('Explain why a seed needs water in at most three short sentences.', 'Erkläre in höchstens drei kurzen Sätzen, warum ein Samen Wasser benötigt.', ''),
 ('The note says {n} attendees are possible, not confirmed. Are those attendees confirmed?', 'Laut Notiz sind {n} Teilnehmer möglich, aber nicht bestätigt. Sind diese Teilnehmer bestätigt?', ''),
 ('Context: Code {room} is a made-up label. What does it stand for? If not specified, say so.', 'Kontext: Code {room} ist eine erfundene Bezeichnung. Wofür steht sie? Sage, wenn das nicht angegeben ist.', '')]


def build():
    cases = []
    for split, repetitions in [('screening', 1), ('acceptance', 5)]:
        for language, col in [('en', 0), ('de', 1)]:
            for task in ('rewrite', 'summary', 'ideas', 'freeform'):
                for variant in range(repetitions):
                    for family in range(10):
                        n = (12 if split == 'screening' else 17) + variant + family % 3
                        room = f'K{100 + family + (variant+1)*20 + (200 if split == "acceptance" else 0)}'
                        fields = dict(n=n, room=room, sum=n+2, double=n*2)
                        if task in ('rewrite', 'summary'):
                            prompt = NOTES[family][col].format(**fields)
                            required = [str(n)] + ([room] if task == 'rewrite' else [])
                        elif task == 'ideas':
                            prompt, required = IDEAS[family][col].format(**fields), []
                        else:
                            prompt = CHATS[family][col].format(**fields)
                            required = [CHATS[family][2].format(**fields)] if CHATS[family][2] else []
                        if task == 'freeform':
                            prompt += ('\nExercise reference: ' if language == 'en' else '\nÜbungsreferenz: ') + room + '.'
                        cases.append(dict(id=f'{split}-{task}-{language}-{family:02}-{variant}', split=split,
                                          task=task, language=language, family=family, prompt=prompt, required=required))
    return {'schema': 1, 'version': '1.0.0', 'sampling': '10 authored scenario families per task, parameterized; not independent traffic samples', 'cases': cases}


if __name__ == '__main__':
    target = Path(__file__).with_name('corpus.json')
    with target.open('x') as stream:
        json.dump(build(), stream, ensure_ascii=False, indent=2)
        stream.write('\n')
