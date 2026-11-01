# Head Tracking Host

Head tracking for binaural renderers such as **APL Virtuoso** with the **Supperware Head Tracker** –
without the crackling.

Some renderers crackle on head movements when they get head tracking through their OSC input,
while turning their Yaw/Pitch/Roll knobs does not. Head Tracking Host loads the renderer as a
plugin, plays your audio through it and turns those knobs with the head tracking from Bridgehead.

```
Supperware → Bridgehead (OSC /ypr) → Head Tracking Host (renderer plugin inside) → headphones
```

Works with any VST3 plugin that has yaw/pitch/roll parameters. Unofficial tool, not affiliated
with Supperware or APL.

## Download

From **Releases** (right-hand side of the GitHub page):

| System | File |
|---|---|
| Windows 10/11 | `HeadTrackingHost.exe` |
| Mac (Apple Silicon and Intel) | `HeadTrackingHost-macOS.zip` |

Portable – no installation needed. Requires the renderer as a **VST3 plugin** (for Virtuoso it is
installed together with Virtuoso).

## Setup

1. **Close the renderer's standalone app** (e.g. Virtuoso standalone).
2. **Bridgehead:** profile **DaVinci Resolve**, tracking rate **25 Hz** (higher rates work too).
   Despite its name, this profile simply sends the tracker's raw yaw/pitch/roll in one message
   (`/ypr`) to port 8000 – exactly what Head Tracking Host needs.
3. **Start Head Tracking Host** and choose the plugin from the **Plugin** list
   (or **Browse...** for a plugin in another folder). Next time it opens the last plugin automatically.
4. **Audio settings...** – the same devices you use for the standalone app:
   - **Windows:** device type **ASIO**, device **FlexASIO**. Enable the input channels
     (IN 0–7 for 7.1) and the output channels.
   - **Mac:** choose your device, e.g. an **aggregate device** with BlackHole and your headphones
     (recommended for drift correction). Enable the **BlackHole input channels** (1–8 for 7.1) and
     **only the two output channels of your headphones** – not BlackHole's outputs.
     macOS asks for microphone access on first start; allow it, otherwise no audio arrives.
5. **Yaw / Pitch / Roll:** choose the plugin parameters. For Virtuoso they are selected
   automatically (HT Yaw, HT Pitch, HT Roll).
6. **In the plugin:** turn off its own head tracking input. Virtuoso: Head tracking → **Manual**.
7. **Input:** the channel layout of your source – 7.1 by default.
8. If an axis turns the wrong way, tick **Invert** for it. Pitch is inverted by default, which is
   correct for Virtuoso with the Bridgehead profile DaVinci Resolve.

Everything is saved and restored on the next start: audio devices, Bridgehead port, the last plugin
and, per plugin, its own settings, the chosen parameters and the invert options.

**Input channels:** click **Channels...** to choose which audio input feeds each plugin channel
(L, R, C, LFE, Lss, Rss, Lrs, Rrs …). This works with every plugin. On Windows the 7.1 side and
rear surrounds often arrive swapped – the preset **Swap side/rear (Windows)** fixes that in one
click, **Reset (1:1)** restores the normal order. (If you corrected this in a plugin's own matrix
before, set that matrix back to normal.)

## Hearing protection

- After every start or audio device change the sound fades in softly.
- If a loopback device (e.g. BlackHole, VB-Cable) is input **and** output – including FlexASIO's
  configured devices – the output stays muted and the app shows a warning.
- If the output clips continuously for half a second (typical for feedback), the output is muted.
  Fix the output channels, then click **Audio settings...** to unmute with a fade-in.

## First launch: security warnings

The app is not commercially code-signed, so the system warns once:

- **Windows (SmartScreen):** "More info" → "Run anyway".
- **Mac:** a warning appears on first launch. Then go to
  *System Settings → Privacy & Security* and click **"Open Anyway"** at the bottom.

## Troubleshooting

- **Red "port in use":** another program listens on port 8000 (e.g. Virtuoso standalone with OSC
  head tracking). Close it – the app connects automatically.
- **Yellow "no yaw/pitch/roll":** Bridgehead uses another profile → choose **DaVinci Resolve**.
- **No sound:** check the channels in **Audio settings...** and, on the Mac, microphone access in
  *System Settings → Privacy & Security → Microphone*.
- Settings are stored in `%APPDATA%\Head Tracking Host` (Windows) or
  `~/Library/Application Support/Head Tracking Host` (Mac).

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
