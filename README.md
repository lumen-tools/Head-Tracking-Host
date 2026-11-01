# HeadSmooth

Glättet das Head-Tracking des **Supperware Head Trackers** (über Bridgehead),
bevor es bei **APL Virtuoso** ankommt. Gegen Knistern bei Kopfbewegungen.

```
Supperware → Bridgehead → HeadSmooth (Glättung, 200 Hz) → APL Virtuoso
```

Inoffizielles Hilfsprogramm, nicht verbunden mit Supperware oder APL.

## Download

Unter **Releases** (rechts auf der GitHub-Seite):

| System | Datei |
|---|---|
| Windows 10/11 | `HeadSmooth.exe` |
| Mac mit M1/M2/M3/M4 | `HeadSmooth-macOS-AppleSilicon.zip` |
| Mac mit Intel-Prozessor | `HeadSmooth-macOS-Intel.zip` |

Alle Programme sind portabel, eine Installation ist nicht nötig.

## Einrichten

1. **Bridgehead:** Profil *APL Virtuoso* lassen, nichts ändern (sendet an Port 8000).
2. **Virtuoso:** Head tracking → *OSC* → Zahnrad → Profil *APL*, **Port auf 8001** stellen.
3. **HeadSmooth** starten. Der Punkt wird grün und zeigt „Daten kommen an“.

Die Ports lassen sich in HeadSmooth ändern („Übernehmen“). Einstellungen werden gespeichert.

### Glättung einstellen

- Regler nach links: direkter, nach rechts: weicher. Start: 40 ms.
- „Glättung umgehen“ schickt die Daten ungeglättet weiter, zum direkten A/B-Vergleich.

### Hintergrund

- **Windows:** Fenster schließen oder „In den Tray“ – läuft im Infobereich weiter.
  Rechtsklick auf das Symbol → Öffnen / Beenden.
- **Mac:** Fenster schließen – läuft weiter, Klick auf das Dock-Symbol holt es zurück.
  Beenden mit Cmd+Q.

## Erster Start: Sicherheitswarnungen

Das Programm ist nicht kostenpflichtig signiert, daher warnen die Systeme einmalig:

- **Windows (SmartScreen):** „Weitere Informationen“ → „Trotzdem ausführen“.
- **Mac:** Beim ersten Öffnen erscheint eine Warnung. Dann
  *Systemeinstellungen → Datenschutz & Sicherheit* → unten **„Trotzdem öffnen“**.

## Fehlersuche

- **„Port 8000 ist belegt“:** Virtuoso hört noch auf 8000 → in Virtuoso auf 8001 stellen,
  dann in HeadSmooth „Übernehmen“. Oder HeadSmooth läuft schon ein zweites Mal.
- **Gelb „warte auf Bridgehead“:** Bridgehead läuft nicht, Tracker nicht verbunden,
  oder Bridgehead sendet an einen anderen Port als „Eingang“.
- Abstürze werden protokolliert in `%APPDATA%\HeadSmooth\crash.log` (Windows) bzw.
  `~/Library/Application Support/HeadSmooth/crash.log` (Mac).

## Selbst bauen

```
pip install -r requirements.txt
python make_icons.py
pyinstaller --onefile --windowed --name HeadSmooth --icon icon.ico app.py      # Windows
pyinstaller --windowed --name HeadSmooth --icon icon.icns app.py               # Mac
```

Tests: `python -m unittest discover -s tests`
