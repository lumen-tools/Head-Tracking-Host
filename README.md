# Head Tracking Host

Head tracking for binaural renderers such as **APL Virtuoso** with the **Supperware Head Tracker** –
without the crackling.

Some renderers crackle on head movements when they get head tracking through their OSC input,
while turning their Yaw/Pitch/Roll knobs does not. Head Tracking Host loads the renderer as a
plugin, plays your audio through it and turns those knobs with the head tracking from Bridgehead.

```
Supperware → Bridgehead (OSC, IEM profile) → Head Tracking Host (renderer plugin inside) → headphones
```

Works with any VST3 plugin that has yaw/pitch/roll parameters. Unofficial tool, not affiliated
with Supperware or APL.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingHost.exe` |
| Mac (Apple Silicon and Intel) | `HeadTrackingHost-macOS.zip` |

Portable – no installation needed, settings are stored next to the program. Requires the renderer
as a **VST3 plugin** (for Virtuoso it is installed together with Virtuoso).

**Updating:** replace only the EXE or the app and keep `Head Tracking Host.settings` next to it –
all settings stay.

## Setup

1. **Close the renderer's standalone app** (e.g. Virtuoso standalone).
2. **Bridgehead:** profile **IEM Scene Rotator YPR** (sends yaw, pitch and roll to port 9000),
   tracking rate **25 Hz** (higher rates work too).
3. **Start Head Tracking Host** and choose the plugin from the **Plugin** list
   (or **Browse...** for a plugin in another folder). Next time it opens the last plugin automatically.
   Files that hold several plugins (e.g. Waves' **WaveShell**) ask once which plugin to load; after
   that every plugin inside is listed by its own name (e.g. Abbey Road Studio 3).
4. **Audio settings...** – the same devices you use for the standalone app:
   - **Windows:** device type **ASIO**, device **FlexASIO**. Enable the input channels
     (IN 0–7 for 7.1) and the output channels.
   - **Mac:** input = your **aggregate device** with BlackHole (created in Audio MIDI Setup with drift
     correction for BlackHole and your headphones as clock source), output = your **headphones**.
     Enable the **BlackHole input channels** (1–8 for 7.1). macOS asks for microphone access on
     first start; allow it, otherwise no audio arrives.
5. **Yaw / Pitch / Roll:** choose the plugin parameters. For Virtuoso they are selected
   automatically (HT Yaw, HT Pitch, HT Roll).
6. **In the plugin:** turn off its own head tracking input. Virtuoso: Head tracking → **Manual**.
7. **Input:** the channel layout of your source – 7.1 by default.
8. If an axis turns the wrong way, tick **Invert** for it. By default nothing is inverted – the
   normal orientation is the one used by IEM Scene Rotator and Virtuoso.

Everything is saved and restored on the next start: audio devices, Bridgehead port, the last plugin
and, per plugin, its own settings, the chosen parameters and the invert options.

**Input channels:** click **Channels...** to choose which audio input feeds each plugin channel
(L, R, C, LFE, Lss, Rss, Lrs, Rrs …). This works with every plugin. On **Windows** the 7.1 side and
rear surrounds often arrive swapped – the preset **Swap side/rear (Windows)** fixes that in one
click, **Reset (1:1)** restores the normal order. On the **Mac** this is not needed. (If you
corrected this in a plugin's own matrix before, set that matrix back to normal.)

**Without head tracking:** untick **Head tracking**. The app stops listening to Bridgehead,
frees the port, sets Yaw/Pitch/Roll to 0° and leaves the plugin's knobs to you.

## Window

- **Sidebar** on the left with all settings in groups (Plugin, Head tracking, Output, Input levels);
  the plugin's own interface on the right.
- **All windows can be resized.** If the window is smaller than the plugin, scroll horizontally and
  vertically; if it is larger, the plugin is centred. **Plugin zoom** (75–150 %): on Windows for
  plugins that support scaling; on the Mac only for plugins with a resizable window (macOS doesn't
  let a host scale fixed-size plugins – the zoom is greyed out then).
- **Input levels:** twelve meters show the audio going into the plugin (after channel routing);
  unused channels are dimmed.
- **Head indicator:** a top view of your head that turns with you, plus yaw, pitch and roll.
  Switch it off if the plugin has its own (e.g. Virtuoso).

## Two switches: speaker simulation and headphone correction

- **Speaker simulation** – on: the plugin (e.g. Virtuoso) renders your speakers. Off: you hear the
  input without the plugin (surround downmixed to stereo) – handy for A/B comparisons.
- **Headphone correction** – AutoEq correction for your headphones, on or off. It works **with or
  without** speaker simulation, and even without any plugin – choose **(none - headphone correction
  only)** in the plugin list, then Head Tracking Host is a system-wide headphone EQ. For stereo sources set **Input** to **Stereo** (the 7.1 downmix is a bit
  quieter to avoid clipping).

Don't combine headphone correction with a plugin's own headphone correction (e.g. Virtuoso's HPC EQ),
otherwise the headphones are corrected twice.

### Headphone correction window

Click **Headphone correction...**:
- **Five slots** store profiles offline; the dropdown (here or in the main window) picks the active one.
- **Find in database...** searches the [AutoEq](https://github.com/jaakkopasanen/AutoEq) database
  (internet needed only for searching, the list is cached). By default it shows the **recommended
  measurement** per headphone; tick **Show all measurements** to see every source
  (oratory1990, crinacle, Rtings ...). Select a headphone and click a slot number (**1–5**) next to *Save to slot*.
- **Import file...** loads a parametric EQ file into the selected slot – no internet needed
  afterwards. To get one from [autoeq.app](https://autoeq.app): choose your headphones and target
  (Harman), set **Select equalizer app** to **EqualizerAPO ParametricEq** (not "Custom Parametric Eq")
  and click the download arrow. The `.txt` file looks like this:
  ```
  Preamp: -3.0 dB
  Filter 1: ON LSC Fc 105 Hz Gain 0.6 dB Q 0.70
  Filter 2: ON PK Fc 71 Hz Gain 1.4 dB Q 3.73
  ```
  The `… ParametricEQ.txt` files from the AutoEq GitHub repository have the same format.
- **Clear slot** empties it. The **curve** shows the correction of the selected slot.

The profiles use AutoEq's Harman targets (over-ear target for over-ear, in-ear target for in-ear).
The EQ is minimum-phase (IIR) and adds **no latency** – important with head tracking.

## Hearing protection

- After every start, audio device change or automatic reconnect the sound fades in over 2 seconds.
- If a loopback device (e.g. BlackHole, VB-Cable) is input **and** output – including FlexASIO's
  configured devices – the output stays muted and the app shows a warning.

## Automatic reconnect

If the audio device really stops – e.g. it was closed by the system, another program changed the
sample rate, or the device was briefly disconnected – Head Tracking Host reopens it automatically
(status: "Audio device reconnected"), with a soft fade-in. A device that just delivers silence
(e.g. FlexASIO while nothing is playing) is left alone and continues by itself when audio starts.

## Safe start

If the previous start did not complete (freeze or crash), the next start opens neither the saved
audio device nor the plugin and shows "Safe start". Choose them again – all settings are kept.

## First launch: security warnings

The app is not commercially code-signed, so the system warns once:

- **Windows (SmartScreen):** "More info" → "Run anyway".
- **Mac:** a warning appears on first launch. Then go to
  *System Settings → Privacy & Security* and click **"Open Anyway"** at the bottom.

## Status display

The line at the top left only speaks up when something needs doing. With a **green dot**
everything works and it shows what you cannot see elsewhere: sample rate, input format and total
latency, below that the tracker rate (with head tracking on), the audio route and the buffer size.
A **yellow or red dot** replaces it with the problem and what to do. The switches below are not
repeated there.

## Troubleshooting

- **Red "port in use":** another program listens on port 9000 (e.g. Virtuoso standalone with OSC
  head tracking). Close it – the app connects automatically.
- **Yellow "no head tracking data":** nothing arrives on the port – start your head tracker app
  (Bridgehead: profile **IEM Scene Rotator YPR**) or turn head tracking off.
- **Yellow "no yaw/pitch/roll":** messages arrive but in another format – send
  `/SceneRotator/yaw`, `/pitch`, `/roll` (Bridgehead: profile **IEM Scene Rotator YPR**).
- **No sound:** check the channels in **Audio settings...** and, on the Mac, microphone access in
  *System Settings → Privacy & Security → Microphone*.
- **Settings are portable:** they are stored in `Head Tracking Host.settings` **next to the program**
  (next to the EXE on Windows, next to the app on the Mac) – copy the folder and all settings come
  along. If that folder is not writable, the user folder is used instead: `%APPDATA%\Head Tracking Host`
  (Windows) or `~/Library/Application Support/Head Tracking Host` (Mac). On the Mac, move the app
  out of *Downloads* once (e.g. into its own folder) – macOS runs unsigned apps started from
  Downloads from a hidden read-only location.

## Building from source

Requires CMake 3.22+ and a C++17 compiler (Visual Studio on Windows, Xcode command line tools on
the Mac). JUCE is downloaded automatically.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## License

AGPL-3.0-or-later – see [LICENSE.md](LICENSE.md). Built with [JUCE](https://juce.com) (AGPLv3) and
the ASIO SDK bundled with JUCE (GPLv3). ASIO and VST are trademarks of Steinberg Media
Technologies GmbH.

## Authors

- [lumen-tools](https://github.com/lumen-tools) – idea, design, testing with real hardware
- Claude (Anthropic) – co-author of the code
