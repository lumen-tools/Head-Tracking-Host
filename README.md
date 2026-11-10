# Head Tracking Host

**A small, portable VST3 host for listening to speakers on headphones – with head tracking and
headphone correction.**

Load a binaural room or speaker simulation plugin (e.g. APL Virtuoso, dearVR MONITOR, Waves Abbey
Road Studio 3), send your system audio through it and let your head tracker turn the virtual room.
No DAW, no HeSuVi, no complicated routing inside the plugin.

```
Games / music / video (7.1)
   └─► virtual audio device ─► Head Tracking Host ─► VST3 plugin ─► headphone correction ─► headphones
                                       ▲
       head tracker ── OSC (/SceneRotator/yaw, /pitch, /roll) ──┘
```

## Features

- **Hosts any VST3 plugin** – including plugins inside shell files such as Waves' WaveShell.
- **Head tracking via OSC** in the IEM SceneRotator format. The app turns the plugin's own
  yaw / pitch / roll parameters, so it works with any plugin that has such controls.
- **Headphone correction** with profiles from the [AutoEq](https://github.com/jaakkopasanen/AutoEq)
  database – zero latency, also usable on its own as a system-wide headphone EQ.
- **Up to 7.1.4 input**, free channel routing and a one-click fix for swapped Windows surrounds.
- **Portable:** a single EXE (Windows) or app (Mac); settings are stored next to it.
- **Safe by design:** soft fade-in, feedback protection for loopback devices, safe start after a
  crash, plugin scan in a separate process.

## Requirements

| | Windows 10/11 | macOS (Apple Silicon and Intel) |
|---|---|---|
| Virtual audio device | [VB-Audio HiFi-Cable](https://vb-audio.com/Cable/) | [BlackHole 16ch](https://existential.audio/blackhole/) |
| Audio driver | [FlexASIO](https://github.com/dechamps/FlexASIO) (+ FlexASIO GUI) | built in (Core Audio) |
| Plugin | a binaural / speaker simulation plugin as **VST3** | same |
| Head tracker (optional) | anything that sends the **IEM SceneRotator** OSC format | same |

**Head trackers:** any tracker or app that sends `/SceneRotator/yaw`, `/SceneRotator/pitch` and
`/SceneRotator/roll` (angles in degrees, one message per axis) works – for example the
[Supperware Head Tracker](https://supperware.co.uk) with its Bridgehead app (profile
**IEM Scene Rotator YPR**). Combined `ypr` or quaternion messages are not supported.

## Download

Get the latest version from **[Releases](../../releases)**:

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingHost.exe` |
| macOS | `HeadTrackingHost-macOS.zip` |

Put the program in **its own folder** – its settings file is stored next to it. To update, just
replace the EXE or app; your settings stay.

The app is not commercially code-signed, so the system warns once:
- **Windows:** SmartScreen → *More info* → *Run anyway*.
- **macOS:** open it once, then *System Settings → Privacy & Security* → *Open Anyway*. Move the
  app out of *Downloads* first (e.g. into its own folder).

## Setup on Windows

**1. Virtual 7.1 device (VB-Audio HiFi-Cable)**
1. Install VB-Audio HiFi-Cable and restart.
2. Open the Windows sound control panel (*Sound settings → More sound settings*), tab *Playback*.
3. Set **Hi-Fi Cable Input** as the default device, click **Configure** and choose **7.1 Surround**.
4. In its *Properties → Advanced*, set the same sample rate as your headphones (e.g. 24 bit,
   48000 Hz).

**2. FlexASIO**
1. Install FlexASIO and FlexASIO GUI.
2. In FlexASIO GUI: backend **WASAPI**, input device **Hi-Fi Cable Output**, output device
   **your headphones**, channels **8** (input) and **2** (output). Leave buffer size, latency
   and the WASAPI exclusive options unset. *Save to default*.

**3. Head Tracking Host**
1. Start the app and pick your plugin from the **Plugin** list (or **Browse...**).
2. **Audio settings...**: type **ASIO**, device **FlexASIO**, sample rate as your headphones,
   buffer size e.g. 256. Enable inputs **1–8** and both outputs.
3. Set **Input** to **7.1 (8 channels)**.
4. Click **Channels** → **Swap side/rear (Windows)** – Windows delivers the side and rear
   surrounds in swapped order.
5. Optional: turn on **FlexASIO start sound** (see [below](#flexasio-start-sound-windows)), so you
   don't have to start playback before opening the app.

Then continue with [Head tracking](#head-tracking).

## Setup on macOS

**1. Virtual device (BlackHole)**
1. Install BlackHole 16ch.
2. In *Audio MIDI Setup*, create an **Aggregate Device** with BlackHole 16ch (drift correction
   **on**) and your headphones (drift correction off). Use the headphones as **clock source**.
3. Configure the speakers of BlackHole 16ch as **7.1 Rear Surround**.
4. Choose **BlackHole 16ch** as sound output in *System Settings → Sound* (control the volume
   in Audio MIDI Setup).

**2. Head Tracking Host**
1. Start the app and allow **microphone access** (otherwise no audio arrives from BlackHole).
2. Pick your plugin from the **Plugin** list (or **Browse...**).
3. **Audio settings...**: input = the **aggregate device**, output = **your headphones**,
   sample rate as your headphones, buffer size e.g. 128. Enable **8 inputs**.
4. Set **Input** to **7.1 (8 channels)**. No channel swap is needed on the Mac.

## Head tracking

1. Start your head tracker app. With Supperware: connect the tracker, open Bridgehead, choose the
   profile **IEM Scene Rotator YPR** (port 9000), tracking rate 25 Hz or higher.
2. Close other programs that listen on the same port (e.g. a plugin's standalone app with its
   own OSC head tracking).
3. In Head Tracking Host turn on **Head tracking** and keep **OSC port** at 9000.
4. For **Yaw**, **Pitch** and **Roll** choose the matching plugin parameters
   (APL Virtuoso: *HT Yaw*, *HT Pitch*, *HT Roll*). If the plugin only has one rotation control,
   use it for Yaw and leave Pitch and Roll at **- none -**.
5. If an axis turns the wrong way, tick **Invert** for it.
6. In the plugin, turn off its own head tracking input (Virtuoso: *Head tracking → Manual*) and
   choose its 7.1 input configuration (Virtuoso: *7.1 A SMPTE*).

When everything runs, the dot at the top left is green and shows sample rate, format, latency and
the tracker rate.

## Using the app

**Plugin list.** On the first start the app scans your VST3 folder like a DAW. Plugins inside
shell files are listed by name, and each plugin keeps its own settings. A plugin that crashes or
hangs during the scan is marked **(skipped by scan)** and can still be loaded by hand. After
installing new plugins, choose **Rescan plugins** at the end of the list.
Choose **(none - headphone correction only)** to use the app purely as a headphone EQ.

**Input and channels.** *Input* sets how many channels the plugin receives (7.1, 7.1.4, 5.1 or
stereo). *Channels* lets you route any device input to any plugin channel. If a plugin only
accepts 5.1, **Mix 7.1 rear into 5.1** mixes the two extra 7.1 channels into its surround
channels instead of dropping them.

**Output.**
- **Speaker simulation** – off: you hear the input without the plugin (downmixed to stereo), handy
  for A/B comparisons.
- **Headphone correction** – choose one of five slots; **Edit...** opens the correction window:
  search the AutoEq database (*Find in database...*, recommended measurement per headphone,
  *Show all measurements* for every source) or **import** an *EqualizerAPO ParametricEq* file
  from [autoeq.app](https://autoeq.app). Don't combine it with a correction inside the plugin
  (e.g. Virtuoso's HPC EQ), or your headphones are corrected twice.

**Window.** All windows can be resized; **Plugin zoom** scales plugins that support it. The
**input levels** show the audio going into the plugin, the **head indicator** shows your head
movement.

**Status.** The line at the top left only speaks up when something needs doing: green means all
good, yellow or red names the problem and what to do.

**Without head tracking.** With **Head tracking** off, the app frees the port and sets the chosen
yaw / pitch / roll parameters to 0° – also at every start and plugin load – and leaves the
plugin's controls to you.

<a id="flexasio-start-sound-windows"></a>**FlexASIO start sound (Windows, off by default).**
FlexASIO only starts once its input receives audio, so normally you have to start playback first.
With this switch on (group *Global (all plugins)*), the app plays a short, very quiet burst of soft
noise (about half a second) into the playback side of FlexASIO's input whenever FlexASIO is
opened – at program start or when you choose it in *Audio settings...*. FlexASIO starts right
away; nothing else is played. Automatic reconnects play no sound.

**Auto-reconnect audio device** (off by default). If the audio device stops unexpectedly, the
status shows *Audio device stopped* – click **Audio settings...** to reconnect. With the switch on,
the app tries to reopen your saved device every few seconds instead (it may pause briefly during
each attempt).

**Protection.**
- The sound fades in softly after every start or device change.
- If a loopback device is used as input **and** output (also inside FlexASIO's configuration),
  the output stays muted and a warning is shown.
- If the last start did not complete, the next one starts without the saved device and plugin
  (*Safe start*) – choose them again, all settings are kept.

## Troubleshooting

| Status | What to do |
|---|---|
| **Port 9000 is in use** | Another program listens on that port – close it. |
| **No head tracking data** | Start your head tracker app, or turn head tracking off. |
| **Receiving … but no yaw/pitch/roll** | Your tracker sends another format – use the IEM SceneRotator yaw/pitch/roll messages. |
| **Output muted: input and output are the same loopback device** | Choose your headphones as output (in FlexASIO GUI on Windows). |
| **Audio device stopped** | Click *Audio settings...* or turn on *Auto-reconnect audio device*. |
| No sound at all | Check the enabled channels in *Audio settings...*; on the Mac check microphone access. |

**Settings** are stored in `Head Tracking Host.settings` next to the program. If that folder is not
writable, `%APPDATA%\Head Tracking Host` (Windows) or
`~/Library/Application Support/Head Tracking Host` (Mac) is used instead.

## Building from source

Requires CMake 3.22+ and a C++17 compiler (Visual Studio on Windows, Xcode command line tools on
macOS). JUCE is downloaded and patched automatically.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## License

Head Tracking Host is free software under the **GNU Affero General Public License v3.0 or later**
– see [LICENSE](LICENSE). Third-party software and its licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and shown in the app under **About & licenses**.

Head Tracking Host is an independent project and is not affiliated with or endorsed by Supperware,
APL, Waves, Dear Reality, Sennheiser, VB-Audio, Steinberg or any other company mentioned here. VST
and ASIO are trademarks of Steinberg Media Technologies GmbH; all other names belong to their
respective owners.

## Credits

- [lumen-tools](https://github.com/lumen-tools) – idea, design and testing with real hardware
- Claude (Anthropic) – co-author of the code
- [AutoEq](https://github.com/jaakkopasanen/AutoEq) by Jaakko Pasanen – headphone correction data
- Built with [JUCE](https://juce.com)
