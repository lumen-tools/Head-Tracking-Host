# Head Tracking Bridge

Head tracking for **APL Virtuoso** with the **Supperware Head Tracker** – without the crackling.

Why: Virtuoso's built-in OSC head tracking input crackles on head movements, while turning its
Yaw/Pitch/Roll knobs does not. Both tools in this repository turn the knobs instead.

Unofficial helper tools, not affiliated with Supperware, APL or Deskew.

## Head Tracking Host (recommended)

One program instead of Virtuoso standalone. It loads the Virtuoso plugin, plays your audio
through it and turns its knobs with the head tracking from Bridgehead. No extra software needed.

```
Supperware → Bridgehead (OSC /ypr) → Head Tracking Host (with Virtuoso inside) → headphones
```

### Download

From **Releases**: `HeadTrackingHost.exe` (Windows 10/11) or `HeadTrackingHost-macOS.zip`
(Mac, Apple Silicon and Intel). Portable – no installation needed.
Requires the **Virtuoso VST3 plugin** (installed together with Virtuoso).

### Setup

1. **Close Virtuoso standalone.**
2. **Bridgehead:** profile **DaVinci Resolve**, tracking rate 100 Hz.
3. **Start Head Tracking Host.** It finds the Virtuoso VST3 automatically
   (otherwise click **Choose plugin...**). Virtuoso's usual window appears below the toolbar.
4. **Audio settings...** – choose the same devices you use for Virtuoso standalone:
   - Windows: device type **ASIO**, device **FlexASIO**; enable the input channels (IN 0–7 for 7.1)
     and the output channels.
   - Mac: input **BlackHole**, output your headphones / interface.
5. **In Virtuoso:** set **Head tracking** to **Manual**. Choose HRTF, room and input config as usual.
6. **Input** (toolbar): the channel layout of your source – 7.1 by default.
7. If an axis turns the wrong way, tick **Invert** for it.

All settings – audio devices, Bridgehead port, Virtuoso's own settings – are saved and restored
on the next start.

If the surround channels end up in the wrong places, set **Channel mapping** in Virtuoso to
**1:1** and choose the same **Input config** as in Virtuoso standalone.

License: AGPL-3.0-or-later (see `host/LICENSE.md`).

---

## Head Tracking Bridge (MIDI, for Gig Performer)

Alternative if you run Virtuoso as a plugin in **Gig Performer**: turns Virtuoso's Yaw/Pitch/Roll
knobs via MIDI (Gig Performer Essentials is enough).

```
Supperware → Bridgehead (OSC /ypr) → Head Tracking Bridge → virtual MIDI port → Gig Performer → Virtuoso knobs
```

### Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingBridge.exe` |
| Mac with Apple Silicon (M1/M2/M3/M4) | `HeadTrackingBridge-macOS-AppleSilicon.zip` |
| Mac with Intel processor | `HeadTrackingBridge-macOS-Intel.zip` |

All builds are portable – no installation needed.

### Setup

Do steps 1 and 4 once. After that you only start Bridgehead, loopMIDI (Windows),
Head Tracking Bridge and Gig Performer.

#### 1. Virtual MIDI port (once)

- **Windows:** download and install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html).
  Start it and click **+**. A port called **loopMIDI Port** appears. Leave loopMIDI running.
- **Mac:** nothing to install.

#### 2. Bridgehead

- **Profile:** choose **DaVinci Resolve** (it sends yaw/pitch/roll to port 8000).
- **Tracking rate:** 100 Hz.
- Close **Virtuoso standalone** – Virtuoso now runs inside Gig Performer.

#### 3. Head Tracking Bridge

- Start the app.
- **MIDI port:** choose **loopMIDI Port** (Mac: **Head Tracking Bridge (virtual port)**).
  The tracker's own port ("Head Tracker") and the Windows synth are hidden on purpose.
- The dot turns green: "Receiving – sending MIDI to Gig Performer".

#### 4. Gig Performer (once)

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

#### 5. Check

Turn your head to the left: Virtuoso's **Yaw** knob must follow. Nod and tilt to check Pitch and
Roll. If an axis turns the wrong way, tick **Invert** for it in Head Tracking Bridge.

Details: Yaw = pitch bend channel 1, Pitch = channel 2, Roll = channel 3. Pitch bend has
16,384 steps (about 0.02° for yaw), so there are no audible steps. Assumed knob ranges:
yaw ±180°, pitch ±90°, roll ±90°.

Settings are saved automatically. **Reset** restores the defaults. **Setup guide** opens this page.

#### Running in the background

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
