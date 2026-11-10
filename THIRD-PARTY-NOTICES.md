# Third-party notices

Head Tracking Host is licensed under the GNU Affero General Public License v3.0 or later (see
[LICENSE](LICENSE)). It is built with the following third-party software. The full license texts
of everything compiled into the program are shown in the app under **About & licenses**.

## Built into the program

| Component | License | Notes |
|---|---|---|
| [JUCE](https://juce.com) 8.0.12 | AGPLv3 (open-source option of JUCE's dual license) | Downloaded at build time. **Modified:** the build applies `cmake/PatchJuceVst3ShellIndex.cmake`, which changes `juce_VST3PluginFormatImpl.h` so that plugins inside VST3 shell files (e.g. Waves' WaveShell) are found by their class ID instead of their list position. Changed places are marked `HTH_SHELL_INDEX_FIX`. |
| VST3 SDK (bundled with JUCE) | MIT, © Steinberg Media Technologies GmbH | |
| ASIO SDK (bundled with JUCE) | used under the GPLv3 (dual license: Steinberg ASIO License or GPLv3) | Windows only |
| FLAC, Ogg Vorbis (bundled with JUCE) | BSD | |
| HarfBuzz (bundled with JUCE) | Old MIT | |
| SheenBidi (bundled with JUCE) | Apache 2.0 | |
| libpng, zlib (bundled with JUCE) | libpng / zlib licenses | |
| jpeglib (bundled with JUCE) | IJG license | This software is based in part on the work of the Independent JPEG Group. |

## Used at run time, not distributed

| Component | License | Notes |
|---|---|---|
| [AutoEq](https://github.com/jaakkopasanen/AutoEq) by Jaakko Pasanen | MIT | Headphone correction profiles are downloaded from the AutoEq repository when you search for your headphones. |

## Trademarks

VST and ASIO are trademarks of Steinberg Media Technologies GmbH. All other product names
mentioned in this project (e.g. APL Virtuoso, Supperware, Bridgehead, Waves, Abbey Road Studio 3,
dearVR, Sennheiser, FlexASIO, VB-Audio, BlackHole) belong to their respective owners and are used
only to describe compatibility. Head Tracking Host is not affiliated with or endorsed by any of them.
