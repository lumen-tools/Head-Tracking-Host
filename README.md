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
8. If an axis turns the wrong way, tick **Invert** for it. By default nothing is inverted – the
   normal orientation is the one used by IEM Scene Rotator and Virtuoso.

**A/B comparison:** **Bypass (dry)** plays the input without the plugin. Stereo passes through
unchanged; surround is downmixed to stereo (centre and surrounds at −3 dB, LFE left out, a bit
quieter overall so it does not clip).

Everything is saved and restored on the next start: audio devices, Bridgehead port, the last plugin
and, per plugin, its own settings, the chosen parameters and the invert options.

**Input channels:** click **Channels...** to choose which audio input feeds each plugin channel
(L, R, C, LFE, Lss, Rss, Lrs, Rrs …). This works with every plugin. On Windows the 7.1 side and
rear surrounds often arrive swapped – the preset **Swap side/rear (Windows)** fixes that in one
click, **Reset (1:1)** restores the normal order. (If you corrected this in a plugin's own matrix
before, set that matrix back to normal.)

## Hearing protection

- After every start, audio device change or automatic reconnect the sound fades in over 2 seconds.
- If a loopback device (e.g. BlackHole, VB-Cable) is input **and** output – including FlexASIO's
  configured devices – the output stays muted and the app shows a warning.

## Automatic reconnect

If the audio stream stops – e.g. the audio device went to sleep after a pause, another program
changed the sample rate, or the device was briefly disconnected – Head Tracking Host notices it
within about two seconds and reopens the device automatically (status: "Reconnecting audio
device..."), with a soft fade-in. A device that is still starting up is left alone, and retries
slow down (3 s, 10 s, 30 s) and stop after five attempts.

## Safe start

If the previous start did not complete (freeze or crash), the next start opens neither the saved
audio device nor the plugin and shows "Safe start". Choose them again – all settings are kept.

## First launch: security warnings

The app is not commercially code-signed, so the system warns once:

- **Windows (SmartScreen):** "More info" → "Run anyway".
- **Mac:** a warning appears on first launch. Then go to
  *System Settings → Privacy & Security* and click **"Open Anyway"** at the bottom.

## Troubleshooting

- **Red "port in use":** another program listens on port 9000 (e.g. Virtuoso standalone with OSC
  head tracking). Close it – the app connects automatically.
- **Yellow "no yaw/pitch/roll":** Bridgehead uses another profile → choose **IEM Scene Rotator YPR**.
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
