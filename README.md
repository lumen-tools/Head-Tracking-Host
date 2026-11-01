# Head Tracking Bridge

Turns **APL Virtuoso's Yaw/Pitch/Roll knobs** with the **Supperware Head Tracker** – via MIDI and
**Gig Performer** (Essentials is enough).

Why: Virtuoso's built-in OSC head tracking input crackles on head movements, while turning its
knobs does not. This app takes the head tracking from Bridgehead and drives the knobs instead.

```
Supperware → Bridgehead (OSC /ypr) → Head Tracking Bridge → virtual MIDI port → Gig Performer → Virtuoso knobs
```

Unofficial helper tool, not affiliated with Supperware, APL or Deskew.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingBridge.exe` |
| Mac with Apple Silicon (M1/M2/M3/M4) | `HeadTrackingBridge-macOS-AppleSilicon.zip` |
| Mac with Intel processor | `HeadTrackingBridge-macOS-Intel.zip` |

All builds are portable – no installation needed.

## Setup

Do steps 1 and 4 once. After that you only start Bridgehead, loopMIDI (Windows),
Head Tracking Bridge and Gig Performer.

### 1. Virtual MIDI port (once)

- **Windows:** download and install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html).
  Start it and click **+**. A port called **loopMIDI Port** appears. Leave loopMIDI running.
- **Mac:** nothing to install.

### 2. Bridgehead

- **Profile:** choose **DaVinci Resolve** (it sends yaw/pitch/roll to port 8000).
- **Tracking rate:** 100 Hz.
- Close **Virtuoso standalone** – Virtuoso now runs inside Gig Performer.

### 3. Head Tracking Bridge

- Start the app.
- **MIDI port:** choose **loopMIDI Port** (Mac: **Head Tracking Bridge (virtual port)**).
  The tracker's own port ("Head Tracker") and the Windows synth are hidden on purpose.
- The dot turns green: "Receiving – sending MIDI to Gig Performer".

### 4. Gig Performer (once)

**a) Allow the MIDI port**
Open **Options → MIDI Ports** and enable **loopMIDI Port** (Mac: **Head Tracking Bridge**) as an input.

**b) Add Virtuoso**
1. Switch to the **Wiring** view.
2. Right-click an empty spot and choose **APL Virtuoso** from the plugin list.
3. Connect **Audio Input → Virtuoso → Audio Output**.
4. Double-click Virtuoso to open it and set **Head tracking** to **Manual**.

**c) Add three knobs**
1. Switch to the **Panels** view and turn on **Edit mode**.
2. Drag three **knobs** from the widget list onto the panel.

**d) Connect each knob – example Yaw**
1. Select the first knob. On the right, the **widget properties** open.
2. In **Mapping**, choose plugin **APL Virtuoso** and parameter **Yaw**.
3. Click **MIDI Learn** in the widget properties.
4. In Head Tracking Bridge, click **MIDI Learn → Yaw**. Only the yaw knob moves for a few
   seconds, so Gig Performer learns exactly this one.
5. The knob in Gig Performer starts moving – done.

Repeat with the second knob for **Pitch** and the third for **Roll**.
Then turn **Edit mode** off and save the gig.

### 5. Check

Turn your head to the left: Virtuoso's **Yaw** knob must follow. Nod and tilt to check Pitch and
Roll. If an axis turns the wrong way, tick **Invert** for it in Head Tracking Bridge.

Details: Yaw = pitch bend channel 1, Pitch = channel 2, Roll = channel 3. Pitch bend has
16,384 steps (about 0.02° for yaw), so there are no audible steps. Assumed knob ranges:
yaw ±180°, pitch ±90°, roll ±90°.

Settings are saved automatically. **Reset** restores the defaults. **Setup guide** opens this page.

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
- **"No MIDI port found" / "Choose a MIDI port" (Windows):** start loopMIDI, click **+**, press ↻.
- Crashes are logged to `%APPDATA%\Head Tracking Bridge\crash.log` (Windows) or
  `~/Library/Application Support/Head Tracking Bridge/crash.log` (Mac).

## Building from source

```
pip install -r requirements.txt
python make_icons.py
pyinstaller --onefile --windowed --collect-all customtkinter --hidden-import rtmidi --name "Head Tracking Bridge" --icon icon.ico app.py   # Windows
pyinstaller --windowed --collect-all customtkinter --hidden-import rtmidi --name "Head Tracking Bridge" --icon icon.icns app.py            # Mac
```

Tests: `python -m unittest discover -s tests`

## Authors

- [lumen-tools](https://github.com/lumen-tools) – idea, design, testing with real hardware
- Claude (Anthropic) – co-author of the code
