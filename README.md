# Head Tracking Smoother

Smooths the head tracking of the **Supperware Head Tracker** (via Bridgehead)
before it reaches **APL Virtuoso** – against crackling during head movements.

```
Supperware → Bridgehead → Head Tracking Smoother (smoothing, 200 Hz) → APL Virtuoso
```

Unofficial helper tool, not affiliated with Supperware or APL.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingSmoother.exe` |
| Mac with Apple Silicon (M1/M2/M3/M4) | `HeadTrackingSmoother-macOS-AppleSilicon.zip` |
| Mac with Intel processor | `HeadTrackingSmoother-macOS-Intel.zip` |

All builds are portable – no installation needed.

## Setup

1. **Bridgehead:** keep the *APL Virtuoso* profile, change nothing (it sends to port 8000).
2. **Virtuoso:** Head tracking → *OSC* → gear icon → profile *APL*, set **port to 8001**.
3. Start **Head Tracking Smoother**. The dot turns green and shows "Receiving".

The window follows the light/dark mode of Windows or macOS.

Ports can be changed in Head Tracking Smoother and take effect automatically. All settings are saved.
"Reset all settings" restores the defaults (8000 → 8001, 40 ms, bypass off).

### Adjusting the smoothing

- **Recommended: 20–60 ms** (default 40 ms). Below 20 ms there is little smoothing;
  above 60 ms the added delay becomes noticeable.
- Slider to the left: more direct; to the right: smoother.
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

- **"Port 8000 is in use":** Virtuoso is still listening on 8000 → set Virtuoso to 8001.
  Head Tracking Smoother connects automatically. Or Head Tracking Smoother is already running.
- **Yellow "waiting for Bridgehead":** Bridgehead is not running, the tracker is not connected,
  or Bridgehead sends to a port other than "Input".
- Crashes are logged to `%APPDATA%\Head Tracking Smoother\crash.log` (Windows) or
  `~/Library/Application Support/Head Tracking Smoother/crash.log` (Mac).

## Building from source

```
pip install -r requirements.txt
python make_icons.py
pyinstaller --onefile --windowed --name "Head Tracking Smoother" --icon icon.ico app.py      # Windows
pyinstaller --windowed --name "Head Tracking Smoother" --icon icon.icns app.py               # Mac
```

Tests: `python -m unittest discover -s tests`

## Authors

- [lumen-tools](https://github.com/lumen-tools) – idea, design, testing with real hardware
- Claude (Anthropic) – co-author of the code
