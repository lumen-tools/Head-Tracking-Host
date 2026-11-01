# Head Tracking Smoother

Turns **APL Virtuoso's Yaw/Pitch/Roll knobs** with the **Supperware Head Tracker** – via MIDI and
**Gig Performer** (Essentials is enough).

Why: Virtuoso's built-in OSC head tracking input crackles on head movements, while turning its
knobs does not. This app takes the head tracking from Bridgehead and drives the knobs instead.

```
Supperware → Bridgehead (OSC /ypr) → Head Tracking Smoother → virtual MIDI port → Gig Performer → Virtuoso knobs
```

Unofficial helper tool, not affiliated with Supperware, APL or Deskew.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingSmoother.exe` |
| Mac with Apple Silicon (M1/M2/M3/M4) | `HeadTrackingSmoother-macOS-AppleSilicon.zip` |
| Mac with Intel processor | `HeadTrackingSmoother-macOS-Intel.zip` |

All builds are portable – no installation needed.

## Setup

1. **Virtual MIDI port**
   - Windows: install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html), start it and create a port (click **+**).
   - Mac: nothing to install – choose **"Head Tracking Smoother (virtual port)"** in the app.
2. **Bridgehead:** profile **DaVinci Resolve** (sends `/ypr` yaw/pitch/roll to port 8000).
   Close Virtuoso standalone if it is still listening on port 8000.
3. **Head Tracking Smoother:** start it and choose the MIDI port. The dot turns green and shows
   "sending MIDI to Gig Performer".
4. **Gig Performer:**
   - Load **APL Virtuoso** as a plugin and set its head tracking to **Manual**.
   - Add three knob widgets and map them to Virtuoso's **Yaw**, **Pitch** and **Roll** parameters.
   - MIDI-learn each widget from the virtual MIDI port:
     Yaw = **pitch bend channel 1**, Pitch = **pitch bend channel 2**, Roll = **pitch bend channel 3**.
     Tip: move only the matching axis of your head while learning.
5. If an axis turns the wrong way, tick **Invert** for it in the app.

Pitch bend has 16,384 steps (about 0.02° for yaw), so there are no audible steps.
Assumed knob ranges: yaw ±180°, pitch ±90°, roll ±90°.

Settings are saved automatically. **Reset all settings** restores the defaults.

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

- **Red "port in use":** another program listens on the input port (e.g. Virtuoso standalone with
  OSC head tracking). Close it – the app connects automatically.
- **Yellow "no yaw/pitch/roll":** Bridgehead uses a quaternion profile → choose **DaVinci Resolve**.
- **"No MIDI port found" (Windows):** start loopMIDI, create a port, press ↻.
- Crashes are logged to `%APPDATA%\Head Tracking Smoother\crash.log` (Windows) or
  `~/Library/Application Support/Head Tracking Smoother/crash.log` (Mac).

## Building from source

```
pip install -r requirements.txt
python make_icons.py
pyinstaller --onefile --windowed --collect-all customtkinter --hidden-import rtmidi --name "Head Tracking Smoother" --icon icon.ico app.py   # Windows
pyinstaller --windowed --collect-all customtkinter --hidden-import rtmidi --name "Head Tracking Smoother" --icon icon.icns app.py            # Mac
```

Tests: `python -m unittest discover -s tests`

## Authors

- [lumen-tools](https://github.com/lumen-tools) – idea, design, testing with real hardware
- Claude (Anthropic) – co-author of the code
