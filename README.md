# HeadSmooth

Smooths the head tracking of the **Supperware Head Tracker** (via Bridgehead)
before it reaches **APL Virtuoso** – against crackling during head movements.

```
Supperware → Bridgehead → HeadSmooth (smoothing, 200 Hz) → APL Virtuoso
```

Unofficial helper tool, not affiliated with Supperware or APL.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadSmooth.exe` |
| Mac with Apple Silicon (M1/M2/M3/M4) | `HeadSmooth-macOS-AppleSilicon.zip` |
| Mac with Intel processor | `HeadSmooth-macOS-Intel.zip` |

All builds are portable – no installation needed.

## Setup

1. **Bridgehead:** keep the *APL Virtuoso* profile, change nothing (it sends to port 8000).
2. **Virtuoso:** Head tracking → *OSC* → gear icon → profile *APL*, set **port to 8001**.
3. Start **HeadSmooth**. The dot turns green and shows "Receiving".

Ports can be changed in HeadSmooth ("Apply"). Settings are saved.

### Adjusting the smoothing

- Slider to the left: more direct; to the right: smoother. Default: 40 ms.
- "Bypass smoothing" forwards the data unsmoothed, for a direct A/B comparison.

### Running in the background

- **Windows:** close the window or click "Minimize to tray" – it keeps running in the
  notification area. Right-click the icon → Open / Quit.
- **Mac:** close the window – it keeps running; click the Dock icon to bring it back.
  Quit with Cmd+Q.

## First launch: security warnings

The app is not commercially code-signed, so the system warns once:

- **Windows (SmartScreen):** "More info" → "Run anyway".
- **Mac:** a warning appears on first launch. Then go to
  *System Settings → Privacy & Security* and click **"Open Anyway"** at the bottom.

## Troubleshooting

- **"Port 8000 is already in use":** Virtuoso is still listening on 8000 → set Virtuoso to 8001,
  then click "Apply" in HeadSmooth. Or HeadSmooth is already running.
- **Yellow "waiting for Bridgehead":** Bridgehead is not running, the tracker is not connected,
  or Bridgehead sends to a port other than "Input".
- Crashes are logged to `%APPDATA%\HeadSmooth\crash.log` (Windows) or
  `~/Library/Application Support/HeadSmooth/crash.log` (Mac).

## Building from source

```
pip install -r requirements.txt
python make_icons.py
pyinstaller --onefile --windowed --name HeadSmooth --icon icon.ico app.py      # Windows
pyinstaller --windowed --name HeadSmooth --icon icon.icns app.py               # Mac
```

Tests: `python -m unittest discover -s tests`
