// Head Tracking Host
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hosts a VST3 plugin (e.g. APL Virtuoso) and turns three of its parameters (yaw, pitch, roll)
// with head tracking from Bridgehead (profile "IEM Scene Rotator YPR"). Driving the plugin's own knobs
// avoids the crackling some renderers produce on their OSC head tracking input.

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_osc/juce_osc.h>

#include <array>
#include <atomic>
#include <cmath>
#include <complex>

using namespace juce;

#if JUCE_MAC
void requestMicrophoneAccess (std::function<void (bool)> callback); // MicrophonePermission.mm
#endif

namespace
{
constexpr int numEqSlots = 5;
constexpr int defaultOscPort = 9000; // Bridgehead profile "IEM Scene Rotator YPR"
constexpr int minWidth = 900;
constexpr int barHeight = 268;
const String autoEqBase ("https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/");
const std::array<const char*, 3> axisNames { "Yaw", "Pitch", "Roll" };
const std::array<float, 3> fallbackRange { 180.0f, 90.0f, 90.0f }; // +/- degrees if the plugin text can't be parsed
const std::array<bool, 3> defaultInvert { false, false, false };

const Colour green (0xff1f9d55);
const Colour amber (0xffd4a017);
const Colour red   (0xffd64545);
const Colour grey  (0xff8a8a8a);
const Colour panel (0xff262626);

struct InputLayout
{
    const char* name;
    AudioChannelSet set;
};

std::vector<InputLayout> inputLayouts()
{
    return { { "7.1 (8 channels)",    AudioChannelSet::create7point1() },
             { "7.1.4 (12 channels)", AudioChannelSet::create7point1point4() },
             { "5.1 (6 channels)",    AudioChannelSet::create5point1() },
             { "Stereo (2 channels)", AudioChannelSet::stereo() } };
}

// Portable: the settings file lives next to the program (EXE on Windows, .app on the Mac), so
// copying the folder takes all settings along. Falls back to the user folder if that place is
// not writable (e.g. a protected folder, or macOS running an unsigned app from a hidden
// read-only "AppTranslocation" location until it has been moved once).
std::unique_ptr<PropertiesFile> openSettings()
{
    PropertiesFile::Options o;
    o.applicationName     = "Head Tracking Host";
    o.folderName          = "Head Tracking Host";
    o.filenameSuffix      = ".settings";
    o.osxLibrarySubFolder = "Application Support";

    const auto userFile = o.getDefaultFile();
    const auto app = File::getSpecialLocation (File::currentApplicationFile);
    const auto folder = app.getParentDirectory();
    const auto portableFile = folder.getChildFile ("Head Tracking Host.settings");

    const bool translocated = app.getFullPathName().contains ("/AppTranslocation/");
    if (! translocated && folder.hasWriteAccess())
    {
        if (! portableFile.existsAsFile() && userFile.existsAsFile())
            userFile.copyFileTo (portableFile); // take over existing settings once
        return std::make_unique<PropertiesFile> (portableFile, o);
    }
    return std::make_unique<PropertiesFile> (o);
}

Array<File> pluginFolders()
{
    Array<File> roots;
   #if JUCE_WINDOWS
    roots.add (File ("C:\\Program Files\\Common Files\\VST3"));
   #elif JUCE_MAC
    roots.add (File ("/Library/Audio/Plug-Ins/VST3"));
    roots.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"));
   #else
    roots.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3"));
    roots.add (File ("/usr/lib/vst3"));
   #endif
    return roots;
}

// Lists installed VST3 plugins by file name only - nothing is loaded, so this is fast and safe.
Array<File> findInstalledPlugins()
{
    Array<File> result;
    for (auto& root : pluginFolders())
    {
        if (! root.isDirectory())
            continue;
        for (const auto& entry : RangedDirectoryIterator (root, true, "*.vst3", File::findFilesAndDirectories))
        {
            auto f = entry.getFile();
            // Skip files inside another .vst3 bundle.
            if (f.getParentDirectory().getFullPathName().containsIgnoreCase (".vst3"))
                continue;
            result.addIfNotAlreadyThere (f);
        }
    }
    std::sort (result.begin(), result.end(),
               [] (const File& a, const File& b) { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });
    return result;
}

// Returns the loopback "family" of a virtual audio device (input and output side of the same
// cable have different names, e.g. "CABLE Output" / "CABLE Input"), or an empty string.
String loopbackFamily (const String& name)
{
    for (auto* word : { "blackhole", "loopback", "soundflower", "vb-audio", "cable", "voicemeeter", "virtual" })
        if (name.containsIgnoreCase (word))
            return word;
    return {};
}

bool sameLoopback (const String& input, const String& output)
{
    const auto family = loopbackFamily (input);
    return family.isNotEmpty() && family == loopbackFamily (output);
}

// FlexASIO keeps its input/output devices in %USERPROFILE%\FlexASIO.toml.
std::pair<String, String> readFlexAsioDevices()
{
    const auto file = File::getSpecialLocation (File::userHomeDirectory).getChildFile ("FlexASIO.toml");
    String section, input, output;
    for (auto line : StringArray::fromLines (file.loadFileAsString()))
    {
        line = line.trim();
        if (line.startsWith ("["))
            section = line.removeCharacters ("[] ").toLowerCase();
        else if (line.startsWithIgnoreCase ("device") && line.contains ("="))
        {
            const auto value = line.fromFirstOccurrenceOf ("=", false, false).trim().unquoted();
            if (section == "input")  input = value;
            if (section == "output") output = value;
        }
    }
    return { input, output };
}

float parseDegrees (const String& text)
{
    return text.retainCharacters ("-0123456789.").getFloatValue();
}
} // namespace

//==============================================================================
// AutoEq headphone correction: parses AutoEq "ParametricEQ.txt" profiles and runs them as biquads.
struct EqBand
{
    enum Type { peak, lowShelf, highShelf };
    Type type = peak;
    double freq = 1000.0, gainDb = 0.0, q = 0.707;
};

struct EqProfile
{
    double preampDb = 0.0;
    std::vector<EqBand> bands;
    bool isEmpty() const { return bands.empty() && std::abs (preampDb) < 1.0e-9; }
};

// "Preamp: -3.1 dB" / "Filter 1: ON PK Fc 156 Hz Gain -5.2 dB Q 0.73" (also LSC/HSC).
EqProfile parseAutoEq (const String& text)
{
    EqProfile profile;
    for (auto line : StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.startsWithIgnoreCase ("Preamp:"))
        {
            profile.preampDb = line.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
            continue;
        }
        if (! line.startsWithIgnoreCase ("Filter") || ! line.containsIgnoreCase (" ON "))
            continue;

        auto tokens = StringArray::fromTokens (line.fromFirstOccurrenceOf (":", false, false), " ", {});
        tokens.removeEmptyStrings();
        EqBand band;
        const auto type = tokens[1].toUpperCase();
        if (type == "PK" || type == "PEQ")              band.type = EqBand::peak;
        else if (type == "LSC" || type == "LS")         band.type = EqBand::lowShelf;
        else if (type == "HSC" || type == "HS")         band.type = EqBand::highShelf;
        else continue;                                  // unsupported filter types are skipped

        for (int i = 0; i + 1 < tokens.size(); ++i)
        {
            if (tokens[i] == "Fc")        band.freq   = tokens[i + 1].getDoubleValue();
            else if (tokens[i] == "Gain") band.gainDb = tokens[i + 1].getDoubleValue();
            else if (tokens[i] == "Q")    band.q      = tokens[i + 1].getDoubleValue();
        }
        if (band.freq > 0.0 && band.q > 0.0)
            profile.bands.push_back (band);
    }
    return profile;
}

class HeadphoneEq
{
public:
    static constexpr int maxBands = 24;

    bool isActive() const { return enabled; }

    // Called from the message thread.
    void setProfile (const EqProfile& p, bool enable)
    {
        const SpinLock::ScopedLockType sl (lock);
        profile = p;
        enabled = enable && ! p.isEmpty();
        updateCoefficients();
    }

    void prepare (double newSampleRate)
    {
        const SpinLock::ScopedLockType sl (lock);
        sampleRate = newSampleRate;
        updateCoefficients();
    }

    // Audio thread: first two output channels (left/right).
    void process (float* const* outputs, int numOutputs, int numSamples)
    {
        const SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked() || ! enabled)
            return;
        for (int ch = 0; ch < jmin (2, numOutputs); ++ch)
        {
            auto* d = outputs[ch];
            if (d == nullptr)
                continue;
            for (int i = 0; i < numSamples; ++i)
            {
                double x = d[i] * preampGain;
                for (int b = 0; b < numBands; ++b)
                {
                    auto& c = coeffs[(size_t) b];
                    auto& st = state[(size_t) ch][(size_t) b];
                    const double y = c.b0 * x + st.z1;
                    st.z1 = c.b1 * x - c.a1 * y + st.z2;
                    st.z2 = c.b2 * x - c.a2 * y;
                    x = y;
                }
                d[i] = (float) x;
            }
        }
    }

private:
    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State  { double z1 = 0, z2 = 0; };

    // RBJ cookbook biquads (as used by Equalizer APO for AutoEq profiles).
    void updateCoefficients()
    {
        numBands = jmin ((int) profile.bands.size(), maxBands);
        preampGain = std::pow (10.0, profile.preampDb / 20.0);
        for (int b = 0; b < numBands; ++b)
        {
            const auto& band = profile.bands[(size_t) b];
            const double A = std::pow (10.0, band.gainDb / 40.0);
            const double w0 = MathConstants<double>::twoPi * jlimit (10.0, sampleRate * 0.49, band.freq) / sampleRate;
            const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * band.q);
            double b0, b1, b2, a0, a1, a2;
            if (band.type == EqBand::peak)
            {
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            }
            else
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                const double sign = band.type == EqBand::lowShelf ? 1.0 : -1.0;
                b0 =        A * ((A + 1) - sign * (A - 1) * cw + sq);
                b1 = sign * 2 * A * ((A - 1) - sign * (A + 1) * cw);
                b2 =        A * ((A + 1) - sign * (A - 1) * cw - sq);
                a0 =            (A + 1) + sign * (A - 1) * cw + sq;
                a1 = -sign * 2 * ((A - 1) + sign * (A + 1) * cw);
                a2 =            (A + 1) + sign * (A - 1) * cw - sq;
            }
            coeffs[(size_t) b] = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        }
        for (auto& ch : state)
            for (auto& st : ch)
                st = {};
    }

    SpinLock lock;
    EqProfile profile;
    bool enabled = false;
    double sampleRate = 48000.0, preampGain = 1.0;
    int numBands = 0;
    std::array<Coeffs, maxBands> coeffs {};
    std::array<std::array<State, maxBands>, 2> state {};
};

// Magnitude response of a profile's bands in dB (preamp excluded), for drawing the curve.
double eqResponseDb (const EqProfile& profile, double freq, double fs = 48000.0)
{
    double totalDb = 0.0;
    const std::complex<double> z1 = std::polar (1.0, -MathConstants<double>::twoPi * freq / fs);
    const auto z2 = z1 * z1;
    for (const auto& band : profile.bands)
    {
        const double A = std::pow (10.0, band.gainDb / 40.0);
        const double w0 = MathConstants<double>::twoPi * jlimit (10.0, fs * 0.49, band.freq) / fs;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * band.q);
        double b0, b1, b2, a0, a1, a2;
        if (band.type == EqBand::peak)
        {
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        }
        else
        {
            const double sq = 2 * std::sqrt (A) * alpha;
            const double sign = band.type == EqBand::lowShelf ? 1.0 : -1.0;
            b0 =        A * ((A + 1) - sign * (A - 1) * cw + sq);
            b1 = sign * 2 * A * ((A - 1) - sign * (A + 1) * cw);
            b2 =        A * ((A + 1) - sign * (A - 1) * cw - sq);
            a0 =            (A + 1) + sign * (A - 1) * cw + sq;
            a1 = -sign * 2 * ((A - 1) + sign * (A + 1) * cw);
            a2 =            (A + 1) + sign * (A - 1) * cw - sq;
        }
        const auto h = (b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2);
        totalDb += 20.0 * std::log10 (jmax (1.0e-9, std::abs (h)));
    }
    return totalDb;
}

class EqCurve : public Component
{
public:
    void setProfile (const EqProfile& p) { profile = p; repaint(); }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colour (0xff1b1b1b));
        g.fillRoundedRectangle (r, 6.0f);
        auto plot = r.reduced (34.0f, 16.0f).withTrimmedBottom (10.0f);
        const float range = 12.0f;
        auto xFor = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log10 (f / 20.0) / 3.0); };
        auto yFor = [&] (double db) { return plot.getCentreY() - plot.getHeight() * 0.5f * (float) (jlimit (-range, range, (float) db) / range); };

        g.setFont (FontOptions (11.0f));
        for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            g.setColour (Colours::white.withAlpha (0.08f));
            g.drawVerticalLine ((int) xFor (f), plot.getY(), plot.getBottom());
            g.setColour (Colours::grey);
            g.drawText (f >= 1000.0 ? String ((int) (f / 1000)) + "k" : String ((int) f),
                        (int) xFor (f) - 16, (int) plot.getBottom() + 2, 32, 14, Justification::centred);
        }
        for (double db : { -12.0, -6.0, 0.0, 6.0, 12.0 })
        {
            g.setColour (Colours::white.withAlpha (std::abs (db) < 0.01 ? 0.25f : 0.08f));
            g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());
            g.setColour (Colours::grey);
            g.drawText ((db > 0 ? "+" : "") + String ((int) db), 2, (int) yFor (db) - 7, 28, 14, Justification::centredRight);
        }

        if (profile.bands.empty())
        {
            g.setColour (Colours::grey);
            g.setFont (FontOptions (14.0f));
            g.drawText ("Empty slot", plot, Justification::centred);
            return;
        }

        Path curve;
        for (int i = 0; i <= 300; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 300.0);
            const auto pt = Point<float> (xFor (f), yFor (eqResponseDb (profile, f)));
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour (Colour (0xff4aa3df));
        g.strokePath (curve, PathStrokeType (2.0f));
    }

private:
    EqProfile profile;
};

//==============================================================================
// Apple-style palette (dark appearance).
namespace ui
{
const Colour window    (0xff1e1e1e);
const Colour group     (0xff2a2a2a);
const Colour separator (0xff3a3a3c);
const Colour label     (0xfff2f2f2);
const Colour secondary (0xffa1a1a6);
const Colour accent    (0xff0a84ff);
const Colour green     (0xff30d158);
const Colour yellow    (0xffffd60a);
const Colour red       (0xffff453a);
}

// Twelve input level meters (green, yellow, red), fed with peak values from the audio thread.
class LevelMeters : public Component, private Timer
{
public:
    static constexpr int numMeters = 12;
    std::function<float (int)> takePeak;   // returns and resets the latest peak for a channel
    StringArray names;                     // names of the active channels

    LevelMeters() { startTimerHz (30); }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float labelH = 14.0f, gap = 4.0f;
        const float barW = (r.getWidth() - gap * (numMeters - 1)) / numMeters;
        auto bars = r.withTrimmedBottom (labelH + 2.0f);
        for (int i = 0; i < numMeters; ++i)
        {
            const float x = r.getX() + i * (barW + gap);
            const bool active = i < names.size();
            Rectangle<float> bar (x, bars.getY(), barW, bars.getHeight());
            g.setColour (ui::separator.withAlpha (active ? 1.0f : 0.4f));
            g.fillRoundedRectangle (bar, 2.0f);
            if (active && display[(size_t) i] > 0.0f)
            {
                // -60 dB .. 0 dB
                const float db = Decibels::gainToDecibels (display[(size_t) i], -60.0f);
                const float level = jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
                auto fill = bar.withTop (bar.getBottom() - bar.getHeight() * level);
                g.setColour (ui::accent);
                g.fillRoundedRectangle (fill, 2.0f);
            }
            g.setColour (active ? ui::secondary : ui::secondary.withAlpha (0.35f));
            g.setFont (FontOptions (10.0f));
            g.drawText (active ? names[i] : String (i + 1), (int) x - 4, (int) (r.getBottom() - labelH),
                        (int) barW + 8, (int) labelH, Justification::centred);
        }
    }

private:
    void timerCallback() override
    {
        if (! takePeak || ! isShowing())
            return;
        for (int i = 0; i < numMeters; ++i)
            display[(size_t) i] = jmax (takePeak (i), display[(size_t) i] * 0.93f); // slow, calm release
        repaint();
    }

    std::array<float, numMeters> display {};
};

// Top view of the head that turns with the tracked yaw, plus the three angles.
class HeadIndicator : public Component, private Timer
{
public:
    std::function<std::array<float, 3>()> getAngles;   // yaw, pitch, roll in degrees

    HeadIndicator() { startTimerHz (30); }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto circleArea = r.removeFromLeft (r.getHeight()).reduced (6.0f);
        const auto c = circleArea.getCentre();
        const float radius = circleArea.getWidth() * 0.5f;

        g.setColour (ui::separator);
        g.drawEllipse (circleArea, 1.0f);
        g.setColour (ui::secondary.withAlpha (0.6f));
        g.setFont (FontOptions (9.0f));
        g.drawText ("front", (int) (c.x - 20), (int) circleArea.getY() - 1, 40, 10, Justification::centred);

        // head: circle + nose, rotated by yaw (positive yaw = turned left)
        const float angle = degreesToRadians (angles[0]);
        Path head;
        head.addEllipse (-radius * 0.40f, -radius * 0.46f, radius * 0.80f, radius * 0.92f);
        Path nose;
        nose.addTriangle (-radius * 0.16f, -radius * 0.40f, radius * 0.16f, -radius * 0.40f, 0.0f, -radius * 0.72f);
        const auto t = AffineTransform::rotation (angle).translated (c);
        head.applyTransform (t);
        nose.applyTransform (t);
        g.setColour (ui::accent);
        g.fillPath (head);
        g.setColour (ui::label);
        g.fillPath (nose);

        g.setColour (ui::label);
        g.setFont (FontOptions (12.0f));
        auto text = r.reduced (10.0f, 0.0f);
        const char* names[] = { "Yaw", "Pitch", "Roll" };
        for (int i = 0; i < 3; ++i)
        {
            auto line = text.removeFromTop (text.getHeight() / (float) (3 - i));
            g.setColour (ui::secondary);
            g.drawText (names[i], line.removeFromLeft (44.0f), Justification::centredLeft);
            g.setColour (ui::label);
            g.drawText (String (angles[(size_t) i], 0) + String::fromUTF8 ("\xc2\xb0"), line, Justification::centredLeft);
        }
    }

private:
    void timerCallback() override
    {
        if (getAngles && isShowing())
        {
            angles = getAngles();
            repaint();
        }
    }

    std::array<float, 3> angles {};
};

// Scrollable sidebar content; the owner paints the grouped sections.
struct SidebarContent : public Component
{
    std::function<void (Graphics&)> onPaint;
    void paint (Graphics& g) override { if (onPaint) onPaint (g); }
};

//==============================================================================
// Runs the plugin on the audio thread.
class HostEngine : public AudioIODeviceCallback
{
public:
    void setPlugin (AudioPluginInstance* p) { plugin = p; }
    void setMuted (bool m) { muted = m; }
    static constexpr int maxChannels = 32;
    // Which device input channel feeds each plugin input channel (-1 = silent).
    // Speaker simulation off (or no plugin): the routed input is downmixed to stereo ("dry").
    void setSpeakerSimulation (bool on) { speakerSimulation = on; }
    void setDryChannels (int n) { dryChannels = jlimit (1, maxChannels, n); }
    HeadphoneEq& getHeadphoneEq() { return headphoneEq; }

    // Stereo downmix gains per plugin input channel, used while bypassed (set while stopped).
    void setDownmixGains (const std::vector<std::pair<float, float>>& gains)
    {
        for (int i = 0; i < maxChannels; ++i)
            downmix[(size_t) i] = i < (int) gains.size() ? gains[(size_t) i] : std::pair<float, float> { 0.0f, 0.0f };
    }

    float takeInputPeak (int ch) { return isPositiveAndBelow (ch, 12) ? inputPeaks[(size_t) ch].exchange (0.0f) : 0.0f; }

    void setInputSource (int pluginChannel, int deviceChannel)
    {
        if (isPositiveAndBelow (pluginChannel, maxChannels))
            inputMap[(size_t) pluginChannel] = deviceChannel;
    }

    // An extra device channel mixed in at -3 dB (-1 = none), e.g. the 7.1 rear channels into the
    // surround channels when a plugin only accepts 5.1.
    void setFoldSource (int pluginChannel, int deviceChannel)
    {
        if (isPositiveAndBelow (pluginChannel, maxChannels))
            foldMap[(size_t) pluginChannel] = deviceChannel;
    }

    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        headphoneEq.prepare (device->getCurrentSampleRate());
        sampleRate = device->getCurrentSampleRate();
        const int blockSize = device->getCurrentBufferSizeSamples();

        // Soft start after every device (re)start: short silence, then fade in.
        silenceSamples = (int) (0.15 * sampleRate);
        fadeSamples    = (int) (2.0 * sampleRate);  // 2 s fade-in
        fadePosition   = 0;

        const int deviceChannels = jmax (device->getActiveInputChannels().countNumberOfSetBits(),
                                         device->getActiveOutputChannels().countNumberOfSetBits());
        int channels = jmax (deviceChannels, 2, dryChannels.load());

        if (plugin != nullptr)
        {
            plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
            plugin->prepareToPlay (sampleRate, blockSize);
            plugin->reset();
            channels = jmax (channels, jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()));
        }

        buffer.setSize (channels, jmax (blockSize, 4096));
        buffer.clear();
        midi.ensureSize (256);
    }

    // Watchdog support: the audio callback counts its calls; device errors are flagged.
    uint32 getCallbackCount() const   { return callbackCount.load(); }
    bool takeDeviceError()            { return deviceError.exchange (false); }
    void audioDeviceError (const String&) override { deviceError = true; }

    void audioDeviceStopped() override
    {
        if (plugin != nullptr)
            plugin->releaseResources();
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const AudioIODeviceCallbackContext&) override
    {
        callbackCount.fetch_add (1);
        auto* p = plugin;
        if (muted || numSamples > buffer.getNumSamples())
        {
            clearOutputs (outputs, numOutputs, numSamples);
            return;
        }

        const bool usePlugin = p != nullptr && speakerSimulation;
        const int pluginIns  = p != nullptr ? p->getTotalNumInputChannels()  : dryChannels.load();
        const int pluginOuts = p != nullptr ? p->getTotalNumOutputChannels() : 2;
        const int channels   = jmin (buffer.getNumChannels(), jmax (pluginIns, pluginOuts));

        AudioBuffer<float> block (buffer.getArrayOfWritePointers(), channels, numSamples);

        for (int ch = 0; ch < channels; ++ch)
        {
            const int src = ch < maxChannels ? inputMap[(size_t) ch].load() : ch;

            if (isPositiveAndBelow (src, numInputs) && ch < pluginIns && inputs[src] != nullptr)
                FloatVectorOperations::copy (block.getWritePointer (ch), inputs[src], numSamples);
            else
                block.clear (ch, 0, numSamples);

            const int fold = ch < maxChannels ? foldMap[(size_t) ch].load() : -1;
            if (isPositiveAndBelow (fold, numInputs) && ch < pluginIns && inputs[fold] != nullptr)
                FloatVectorOperations::addWithMultiply (block.getWritePointer (ch), inputs[fold], 0.7071f, numSamples);
        }

        // Input meters: peak per routed plugin input channel.
        for (int ch = 0; ch < jmin (12, pluginIns, channels); ++ch)
        {
            const float peak = block.getMagnitude (ch, 0, numSamples);
            if (peak > inputPeaks[(size_t) ch].load())
                inputPeaks[(size_t) ch] = peak;
        }

        if (! usePlugin)
        {
            // Dry signal (speaker simulation off or no plugin): the routed input, downmixed to stereo.
            writeDownmix (block, jmin (channels, pluginIns), outputs, numOutputs, numSamples);
            headphoneEq.process (outputs, numOutputs, numSamples); // headphone correction works on its own too
            applySoftStart (outputs, numOutputs, numSamples);
            return;
        }

        midi.clear();
        p->processBlock (block, midi);

        // Safety: never pass NaN/inf or extreme values to the headphones.
        for (int ch = 0; ch < jmin (channels, pluginOuts); ++ch)
        {
            auto* d = block.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = std::isfinite (d[i]) ? jlimit (-2.0f, 2.0f, d[i]) : 0.0f;
        }

        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            if (ch < pluginOuts && ch < channels)
                FloatVectorOperations::copy (outputs[ch], block.getReadPointer (ch), numSamples);
            else
                FloatVectorOperations::clear (outputs[ch], numSamples);
        }

        headphoneEq.process (outputs, numOutputs, numSamples);
        applySoftStart (outputs, numOutputs, numSamples);
    }

private:
    void writeDownmix (const AudioBuffer<float>& in, int numIn, float* const* outputs, int numOutputs, int numSamples)
    {
        clearOutputs (outputs, numOutputs, numSamples);
        for (int side = 0; side < jmin (2, numOutputs); ++side)
        {
            if (outputs[side] == nullptr)
                continue;
            for (int ch = 0; ch < numIn; ++ch)
            {
                const auto& g = downmix[(size_t) ch];
                const float gain = side == 0 ? g.first : g.second;
                if (std::abs (gain) > 0.0f)
                    FloatVectorOperations::addWithMultiply (outputs[side], in.getReadPointer (ch), gain, numSamples);
            }
            for (int i = 0; i < numSamples; ++i)
                outputs[side][i] = jlimit (-2.0f, 2.0f, outputs[side][i]);
        }
    }

    static void clearOutputs (float* const* outputs, int numOutputs, int numSamples)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                FloatVectorOperations::clear (outputs[ch], numSamples);
    }

    void applySoftStart (float* const* outputs, int numOutputs, int numSamples)
    {
        const int end = silenceSamples + fadeSamples;
        if (fadePosition >= end)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const int pos = fadePosition + i;
            const float gain = pos < silenceSamples ? 0.0f
                                                    : jmin (1.0f, (float) (pos - silenceSamples) / (float) fadeSamples);
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch] != nullptr)
                    outputs[ch][i] *= gain;
        }
        fadePosition += numSamples;
    }

    AudioPluginInstance* plugin = nullptr;
    std::atomic<bool> muted { false }, speakerSimulation { true }, deviceError { false };
    std::atomic<int> dryChannels { 8 };
    std::array<std::atomic<float>, 12> inputPeaks {};
    HeadphoneEq headphoneEq;
    std::atomic<uint32> callbackCount { 0 };
    std::array<std::pair<float, float>, maxChannels> downmix {};
    struct InputMap
    {
        InputMap() { for (int i = 0; i < maxChannels; ++i) channels[(size_t) i] = i; }
        std::atomic<int>& operator[] (size_t i) { return channels[i]; }
        std::array<std::atomic<int>, maxChannels> channels;
    };
    InputMap inputMap;
    struct FoldMap
    {
        FoldMap() { for (auto& c : channels) c = -1; }
        std::atomic<int>& operator[] (size_t i) { return channels[i]; }
        std::array<std::atomic<int>, maxChannels> channels;
    } foldMap;
    AudioBuffer<float> buffer;
    MidiBuffer midi;
    double sampleRate = 48000.0;
    int silenceSamples = 0, fadeSamples = 1, fadePosition = 0;
};

//==============================================================================
// Keeps a top-level window (including the system title bar and frame) inside the visible screen
// area: shrinks it if needed and moves it back on screen. The content scrolls instead.
void fitWindowToScreen (Component& window)
{
    if (! window.isOnDesktop())
        return;
    const auto& displays = Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect (window.getScreenBounds());
    auto usable = (display != nullptr ? display : displays.getPrimaryDisplay())->userArea;
    if (auto* peer = window.getPeer())
        if (const auto frame = peer->getFrameSizeIfPresent())
            usable = frame->subtractedFrom (usable);   // room for the native title bar and frame

    auto b = window.getBounds();
    b.setSize (jmin (b.getWidth(), usable.getWidth()), jmin (b.getHeight(), usable.getHeight()));
    b = b.constrainedWithin (usable);
    if (b != window.getBounds())
        window.setBounds (b);
}

//==============================================================================
class MainComponent : public Component,
                      private OSCReceiver::Listener<OSCReceiver::MessageLoopCallback>,
                      private ComponentListener,
                      private ChangeListener,
                      private Timer
{
public:
    MainComponent()
    {
        settings = openSettings();
        addDefaultFormatsToManager (formatManager);

        // Startup crash guard: if the previous start never finished, start without opening the
        // saved audio device and plugin. The flag is written to disk right away and cleared once
        // the app has been running for a few seconds (and on a normal quit).
        safeMode = settings->getBoolValue ("startupPending", false);
        settings->setValue ("startupPending", true);
        settings->saveIfNeeded();
        setSize (minWidth, barHeight + 360);

        sidebar.addAndMakeVisible (statusDot);
        sidebar.addAndMakeVisible (statusLabel);
        sidebar.addAndMakeVisible (detailLabel);
        statusLabel.setFont (FontOptions (14.0f, Font::bold));
        statusLabel.setColour (Label::textColourId, ui::label);
        detailLabel.setFont (FontOptions (12.0f));
        detailLabel.setColour (Label::textColourId, ui::secondary);
        detailLabel.setJustificationType (Justification::topLeft);

        // Row 2: plugin, audio, port, input layout
        // (plugin label replaced by the card caption)
        sidebar.addAndMakeVisible (pluginBox);
        pluginBox.setTextWhenNothingSelected ("Choose a plugin...");
        pluginBox.onChange = [this] { pluginBoxChanged(); };
        sidebar.addAndMakeVisible (browseButton);
        browseButton.onClick = [this] { browseForPlugin(); };
        sidebar.addAndMakeVisible (audioButton);
        audioButton.onClick = [this] { showAudioSettings(); };

        // (portLabel drawn by the sidebar)
        sidebar.addAndMakeVisible (portEditor);
        portEditor.setInputRestrictions (5, "0123456789");
        portEditor.setJustification (Justification::centred);
        // One-time switch from the earlier default (port 8000, DaVinci Resolve profile) to IEM (9000).
        if (! settings->getBoolValue ("iemDefaults", false))
        {
            if (settings->getIntValue ("oscPort", 8000) == 8000)
                settings->setValue ("oscPort", defaultOscPort);
            settings->setValue ("iemDefaults", true);
        }
        portEditor.setText (String (settings->getIntValue ("oscPort", defaultOscPort)));

        // One-time: "Invert" now means inverted relative to the normal (IEM/Virtuoso) orientation.
        // Older versions stored pitch relative to the tracker's raw angles, i.e. flipped.
        if (! settings->getBoolValue ("pitchConvention2", false))
        {
            for (auto& key : settings->getAllProperties().getAllKeys())
                if (key.endsWith (".invertPitch"))
                    settings->setValue (key, ! settings->getBoolValue (key));
            for (auto& key : settings->getAllProperties().getAllKeys())
                if (key.endsWith (".paramPitch") || key.endsWith (".state"))
                {
                    const auto prefix = key.upToLastOccurrenceOf (".", false, false);
                    if (! settings->containsKey (prefix + ".invertPitch"))
                        settings->setValue (prefix + ".invertPitch", false); // old default (on) -> off
                }
            settings->setValue ("pitchConvention2", true);
        }
        portEditor.onReturnKey = portEditor.onFocusLost = [this] { applyPort(); };

        // (layoutLabel drawn by the sidebar)
        sidebar.addAndMakeVisible (layoutBox);
        int id = 1;
        for (auto& l : inputLayouts())
            layoutBox.addItem (l.name, id++);
        layoutBox.setSelectedItemIndex (jlimit (0, layoutBox.getNumItems() - 1, settings->getIntValue ("layout", 0)),
                                        dontSendNotification);
        layoutBox.onChange = [this] { settings->setValue ("layout", layoutBox.getSelectedItemIndex()); applyLayout(); };

        sidebar.addAndMakeVisible (speakerSimButton);
        speakerSimButton.getProperties().set ("switch", true);
        speakerSimButton.setTooltip ("Off: hear the input without the plugin (surround is downmixed to stereo) - e.g. for A/B comparisons.");
        speakerSimButton.setToggleState (settings->getBoolValue ("speakerSim", true), dontSendNotification);
        engine.setSpeakerSimulation (speakerSimButton.getToggleState());
        speakerSimButton.onClick = [this]
        {
            engine.setSpeakerSimulation (speakerSimButton.getToggleState());
            settings->setValue ("speakerSim", speakerSimButton.getToggleState());
        };

        sidebar.addAndMakeVisible (channelsButton);
        channelsButton.onClick = [this] { showChannelRouting(); };
        channelsButton.setTooltip ("Choose which audio input feeds each plugin channel.");
        loadInputMap();

        // Headphone correction (AutoEq profiles; global, works with or without speaker simulation)
        sidebar.addAndMakeVisible (autoEqButton);
        autoEqButton.getProperties().set ("switch", true);
        autoEqButton.setTooltip ("AutoEq headphone correction - works with or without speaker simulation.");
        autoEqButton.setToggleState (settings->getBoolValue ("autoeqEnabled", false), dontSendNotification);
        autoEqButton.onClick = [this] { settings->setValue ("autoeqEnabled", autoEqButton.getToggleState()); applyAutoEq(); };
        sidebar.addAndMakeVisible (autoEqSlotBox);
        refreshAutoEqSlots();
        autoEqSlotBox.setSelectedItemIndex (jlimit (0, numEqSlots - 1, settings->getIntValue ("autoeqSlot", 0)), dontSendNotification);
        autoEqSlotBox.onChange = [this]
        {
            settings->setValue ("autoeqSlot", autoEqSlotBox.getSelectedItemIndex());
            applyAutoEq();
            notifyHeadphoneWindow();
        };
        sidebar.addAndMakeVisible (autoEqSearchButton);
        autoEqSearchButton.onClick = [this] { showHeadphoneWindow(); };
        autoEqSearchButton.setTooltip ("Headphone correction: slots, curve, database search and file import.");
        applyAutoEq();
        updateDownmix(); // dry path is ready even before a plugin is loaded

        sidebar.addAndMakeVisible (trackingButton);
        trackingButton.getProperties().set ("switch", true);
        trackingButton.setTooltip ("Off: stops listening to Bridgehead, frees the port and sets Yaw/Pitch/Roll to 0 degrees.");
        trackingButton.setToggleState (settings->getBoolValue ("headTracking", true), dontSendNotification);
        trackingButton.onClick = [this] { setHeadTracking (trackingButton.getToggleState()); };

        // Row 3: which plugin parameter is yaw / pitch / roll
        for (size_t i = 0; i < 3; ++i)
        {
            axisLabels[i].setText (String (axisNames[i]) + ":", dontSendNotification);
            axisLabels[i].setJustificationType (Justification::centredRight);
            // (axisLabels[i] drawn by the sidebar)

            axisBoxes[i].setTextWhenNothingSelected ("- none -");
            axisBoxes[i].onChange = [this, i] { axisBoxChanged (i); };
            sidebar.addAndMakeVisible (axisBoxes[i]);

            invertButtons[i].setButtonText ("Invert");
            invertButtons[i].onClick = [this, i] { settings->setValue (pluginKey (String ("invert") + axisNames[i]),
                                                                       invertButtons[i].getToggleState()); };
            sidebar.addAndMakeVisible (invertButtons[i]);
        }

        // Plugin area: scrolls when the window is smaller than the plugin, centres it otherwise.
        addAndMakeVisible (viewport);
        viewport.setScrollBarsShown (true, true, true, true);
        viewport.setViewedComponent (&pluginHolder, false);

        // Sidebar (System Settings style), scrollable on small screens.
        addAndMakeVisible (sidebarViewport);
        sidebarViewport.setViewedComponent (&sidebar, false);
        sidebarViewport.setScrollBarsShown (true, false);
        sidebar.onPaint = [this] (Graphics& g) { paintSidebar (g); };

        sidebar.addAndMakeVisible (zoomBox);
        for (int z : { 75, 100, 125, 150 })
            zoomBox.addItem (String (z) + " %", z);
        zoomBox.setSelectedId (100, dontSendNotification);
        zoomBox.setTooltip ("Plugin zoom - works with plugins that support scaling.");
        zoomBox.onChange = [this] { applyZoom(); };

        sidebar.addAndMakeVisible (foldButton);
        foldButton.getProperties().set ("switch", true);
        foldButton.setToggleState (settings->getBoolValue ("mixRearInto51", true), dontSendNotification);
        foldButton.setTooltip ("When a plugin only accepts 5.1: mix the 7.1 rear channels into the surround channels "
                               "instead of leaving them out.");
        foldButton.onClick = [this]
        {
            settings->setValue ("mixRearInto51", foldButton.getToggleState());
            applyFold();
        };

        sidebar.addAndMakeVisible (indicatorButton);
        indicatorButton.getProperties().set ("switch", true);
        indicatorButton.setToggleState (settings->getBoolValue ("headIndicator", true), dontSendNotification);
        indicatorButton.onClick = [this]
        {
            settings->setValue ("headIndicator", indicatorButton.getToggleState());
            headIndicator.setVisible (indicatorButton.getToggleState());
            resized();
        };
        sidebar.addChildComponent (headIndicator);
        headIndicator.setVisible (indicatorButton.getToggleState());
        headIndicator.getAngles = [this] { return lastAngles; };

        sidebar.addAndMakeVisible (meters);
        meters.takePeak = [this] (int ch) { return engine.takeInputPeak (ch); };

        deviceManager.addChangeListener (this);
        deviceManager.addAudioCallback (&engine);
       #if JUCE_MAC
        // Ask for audio input access once, then open the devices (avoids repeated dialogs).
        requestMicrophoneAccess ([safe = Component::SafePointer<MainComponent> (this)] (bool granted)
                                 {
                                     if (safe != nullptr)
                                         safe->initialiseAudio (granted);
                                 });
       #else
        initialiseAudio (true);
       #endif

        receiver.addListener (this);
        applyPort();

        loadPluginCache();
        refreshPluginList();
        const File lastPlugin (settings->getValue ("pluginPath"));
        if (safeMode)
            pluginBox.setSelectedId (0, dontSendNotification); // choosing it again loads it
        else if (lastPlugin.exists())
            loadPlugin (lastPlugin, settings->getValue ("pluginName"));
        // Plugins are scanned once (first start) and afterwards only on "Rescan plugins", so
        // no plugin is opened in the background on a normal start.
        if (! safeMode && ! settings->getBoolValue ("pluginsScanned", false))
            startPluginScan (false);

        startTimerHz (2);
    }

    // Opens the last audio device (e.g. ASIO / FlexASIO or CoreAudio / aggregate device).
    void initialiseAudio (bool inputAllowed)
    {
        microphoneDenied = ! inputAllowed;
        // In safe mode the saved device is not opened (the system default is used instead).
        std::unique_ptr<XmlElement> savedAudio (safeMode ? nullptr : settings->getXmlValue ("audioDevice"));
        const auto audioError = deviceManager.initialise (inputAllowed ? 16 : 0, 16, savedAudio.get(), true);
        if (audioError.isNotEmpty())
            lastError = "Audio device: " + audioError;
        audioInitialised = true;
        audioStartTime = lastCallbackTime = Time::getMillisecondCounter();
        lastCallbackCount = engine.getCallbackCount();
        checkFeedback();
    }

    ~MainComponent() override
    {
        saveState();
        settings->setValue ("startupPending", false);
        settings->saveIfNeeded();
        deviceManager.removeChangeListener (this);
        receiver.removeListener (this);
        receiver.disconnect();
        unloadPlugin();
        deviceManager.removeAudioCallback (&engine);
    }

    void saveState()
    {
        // Only store the audio device once it has been opened (on the Mac that waits for the
        // microphone permission), so an early quit doesn't overwrite the saved device.
        if (audioInitialised && (! safeMode || audioSettingsOpened))
            if (auto xml = deviceManager.createStateXml())
                settings->setValue ("audioDevice", xml.get());
        if (plugin != nullptr)
        {
            MemoryBlock state;
            plugin->getStateInformation (state);
            settings->setValue (pluginKey ("state"), state.toBase64Encoding());
        }
        settings->saveIfNeeded();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (ui::window);
        g.setColour (ui::separator);
        g.drawVerticalLine (sidebarWidth, 0.0f, (float) getHeight());
    }

    void paintSidebar (Graphics& g)
    {
        g.fillAll (ui::window);
        for (auto& r : sidebarGroups)
        {
            g.setColour (ui::group);
            g.fillRoundedRectangle (r.toFloat(), 10.0f);
        }
        for (auto& t : sidebarTexts)
        {
            g.setColour (t.header ? ui::secondary : ui::label);
            g.setFont (t.header ? FontOptions (11.0f, Font::bold) : FontOptions (13.0f));
            g.drawText (t.text, t.area, Justification::centredLeft);
        }
        // row separators inside groups
        g.setColour (ui::separator);
        for (auto& r : rowSeparators)
            g.drawHorizontalLine (r.getY(), (float) r.getX(), (float) r.getRight());
    }

    // Lays out the sidebar as grouped rows: label left, control right.
    int layoutSidebar (int width)
    {
        sidebarTexts.clear();
        sidebarGroups.clear();
        rowSeparators.clear();
        const int pad = 14, rowH = 34, inset = 12;
        int y = 14;

        auto status = Rectangle<int> (pad, y, width - 2 * pad, 22);
        statusDot.setBounds (status.removeFromLeft (18));
        statusLabel.setBounds (status);
        y += 22;
        detailLabel.setBounds (pad, y, width - 2 * pad, 34);
        y += 42;

        Rectangle<int> group;
        auto beginGroup = [&] (const String& title)
        {
            sidebarTexts.push_back ({ { pad + 4, y, width - 2 * pad, 18 }, title, true });
            y += 20;
            group = { pad, y, width - 2 * pad, 0 };
        };
        auto endGroup = [&]
        {
            group.setBottom (y);
            sidebarGroups.push_back (group);
            y += 14;
        };
        auto row = [&] (const String& label, int labelWidth = 110) -> Rectangle<int>
        {
            if (y > group.getY())
                rowSeparators.push_back ({ group.getX() + inset, y, group.getWidth() - inset, 1 });
            auto r = Rectangle<int> (group.getX() + inset, y, group.getWidth() - 2 * inset, rowH);
            if (label.isNotEmpty())
                sidebarTexts.push_back ({ r.removeFromLeft (labelWidth), label, false });
            y += rowH;
            return r.reduced (0, 4);
        };
        auto switchArea = [] (Rectangle<int> r) { return r.removeFromRight (44); };

        beginGroup ("PLUGIN");
        pluginBox.setBounds (row (""));
        {
            auto r = row ("");
            browseButton.setBounds (r.removeFromLeft (r.getWidth() / 2 - 3));
            r.removeFromLeft (6);
            audioButton.setBounds (r);
        }
        layoutBox.setBounds (row ("Input"));
        channelsButton.setBounds (row ("Channels"));
        foldButton.setBounds (switchArea (row ("Mix 7.1 rear into 5.1", 200)));
        zoomBox.setBounds (row ("Plugin zoom"));
        endGroup();

        beginGroup ("HEAD TRACKING");
        trackingButton.setBounds (switchArea (row ("Head tracking", 200)));
        portEditor.setBounds (row ("Bridgehead port").removeFromRight (80));
        const char* axes[] = { "Yaw", "Pitch", "Roll" };
        for (size_t i = 0; i < 3; ++i)
        {
            auto r = row (axes[i], 48);
            invertButtons[i].setBounds (r.removeFromRight (70));
            r.removeFromRight (6);
            axisBoxes[i].setBounds (r);
        }
        indicatorButton.setBounds (switchArea (row ("Head indicator", 200)));
        if (indicatorButton.getToggleState())
        {
            headIndicator.setBounds (group.getX() + inset, y, group.getWidth() - 2 * inset, 92);
            y += 96;
        }
        endGroup();

        beginGroup ("OUTPUT");
        speakerSimButton.setBounds (switchArea (row ("Speaker simulation", 200)));
        autoEqButton.setBounds (switchArea (row ("Headphone correction", 200)));
        {
            auto r = row ("");
            autoEqSearchButton.setBounds (r.removeFromRight (64));
            r.removeFromRight (6);
            autoEqSlotBox.setBounds (r);
        }
        endGroup();

        beginGroup ("INPUT LEVELS");
        meters.names = pluginChannelNames();
        meters.setBounds (group.getX() + inset, y + 10, group.getWidth() - 2 * inset, 96);
        y += 116;
        endGroup();

        return y;
    }

    void layoutPluginArea()
    {
        const int w = jmax (viewport.getMaximumVisibleWidth(), editor != nullptr ? editor->getWidth() : 0);
        const int h = jmax (viewport.getMaximumVisibleHeight(), editor != nullptr ? editor->getHeight() : 0);
        pluginHolder.setSize (w, h);
        if (editor != nullptr)
            editor->setTopLeftPosition ((w - editor->getWidth()) / 2, (h - editor->getHeight()) / 2);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        sidebarViewport.setBounds (area.removeFromLeft (sidebarWidth));
        const int contentWidth = sidebarWidth - (sidebarViewport.isVerticalScrollBarShown() ? sidebarViewport.getScrollBarThickness() : 0);
        sidebar.setSize (contentWidth, layoutSidebar (contentWidth));
        area.removeFromLeft (1);
        viewport.setBounds (area);
        layoutPluginArea();
    }

private:
    //==============================================================================
    // Window size follows the plugin editor, limited to the screen.
    void updateSize()
    {
        const int wantW = sidebarWidth + 1 + (editor != nullptr ? editor->getWidth() : 600);
        const int wantH = jmax (680, editor != nullptr ? editor->getHeight() : 0);

        auto screen = Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        if (auto* top = getTopLevelComponent(); top != nullptr && top->isOnDesktop())
            if (auto* d = Desktop::getInstance().getDisplays().getDisplayForRect (top->getScreenBounds()))
                screen = d->userArea;

        // Grow to show the whole plugin, but automatically only up to 85 % of the screen (the user
        // can enlarge or maximise it); never shrink a window the user sized.
        const int capW = (int) (screen.getWidth() * 0.85), capH = (int) (screen.getHeight() * 0.85);
        setSize (jlimit (getWidth(), jmax (getWidth(), capW), wantW),
                 jlimit (getHeight(), jmax (getHeight(), capH), wantH));
        if (auto* top = getTopLevelComponent())
            fitWindowToScreen (*top);   // whole window incl. title bar stays on screen; content scrolls
        layoutPluginArea();
    }

    void componentMovedOrResized (Component& c, bool, bool wasResized) override
    {
        if (&c == editor.get() && wasResized)
        {
            updateSize();
            layoutPluginArea();
        }
    }

    //==============================================================================
    // Plugin selection
    String pluginKey (const String& name) const
    {
        // Plugins from a shell file (e.g. Waves' WaveShell) are stored under their own name.
        const auto sub = settings->getValue ("pluginName");
        const auto id = sub.isNotEmpty() ? sub : File (settings->getValue ("pluginPath")).getFileNameWithoutExtension();
        return "plugin." + id.removeCharacters (" .") + "." + name;
    }

    // One entry of the plugin list. A file can hold several plugins (a "shell", e.g. Waves'
    // WaveShell): each plugin inside it gets its own entry (name not empty).
    struct PluginEntry
    {
        File file; String name;
        bool operator== (const PluginEntry& o) const { return file == o.file && name == o.name; }
    };

    static constexpr int rescanItemId = 100000;

    // Plugins found by the last scan inside this file (empty if it was not scanned).
    Array<PluginDescription> typesInFile (const File& file) const
    {
        Array<PluginDescription> result;
        for (auto& t : knownPlugins.getTypes())
            if (File (t.fileOrIdentifier) == file)
                result.add (t);
        return result;
    }

    File deadMansPedalFile() const
    {
        return settings->getFile().getSiblingFile ("Head Tracking Host - plugin scan.txt");
    }

    void loadPluginCache()
    {
        if (auto xml = settings->getXmlValue ("knownPlugins"))
            knownPlugins.recreateFromXml (*xml);
    }

    void savePluginCache()
    {
        if (auto xml = knownPlugins.createXml())
            settings->setValue ("knownPlugins", xml.get());
        settings->saveIfNeeded();
    }

    // Scans the plugin folders like a DAW: every new or changed file is opened once and the
    // plugins inside it are remembered. One file per timer tick keeps the window responsive.
    // A plugin that crashes during the scan is skipped from then on (dead man's pedal).
    void startPluginScan (bool full)
    {
        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : formatManager.getFormats())
            if (f->getName() == "VST3")
                vst3 = f;
        if (vst3 == nullptr)
            return;
        if (full)
        {
            knownPlugins.clear();
            knownPlugins.clearBlacklistedFiles();
            deadMansPedalFile().deleteFile();
        }
        FileSearchPath path;
        for (auto& root : pluginFolders())
            if (root.isDirectory())
                path.add (root);
        pluginScanner = std::make_unique<PluginDirectoryScanner> (knownPlugins, *vst3, path, true,
                                                                  deadMansPedalFile(), false);
        scanTimer.callback = [this] { pluginScanStep(); };
        scanTimer.startTimer (15);
    }

    void pluginScanStep()
    {
        if (pluginScanner == nullptr)
            return scanTimer.stopTimer();
        scanningPluginName = pluginScanner->getNextPluginFileThatWillBeScanned();
        const int before = knownPlugins.getNumTypes();
        String name;
        const bool more = pluginScanner->scanNextFile (true, name);
        if (knownPlugins.getNumTypes() != before)
            savePluginCache();
        if (! more)
        {
            scanTimer.stopTimer();
            pluginScanner.reset();
            scanningPluginName = {};
            settings->setValue ("pluginsScanned", true);
            savePluginCache();
            refreshPluginList (true);
        }
    }

    // onlyIfChanged: keep the list untouched when nothing changed, so an open drop-down is not
    // rebuilt under the mouse (e.g. when the background scan finishes).
    void refreshPluginList (bool onlyIfChanged = false)
    {
        std::vector<PluginEntry> fresh;
        for (auto& f : findInstalledPlugins())
        {
            StringArray inside;
            for (auto& t : typesInFile (f))
                inside.addIfNotAlreadyThere (t.name);
            if (inside.size() <= 1)
                fresh.push_back ({ f, {} });
            else
                for (auto& n : inside)
                    fresh.push_back ({ f, n });
        }
        std::stable_sort (fresh.begin(), fresh.end(), [] (const PluginEntry& a, const PluginEntry& b)
        {
            auto label = [] (const PluginEntry& e) { return e.name.isNotEmpty() ? e.name : e.file.getFileNameWithoutExtension(); };
            return label (a).compareIgnoreCase (label (b)) < 0;
        });
        if (onlyIfChanged && fresh == installedPlugins)
            return;
        installedPlugins = std::move (fresh);

        pluginBox.clear (dontSendNotification);
        pluginBox.addItem ("(none - headphone correction only)", 1);
        pluginBox.addSeparator();
        int id = 2;
        // Plugins that crashed during a scan stay in the list and can still be loaded by hand.
        const auto& skipped = knownPlugins.getBlacklistedFiles();
        for (auto& e : installedPlugins)
            pluginBox.addItem ((e.name.isNotEmpty() ? e.name : e.file.getFileNameWithoutExtension())
                                   + (skipped.contains (e.file.getFullPathName()) ? " (skipped by scan)" : ""),
                               id++);
        pluginBox.addSeparator();
        pluginBox.addItem ("Rescan plugins", rescanItemId);

        const auto path = settings->getValue ("pluginPath");
        const auto name = settings->getValue ("pluginName");
        const File current (path);
        int index = -1;
        for (int i = 0; i < (int) installedPlugins.size(); ++i)
            if (installedPlugins[(size_t) i].file == current && installedPlugins[(size_t) i].name == name)
                index = i;
        if (path.isEmpty())
            pluginBox.setSelectedId (1, dontSendNotification);
        else if (index >= 0)
            pluginBox.setSelectedId (index + 2, dontSendNotification);
        else if (current.exists())
            pluginBox.setText (name.isNotEmpty() ? name : current.getFileNameWithoutExtension(), dontSendNotification);
    }

    void pluginBoxChanged()
    {
        const int id = pluginBox.getSelectedId();
        if (id == rescanItemId)
        {
            refreshPluginList(); // shows the current plugin again
            if (pluginScanner == nullptr)
                startPluginScan (true);
            return;
        }
        if (id == 1)
        {
            // No plugin: dry signal, e.g. headphone correction only.
            if (plugin != nullptr)
                saveState();
            unloadPlugin();
            settings->setValue ("pluginPath", String());
            settings->setValue ("pluginName", String());
            lastError = {};
            deviceManager.addAudioCallback (&engine);
            engine.setMuted (feedbackRisk);
            updateSize();
            return;
        }
        const int index = id - 2;
        if (isPositiveAndBelow (index, (int) installedPlugins.size()))
        {
            // Copy: loading rebuilds the list.
            const auto entry = installedPlugins[(size_t) index];
            loadPlugin (entry.file, entry.name);
        }
    }

    // Shows the plugins inside a shell file and loads the chosen one.
    void askWhichShellPlugin (const File& file)
    {
        settings->setValue ("pluginPath", String());
        settings->setValue ("pluginName", String());
        refreshPluginList(); // now lists every plugin of the shell
        deviceManager.addAudioCallback (&engine);
        checkFeedback(); // dry signal keeps playing
        updateSize();

        StringArray names;
        for (auto& t : typesInFile (file))
            names.addIfNotAlreadyThere (t.name);
        names.sortNatural();
        PopupMenu menu;
        menu.addSectionHeader (file.getFileNameWithoutExtension() + " contains several plugins:");
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (i + 1, names[i]);
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&pluginBox),
                            [safeThis = Component::SafePointer<Component> (this), this, file, names] (int result)
                            {
                                if (safeThis != nullptr && result > 0)
                                    loadPlugin (file, names[result - 1]);
                            });
    }

    void browseForPlugin()
    {
        chooser = std::make_unique<FileChooser> ("Choose a VST3 plugin", pluginFolders()[0], "*.vst3");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                  | FileBrowserComponent::canSelectDirectories,
                              [this] (const FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f.exists())
                                      loadPlugin (f);
                              });
    }

    void unloadPlugin()
    {
        engine.setMuted (true);
        deviceManager.removeAudioCallback (&engine);
        engine.setPlugin (nullptr);
        foldRearIntoSurround = false;
        activeLayoutBase = {};
        applyFold();
        if (editor != nullptr)
        {
            editor->removeComponentListener (this);
            pluginHolder.removeChildComponent (editor.get());
            editor.reset();
        }
        plugin.reset();
        params = {};
        for (auto& box : axisBoxes)
            box.clear (dontSendNotification);
        updateDownmix();
    }

    // Loads a plugin file. For a shell file with several plugins, "name" picks one of them;
    // without a name the user is asked which one to load.
    void loadPlugin (const File file, const String name = {})
    {
        if (plugin != nullptr)
            saveState();
        unloadPlugin();

        // Use the scan results if the file was scanned, otherwise open it now and remember it.
        OwnedArray<PluginDescription> types;
        for (auto& t : typesInFile (file))
            types.add (new PluginDescription (t));
        if (types.isEmpty())
        {
            for (auto* format : formatManager.getFormats())
                if (format->fileMightContainThisPluginType (file.getFullPathName()))
                    format->findAllTypesForFile (types, file.getFullPathName());
            for (auto* t : types)
                knownPlugins.addType (*t);
            if (! types.isEmpty())
            {
                // It loads fine now: no longer skipped by the scan.
                knownPlugins.removeFromBlacklist (file.getFullPathName());
                savePluginCache();
            }
        }

        if (types.isEmpty())
        {
            settings->setValue ("pluginPath", String());
            settings->setValue ("pluginName", String());
            lastError = "Not a usable VST3 plugin: " + file.getFileName();
            deviceManager.addAudioCallback (&engine);
            checkFeedback(); // unmute again (dry signal keeps playing)
            updateSize();
            return;
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        const double sr = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
        const int bs    = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

        // A shell file (e.g. Waves' WaveShell) holds many plugins: pick the requested one.
        const PluginDescription* type = types[0];
        String chosenName;
        if (types.size() > 1)
        {
            type = nullptr;
            for (auto* t : types)
                if (name.isNotEmpty() && t->name == name && type == nullptr)
                    type = t;
            if (type == nullptr)
            {
                askWhichShellPlugin (file);
                return;
            }
            chosenName = type->name;
        }

        String error;
        plugin = formatManager.createPluginInstance (*type, sr, bs, error);

        if (plugin != nullptr)
            pluginDefaultLayout = plugin->getBusesLayout();
        if (plugin == nullptr)
        {
            lastError = "Could not load " + file.getFileName() + ": " + error;
            deviceManager.addAudioCallback (&engine);
            checkFeedback(); // unmute again (dry signal keeps playing)
            updateSize();
            return;
        }

        settings->setValue ("pluginPath", file.getFullPathName());
        settings->setValue ("pluginName", chosenName);
        refreshPluginList();

        MemoryBlock state;
        if (state.fromBase64Encoding (settings->getValue (pluginKey ("state"))) && state.getSize() > 0)
            plugin->setStateInformation (state.getData(), (int) state.getSize());

        setPluginLayout();
        setupAxisSelection();

        engine.setPlugin (plugin.get());
        deviceManager.addAudioCallback (&engine);
        checkFeedback();

        if (plugin->hasEditor())
        {
            editor.reset (plugin->createEditorIfNeeded());
            if (editor != nullptr)
            {
                pluginHolder.addAndMakeVisible (*editor);
                editorBaseSize = { editor->getWidth(), editor->getHeight() };
               #if JUCE_MAC
                const bool canZoom = editor->isResizable();
                zoomBox.setEnabled (canZoom);
                zoomBox.setTooltip (canZoom ? "Plugin zoom (resizes the plugin window)."
                                            : "This plugin has a fixed size - macOS can't zoom it from the host.");
                if (! canZoom)
                    zoomBox.setSelectedId (100, dontSendNotification);
                else
               #endif
                zoomBox.setSelectedId (settings->getIntValue (pluginKey ("zoom"), 100), dontSendNotification);
                applyZoom();
                editor->addComponentListener (this);
            }
        }
        lastError = {};
        pluginLoadedTime = Time::getMillisecondCounter();
        updateSize();
    }

    // Tries one input arrangement, keeping the plugin's other buses (e.g. a side chain) as they are.
    bool tryInputLayout (const AudioChannelSet& in)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto l = pluginDefaultLayout;
            if (l.inputBuses.isEmpty())
                l.inputBuses.add (in);
            else
                l.inputBuses.getReference (0) = in;
            if (attempt == 0) // stereo output first, then the plugin's own main output
            {
                if (l.outputBuses.isEmpty())
                    l.outputBuses.add (AudioChannelSet::stereo());
                else
                    l.outputBuses.getReference (0) = AudioChannelSet::stereo();
            }
            if (plugin->setBusesLayout (l))
                return true;
        }
        return false;
    }

    void setPluginLayout()
    {
        auto layouts = inputLayouts();
        const int wanted = jlimit (0, (int) layouts.size() - 1, layoutBox.getSelectedItemIndex());
        const int wantedChannels = layouts[(size_t) wanted].set.size();
        foldRearIntoSurround = false;

        auto accept = [&] (int i)
        {
            // A 7.1 source into a plugin that only takes 5.1: optionally mix the rear pair in.
            foldRearIntoSurround = wantedChannels == 8 && layouts[(size_t) i].set.size() == 6;
            activeLayoutBase = layouts[(size_t) i].name;
            applyFold();
            updateDownmix();
        };

        // 1. The chosen layout.
        if (tryInputLayout (layouts[(size_t) wanted].set))
            return accept (wanted);

        // 2. The plugin's own arrangement, if it has the same number of channels.
        if (! pluginDefaultLayout.inputBuses.isEmpty()
            && pluginDefaultLayout.inputBuses[0].size() == wantedChannels)
            if (plugin->setBusesLayout (pluginDefaultLayout))
                return accept (wanted);

        // 3. Other layouts: the closest smaller one first, then larger ones.
        std::vector<int> order;
        for (int i = 0; i < (int) layouts.size(); ++i)
            if (i != wanted)
                order.push_back (i);
        std::stable_sort (order.begin(), order.end(), [&] (int a, int b)
        {
            const int ca = layouts[(size_t) a].set.size(), cb = layouts[(size_t) b].set.size();
            const bool sa = ca < wantedChannels, sb = cb < wantedChannels;
            if (sa != sb) return sa;
            return sa ? ca > cb : ca < cb;
        });
        for (int i : order)
            if (tryInputLayout (layouts[(size_t) i].set))
                return accept (i);

        activeLayoutBase = {};
        activeLayout = "plugin default (" + String (plugin->getTotalNumInputChannels()) + " in)";
        applyFold();
        updateDownmix();
    }

    // Feeds the rear pair of a 7.1 source into the surround pair of a 5.1-only plugin.
    void applyFold()
    {
        for (int ch = 0; ch < HostEngine::maxChannels; ++ch)
            engine.setFoldSource (ch, -1);
        if (activeLayoutBase.isNotEmpty())
            activeLayout = ! foldRearIntoSurround ? activeLayoutBase
                         : foldButton.getToggleState() ? String ("5.1 (7.1 rear mixed in)")
                                                       : String ("5.1 (7.1 rear channels off)");
        if (foldRearIntoSurround && foldButton.getToggleState())
        {
            engine.setFoldSource (4, sourceFor (6));
            engine.setFoldSource (5, sourceFor (7));
        }
    }

    // Common stereo downmix: front L/R at 0 dB, centre and all other channels at -3 dB to their
    // side, LFE left out; scaled down so typical material does not clip.
    void updateDownmix()
    {
        std::vector<std::pair<float, float>> gains;
        const auto names = pluginChannelNames();
        engine.setDryChannels (jmax (2, names.size()));
        const float m3 = 0.7071f, scale = names.size() > 2 ? 0.5f : 1.0f;
        for (auto& n : names)
        {
            std::pair<float, float> g { 0.0f, 0.0f };
            if (n.equalsIgnoreCase ("Lfe") || n.startsWithIgnoreCase ("Lfe"))
                g = { 0.0f, 0.0f };
            else if (n == "L")                g = { 1.0f, 0.0f };
            else if (n == "R")                g = { 0.0f, 1.0f };
            else if (n.startsWith ("C"))      g = { m3, m3 };
            else if (n.containsIgnoreCase ("l")) g = { m3, 0.0f };
            else if (n.containsIgnoreCase ("r")) g = { 0.0f, m3 };
            else                              g = { m3, m3 };
            gains.push_back ({ g.first * scale, g.second * scale });
        }
        if (gains.empty())
            gains = { { 1.0f, 0.0f }, { 0.0f, 1.0f } };
        engine.setDownmixGains (gains);
        meters.names = names;
        meters.repaint();
    }

    void applyZoom()
    {
        const int z = zoomBox.getSelectedId() > 0 ? zoomBox.getSelectedId() : 100;
        if (plugin != nullptr)
            settings->setValue (pluginKey ("zoom"), z);
        if (editor != nullptr)
        {
           #if JUCE_MAC
            // macOS: VST3 plugins ignore host scale requests (content scaling is Windows/Linux only).
            // Resizable editors are zoomed via their size instead; fixed-size ones can't be zoomed.
            if (editor->isResizable())
            {
                editor->setSize (roundToInt (editorBaseSize.x * z / 100.0f), roundToInt (editorBaseSize.y * z / 100.0f));
            }
           #else
            editor->setScaleFactor ((float) z / 100.0f);
           #endif
        }
        layoutPluginArea();
    }

    void applyLayout()
    {
        deviceManager.removeAudioCallback (&engine);
        if (plugin != nullptr)
            setPluginLayout();
        else
            updateDownmix();
        deviceManager.addAudioCallback (&engine);
    }

    //==============================================================================
    // Yaw / pitch / roll parameter selection (saved per plugin)
    struct AxisParam
    {
        AudioProcessorParameter* param = nullptr;
        float low = 0.0f, high = 0.0f; // degrees at normalised 0 and 1
    };

    static constexpr int noneId = 100000;

    void setupAxisSelection()
    {
        const auto& all = plugin->getParameters();
        for (size_t i = 0; i < 3; ++i)
        {
            auto& box = axisBoxes[i];
            box.clear (dontSendNotification);
            box.addItem ("- none -", noneId);
            box.addSeparator();
            for (int p = 0; p < all.size(); ++p)
                box.addItem (String (p) + " - " + all[p]->getName (64), p + 1);

            // Saved choice, otherwise guess by name ("HT Yaw" preferred over plain "Yaw").
            const auto saved = settings->getValue (pluginKey (String ("param") + axisNames[i]));
            int chosen = -1;
            for (int p = 0; p < all.size() && chosen < 0; ++p)
                if (saved.isNotEmpty() && all[p]->getName (64) == saved)
                    chosen = p;
            if (saved.isEmpty())   // never chosen for this plugin: guess by name
            {
                for (int p = 0; p < all.size(); ++p)
                {
                    const auto name = all[p]->getName (64);
                    if (name.containsIgnoreCase (axisNames[i]) && (chosen < 0 || name.containsIgnoreCase ("HT")))
                        chosen = p;
                }
            }
            box.setSelectedId (chosen >= 0 ? chosen + 1 : noneId, dontSendNotification);
            selectAxisParam (i, chosen);

            const auto invertKey = pluginKey (String ("invert") + axisNames[i]);
            invertButtons[i].setToggleState (settings->getBoolValue (invertKey, defaultInvert[i]), dontSendNotification);
        }
    }

    void axisBoxChanged (size_t axis)
    {
        if (plugin == nullptr)
            return;
        const int id = axisBoxes[axis].getSelectedId();
        const int p = id == noneId ? -1 : id - 1;
        selectAxisParam (axis, p);
        const auto& all = plugin->getParameters();
        // "<none>" is remembered so the axis is not guessed again on the next start.
        settings->setValue (pluginKey (String ("param") + axisNames[axis]),
                            isPositiveAndBelow (p, all.size()) ? all[p]->getName (64) : String ("<none>"));
    }

    void selectAxisParam (size_t axis, int index)
    {
        auto& a = params[axis];
        a = {};
        const auto& all = plugin->getParameters();
        if (! isPositiveAndBelow (index, all.size()))
            return;

        a.param = all[index];
        a.low  = parseDegrees (a.param->getText (0.0f, 32));
        a.high = parseDegrees (a.param->getText (1.0f, 32));
        if (std::abs (a.high - a.low) < 1.0f)
        {
            a.low  = -fallbackRange[axis];
            a.high = fallbackRange[axis];
        }
        axisBoxes[axis].setTooltip ("Range: " + String (a.low, 0) + " to " + String (a.high, 0) + " degrees");
    }

    int mappedAxes() const
    {
        return (params[0].param != nullptr) + (params[1].param != nullptr) + (params[2].param != nullptr);
    }

    //==============================================================================
    // Audio safety: a loopback device (e.g. BlackHole) as input AND output would feed back.
    void changeListenerCallback (ChangeBroadcaster*) override
    {
        // Any device change (by the user or the watchdog) starts a new grace period.
        audioStartTime = lastCallbackTime = Time::getMillisecondCounter();
        streamWasRunning = false;
        checkFeedback();
    }

    void checkFeedback()
    {
        const auto setup = deviceManager.getAudioDeviceSetup();
        String input = setup.inputDeviceName, output = setup.outputDeviceName;

        if (deviceManager.getCurrentAudioDeviceType() == "ASIO")
        {
            // ASIO only reports the driver name; FlexASIO's real devices are in its config file.
            if (output.containsIgnoreCase ("FlexASIO"))
                std::tie (input, output) = readFlexAsioDevices();
            else
                input = output = {};
        }

        feedbackRisk = sameLoopback (input, output);
        engine.setMuted (feedbackRisk);
    }

    //==============================================================================
    // OSC from Bridgehead
    void applyPort()
    {
        const int port = jlimit (1, 65535, portEditor.getText().getIntValue());
        portEditor.setText (String (port), dontSendNotification);
        settings->setValue ("oscPort", port);
        if (! trackingButton.getToggleState())
        {
            receiver.disconnect();   // head tracking off: don't occupy the port
            oscConnected = false;
            currentPort = port;
            return;
        }
        if (port == currentPort && oscConnected)
            return;
        receiver.disconnect();
        currentPort = port;
        oscConnected = receiver.connect (port);
    }

    void setHeadTracking (bool on)
    {
        settings->setValue ("headTracking", on);
        applyPort();
        if (! on)
            for (size_t i = 0; i < 3; ++i)
                applyAxisDegrees (i, 0.0f);   // look straight ahead
    }

    static bool numberAt (const OSCMessage& m, int index, float& out)
    {
        if (m[index].isFloat32())     { out = m[index].getFloat32(); return true; }
        if (m[index].isInt32())       { out = (float) m[index].getInt32(); return true; }
        return false;
    }

    // Bridgehead profile "IEM Scene Rotator YPR": separate /SceneRotator/yaw, /pitch, /roll messages.
    // That profile sign-flips pitch and roll; internally the tracker's raw angles are used.
    void oscMessageReceived (const OSCMessage& m) override
    {
        ++messagesReceived;
        const auto address = m.getAddressPattern().toString();
        lastAddress = address;

        if (! trackingButton.getToggleState())
            return;
        float value = 0.0f;
        if (m.size() != 1 || ! numberAt (m, 0, value))
            return;

        const auto lower = address.toLowerCase();
        for (size_t i = 0; i < 3; ++i)
        {
            if (lower == "/scenerotator/" + String (axisNames[i]).toLowerCase())
            {
                noteAngles();
                // Normal orientation = IEM Scene Rotator / Virtuoso convention. The IEM profile's
                // roll is flipped relative to that convention, pitch already matches.
                applyAxis (i, i == 2 ? -value : value);
                lastAngles[i] = i == 2 ? -value : value;
            }
        }
    }

    void noteAngles()
    {
        ++angleMessages;
        lastAngleTime = Time::getMillisecondCounter();
    }

    void applyAxis (size_t i, float rawDegrees)
    {
        applyAxisDegrees (i, invertButtons[i].getToggleState() ? -rawDegrees : rawDegrees);
    }

    void applyAxisDegrees (size_t i, float degrees)
    {
        {
            auto& a = params[i];
            if (a.param == nullptr)
                return;
            float d = degrees;
            const float span = a.high - a.low;
            if (std::abs (span) >= 359.0f)
            {
                // Full-circle parameter (e.g. 0..360 degrees): wrap the angle into its range.
                const float lo = jmin (a.low, a.high);
                d = lo + std::fmod (std::fmod (d - lo, 360.0f) + 360.0f, 360.0f);
            }
            const float norm = jlimit (0.0f, 1.0f, (d - a.low) / span);
            if (std::abs (norm - a.param->getValue()) > 1.0e-6f)
                a.param->setValueNotifyingHost (norm);
        }
    }

    void oscBundleReceived (const OSCBundle& bundle) override
    {
        for (auto& element : bundle)
        {
            if (element.isMessage())       oscMessageReceived (element.getMessage());
            else if (element.isBundle())   oscBundleReceived (element.getBundle());
        }
    }

    //==============================================================================
    // Status
    // Reopens the audio device if its stream stopped (power saving, sample rate change by another
    // app, device sleeping or briefly removed). Checked twice per second.
    void audioWatchdog()
    {
        if (! audioInitialised)
            return;

        const auto now = Time::getMillisecondCounter();
        const auto count = engine.getCallbackCount();
        auto* device = deviceManager.getCurrentAudioDevice();

        if (count != lastCallbackCount)
        {
            lastCallbackCount = count;
            lastCallbackTime = now;
            if (now - audioStartTime > 1000)
                streamWasRunning = true;   // ran on its own for a moment - a real, started stream
            if (recovering && now - lastRestartAttempt > 10000)
            {
                recovering = false;        // stable again after a reconnect
                restartAttempts = 0;
            }
        }

        // Clear the startup crash flag once the app has been running for a while.
        if (! startupCleared && now - audioStartTime > 8000)
        {
            startupCleared = true;
            settings->setValue ("startupPending", false);
            settings->saveIfNeeded();
        }

        const bool error   = engine.takeDeviceError();
        const bool inGrace = now - audioStartTime < 5000;          // device may still be starting
        // A running device that simply delivers nothing (e.g. FlexASIO while no audio is playing) is
        // never touched - it continues by itself when audio starts. Only a device that really stopped
        // (closed by the system, stopped, or reported an error) is reopened.
        const bool deviceStopped = device != nullptr && ! device->isPlaying();
        const bool stalled = streamWasRunning && deviceStopped;
        const bool lost    = device == nullptr && streamWasRunning;
        const bool stillDown = recovering && (device == nullptr || deviceStopped) && now - lastCallbackTime > 4000;

        // Never give up: FlexASIO (and other drivers) deliver nothing while no audio is playing and can
        // only restart once something plays again. Keep retrying quietly; the grace period after each
        // attempt spaces the retries ~5 s apart.
        if (inGrace)
            return;
        if (! (stalled || lost || stillDown || (error && streamWasRunning)))
            return;

        lastRestartAttempt = lastReconnectShown = now;
        ++restartAttempts;
        recovering = true;
        streamWasRunning = false;

        deviceManager.closeAudioDevice();
        deviceManager.restartLastAudioDevice();
        if (deviceManager.getCurrentAudioDevice() == nullptr)
        {
            // Fall back to the saved device setup (e.g. after the device was unplugged).
            std::unique_ptr<XmlElement> saved (settings->getXmlValue ("audioDevice"));
            deviceManager.initialise (microphoneDenied ? 0 : 16, 16, saved.get(), true);
        }
        audioStartTime = lastCallbackTime = Time::getMillisecondCounter();
        checkFeedback();
    }

    void timerCallback() override
    {
        audioWatchdog();
        const auto now = Time::getMillisecondCounter();
        const int rate = (int) (angleMessages - lastCountedAngles) * 2;
        lastCountedAngles = angleMessages;
        const bool receivingAngles = now - lastAngleTime < 1000;
        const bool receivingAnything = messagesReceived != lastCountedMessages;
        lastCountedMessages = messagesReceived;
        const bool trackingNeeded = trackingButton.getToggleState() && plugin != nullptr
                                    && speakerSimButton.getToggleState();

        Colour c = green;
        String status, detail;

        if (plugin == nullptr && safeMode)
        {
            c = amber; status = "Safe start - the last start did not complete";
            detail = "The saved audio device and plugin were not opened. Choose them again - your settings are kept.";
        }
        else if (recovering && restartAttempts >= 2 && now - lastCallbackTime > 4000)
        {
            c = amber; status = "Waiting for the audio device...";
            detail = "The audio device stopped. Retrying automatically - for FlexASIO, start playing something.";
        }
        else if (recovering && now - lastCallbackTime > 1000)
        {
            c = amber; status = "Reconnecting audio device...";
            detail = "The audio stream stopped (e.g. power saving or a sample rate change). Retrying automatically.";
        }
        else if (lastReconnectShown != 0 && now - lastReconnectShown < 10000)
        {
            c = amber; status = "Audio device reconnected";
            detail = "The audio device had stopped and was reopened automatically.";
        }
        else if (microphoneDenied)
        {
            c = red; status = "No access to audio input";
            detail = "Allow it in System Settings > Privacy & Security > Microphone > Head Tracking Host, then restart.";
        }
        else if (deviceManager.getCurrentAudioDevice() == nullptr)
        {
            c = red; status = "No audio device"; detail = "Click \"Audio settings...\" and choose your input and output.";
        }
        else if (feedbackRisk)
        {
            c = red; status = "Output muted: input and output are the same loopback device";
            detail = "In \"Audio settings...\" choose your headphones or interface as output.";
        }
        else if (plugin == nullptr && lastError.isNotEmpty())
        {
            c = amber; status = "Plugin not loaded";
            detail = lastError;
        }
        else if (trackingNeeded && ! oscConnected)
        {
            c = red; status = "Port " + String (currentPort) + " is in use";
            detail = "Close other programs listening on this port (e.g. a standalone renderer with OSC head tracking).";
        }
        else if (trackingNeeded && mappedAxes() == 0)
        {
            c = amber; status = "Choose the plugin parameters for Yaw, Pitch and Roll";
        }
        else if (trackingNeeded && ! receivingAngles && receivingAnything)
        {
            c = amber; status = "Receiving " + lastAddress + " - but no yaw/pitch/roll";
            detail = "Send /SceneRotator/yaw, /pitch and /roll (Bridgehead: profile \"IEM Scene Rotator YPR\").";
        }
        else if (trackingNeeded && ! receivingAngles)
        {
            c = amber; status = "No head tracking data on port " + String (currentPort);
            detail = "Start your head tracker app (e.g. Bridgehead, profile \"IEM Scene Rotator YPR\"), or turn head tracking off.";
        }
        else if (pluginScanner != nullptr && scanningPluginName.isNotEmpty())
        {
            status = "Scanning plugins... " + String (roundToInt (pluginScanner->getProgress() * 100.0f)) + " %";
            detail = scanningPluginName;
        }
        else
        {
            // All good: show what cannot be read anywhere else.
            const String sep = String::fromUTF8 ("  \xc2\xb7  "), arrow = String::fromUTF8 ("  \xe2\x86\x92  ");
            auto* device = deviceManager.getCurrentAudioDevice();
            const double sr = device->getCurrentSampleRate();
            const int block = device->getCurrentBufferSizeSamples();
            int latency = block + device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
            if (plugin != nullptr && speakerSimButton.getToggleState())
                latency += plugin->getLatencySamples();
            status = (std::fmod (sr, 1000.0) == 0.0 ? String ((int) (sr / 1000.0)) : String (sr / 1000.0, 1)) + " kHz" + sep
                   + (plugin != nullptr ? activeLayout : String (device->getActiveInputChannels().countNumberOfSetBits()) + " ch")
                   + sep + String (sr > 0 ? 1000.0 * latency / sr : 0.0, 1) + " ms";

            AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup (setup);
            String route = setup.inputDeviceName == setup.outputDeviceName || setup.inputDeviceName.isEmpty()
                         ? device->getName() : setup.inputDeviceName + arrow + setup.outputDeviceName;
            detail = (trackingNeeded ? "Tracker " + String (rate) + "/s" + sep : String()) + route
                   + sep + "buffer " + String (block);
            if (trackingNeeded && now - pluginLoadedTime < 20000)
                detail = "Turn off the plugin's own head tracking input (e.g. Virtuoso: Manual).";
        }

        statusDot.colour = c;
        statusDot.repaint();
        statusLabel.setText (status, dontSendNotification);
        detailLabel.setText (detail, dontSendNotification);
    }

    //==============================================================================
    //==============================================================================
    // Input channel routing: which device input feeds each plugin input channel.
    void loadInputMap()
    {
        inputMap.clear();
        auto tokens = StringArray::fromTokens (settings->getValue ("inputMap"), ",", {});
        if (tokens.isEmpty() && settings->getBoolValue ("swapSurrounds", false))
            tokens = StringArray::fromTokens ("0,1,2,3,6,7,4,5", ",", {}); // from the old swap option
        for (auto& t : tokens)
            inputMap.push_back (t.trim().getIntValue());
        applyInputMap();
    }

    int sourceFor (int pluginChannel) const
    {
        return pluginChannel < (int) inputMap.size() ? inputMap[(size_t) pluginChannel] : pluginChannel;
    }

    void setSource (int pluginChannel, int deviceChannel)
    {
        while ((int) inputMap.size() <= pluginChannel)
            inputMap.push_back ((int) inputMap.size());
        inputMap[(size_t) pluginChannel] = deviceChannel;
        applyInputMap();
    }

    void applyInputMap()
    {
        // Drop trailing identity entries so a plain 1:1 routing is stored as "".
        while (! inputMap.empty() && inputMap.back() == (int) inputMap.size() - 1)
            inputMap.pop_back();

        StringArray tokens;
        for (auto v : inputMap)
            tokens.add (String (v));
        settings->setValue ("inputMap", tokens.joinIntoString (","));
        settings->removeValue ("swapSurrounds");

        for (int ch = 0; ch < HostEngine::maxChannels; ++ch)
            engine.setInputSource (ch, sourceFor (ch));

        channelsButton.setButtonText (inputMap.empty() ? "Channels: 1:1" : "Channels: custom");
        applyFold();
    }

    // Input channels of the plugin, or of the chosen input layout when no plugin is loaded.
    StringArray pluginChannelNames() const
    {
        StringArray names;
        AudioChannelSet layout;
        if (plugin != nullptr && plugin->getBusCount (true) > 0)
            layout = plugin->getBus (true, 0)->getCurrentLayout();
        else
            layout = inputLayouts()[(size_t) jlimit (0, (int) inputLayouts().size() - 1, layoutBox.getSelectedItemIndex())].set;
        for (int i = 0; i < layout.size(); ++i)
            names.add (AudioChannelSet::getAbbreviatedChannelTypeName (layout.getTypeOfChannel (i)));
        return names;
    }

    StringArray deviceInputNames() const
    {
        StringArray names;
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const auto all = device->getInputChannelNames();
            const auto active = device->getActiveInputChannels();
            for (int i = 0; i < all.size(); ++i)
                if (active[i])
                    names.add (all[i]);
        }
        return names;
    }

    class RoutingComponent : public Component
    {
    public:
        explicit RoutingComponent (MainComponent& o) : owner (o)
        {
            const auto channels = owner.pluginChannelNames();
            const auto inputs = owner.deviceInputNames();

            addAndMakeVisible (title);
            title.setText (channels.isEmpty() ? "Load a plugin first." : "Plugin input  <-  audio device input",
                           dontSendNotification);

            for (int ch = 0; ch < channels.size(); ++ch)
            {
                auto* label = rows.add (new Label ({}, channels[ch]));
                addAndMakeVisible (label);
                auto* box = boxes.add (new ComboBox());
                box->addItem ("Off", 1);
                for (int i = 0; i < jmax (inputs.size(), channels.size()); ++i)
                    box->addItem (String (i + 1) + (i < inputs.size() ? " - " + inputs[i] : String (" (not active)")), i + 2);
                box->setSelectedId (owner.sourceFor (ch) + 2, dontSendNotification);
                box->onChange = [this, ch, box] { owner.setSource (ch, box->getSelectedId() - 2); };
                addAndMakeVisible (box);
            }

            for (auto* b : { &resetButton, &swapButton })
                addAndMakeVisible (b);
            resetButton.onClick = [this] { applyPreset ({}); };
            swapButton.onClick  = [this] { applyPreset ({ 0, 1, 2, 3, 6, 7, 4, 5 }); };

            setSize (460, 70 + channels.size() * 30 + 50);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14);
            title.setBounds (r.removeFromTop (28));
            for (int i = 0; i < rows.size(); ++i)
            {
                auto row = r.removeFromTop (30).reduced (0, 2);
                rows[i]->setBounds (row.removeFromLeft (70));
                boxes[i]->setBounds (row);
            }
            r.removeFromTop (12);
            auto buttons = r.removeFromTop (28);
            resetButton.setBounds (buttons.removeFromLeft (120));
            buttons.removeFromLeft (10);
            swapButton.setBounds (buttons.removeFromLeft (230));
        }

    private:
        void applyPreset (std::vector<int> map)
        {
            for (int ch = 0; ch < boxes.size(); ++ch)
            {
                const int src = ch < (int) map.size() ? map[(size_t) ch] : ch;
                owner.setSource (ch, src);
                boxes[ch]->setSelectedId (src + 2, dontSendNotification);
            }
        }

        MainComponent& owner;
        Label title;
        OwnedArray<Label> rows;
        OwnedArray<ComboBox> boxes;
        TextButton resetButton { "Reset (1:1)" }, swapButton { "Swap side/rear (Windows)" };
    };

    void showChannelRouting()
    {
        DialogWindow::LaunchOptions o;
        o.content.setOwned (new RoutingComponent (*this));
        o.dialogTitle = "Input channels";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    //==============================================================================
    // AutoEQ: three offline slots, filled from the AutoEq database (needs internet only to search).
    String slotKey (int slot, const char* what) const { return "autoeqSlot" + String (slot + 1) + "." + what; }

    void refreshAutoEqSlots()
    {
        const int selected = autoEqSlotBox.getSelectedItemIndex();
        autoEqSlotBox.clear (dontSendNotification);
        for (int i = 0; i < numEqSlots; ++i)
        {
            const auto name = settings->getValue (slotKey (i, "name"));
            autoEqSlotBox.addItem ("Slot " + String (i + 1) + ": " + (name.isNotEmpty() ? name : String ("(empty)")), i + 1);
        }
        autoEqSlotBox.setSelectedItemIndex (jmax (0, selected), dontSendNotification);
    }

    void applyAutoEq()
    {
        const int slot = jmax (0, autoEqSlotBox.getSelectedItemIndex());
        const auto profile = parseAutoEq (settings->getValue (slotKey (slot, "text")));
        engine.getHeadphoneEq().setProfile (profile, autoEqButton.getToggleState());
        autoEqSlotBox.setTooltip (profile.isEmpty() ? "Empty slot - click \"Headphone correction...\" to fill it."
                                                    : String ((int) profile.bands.size()) + " filters, preamp "
                                                          + String (profile.preampDb, 1) + " dB");
    }

    void storeAutoEqSlot (int slot, const String& name, const String& text)
    {
        settings->setValue (slotKey (slot, "name"), name);
        settings->setValue (slotKey (slot, "text"), text);
        settings->saveIfNeeded();
        refreshAutoEqSlots();
        autoEqSlotBox.setSelectedItemIndex (slot, sendNotification);
        applyAutoEq();
        notifyHeadphoneWindow();
    }

    void clearAutoEqSlot (int slot)
    {
        settings->removeValue (slotKey (slot, "name"));
        settings->removeValue (slotKey (slot, "text"));
        settings->saveIfNeeded();
        refreshAutoEqSlots();
        applyAutoEq();
        notifyHeadphoneWindow();
    }

    int activeSlot() const { return jmax (0, autoEqSlotBox.getSelectedItemIndex()); }
    String slotName (int slot) const { return settings->getValue (slotKey (slot, "name")); }
    EqProfile slotProfile (int slot) const { return parseAutoEq (settings->getValue (slotKey (slot, "text"))); }

    // Window with the three slots, the curve, database search and file import.
    class HeadphoneWindow : public Component
    {
    public:
        explicit HeadphoneWindow (MainComponent& o) : owner (o)
        {
            addAndMakeVisible (slotBox);
            slotBox.onChange = [this]
            {
                owner.autoEqSlotBox.setSelectedItemIndex (slotBox.getSelectedItemIndex(), sendNotification);
                refresh();
            };
            addAndMakeVisible (curve);
            addAndMakeVisible (info);
            info.setColour (Label::textColourId, Colours::lightgrey);
            for (auto* b : { &findButton, &importButton, &clearButton })
                addAndMakeVisible (b);
            findButton.onClick   = [this] { owner.showAutoEqSearch(); };
            importButton.onClick = [this] { importFile(); };
            clearButton.onClick  = [this] { owner.clearAutoEqSlot (slotBox.getSelectedItemIndex()); };
            addAndMakeVisible (hint);
            hint.setColour (Label::textColourId, Colours::grey);
            hint.setText ("Import: AutoEq \"ParametricEQ.txt\" (from autoeq.app or GitHub, same format as Equalizer APO).",
                          dontSendNotification);
            setSize (640, 460);
            refresh();
        }

        void refresh()
        {
            slotBox.clear (dontSendNotification);
            for (int i = 0; i < numEqSlots; ++i)
            {
                const auto name = owner.slotName (i);
                slotBox.addItem ("Slot " + String (i + 1) + ": " + (name.isNotEmpty() ? name : String ("(empty)")), i + 1);
            }
            const int slot = owner.activeSlot();
            slotBox.setSelectedItemIndex (slot, dontSendNotification);
            const auto profile = owner.slotProfile (slot);
            curve.setProfile (profile);
            info.setText (profile.bands.empty() ? String ("This slot is empty - find your headphones or import a file.")
                                                : String ((int) profile.bands.size()) + " filters, preamp "
                                                      + String (profile.preampDb, 1) + " dB (keeps the level safe)",
                          dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            slotBox.setBounds (r.removeFromTop (28));
            r.removeFromTop (10);
            auto buttons = r.removeFromBottom (28);
            findButton.setBounds (buttons.removeFromLeft (200));
            buttons.removeFromLeft (8);
            importButton.setBounds (buttons.removeFromLeft (150));
            clearButton.setBounds (buttons.removeFromRight (110));
            r.removeFromBottom (6);
            hint.setBounds (r.removeFromBottom (20));
            info.setBounds (r.removeFromBottom (22));
            r.removeFromBottom (6);
            curve.setBounds (r);
        }

    private:
        void importFile()
        {
            chooser = std::make_unique<FileChooser> ("Import an AutoEq ParametricEQ.txt",
                                                     File::getSpecialLocation (File::userHomeDirectory), "*.txt");
            const int slot = slotBox.getSelectedItemIndex();
            Component::SafePointer<HeadphoneWindow> safe (this);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [safe, slot] (const FileChooser& fc)
                                  {
                                      const auto file = fc.getResult();
                                      if (safe == nullptr || ! file.existsAsFile())
                                          return;
                                      const auto text = file.loadFileAsString();
                                      if (parseAutoEq (text).bands.empty())
                                      {
                                          safe->info.setText ("No EQ filters found in " + file.getFileName()
                                                                  + " - expected an AutoEq ParametricEQ.txt.", dontSendNotification);
                                          return;
                                      }
                                      auto name = file.getFileNameWithoutExtension();
                                      name = name.endsWithIgnoreCase (" ParametricEQ") ? name.dropLastCharacters (13) : name;
                                      safe->owner.storeAutoEqSlot (slot, name + " (imported)", text);
                                  });
        }

        MainComponent& owner;
        ComboBox slotBox;
        EqCurve curve;
        Label info, hint;
        TextButton findButton { "Find in database..." }, importButton { "Import file..." }, clearButton { "Clear slot" };
        std::unique_ptr<FileChooser> chooser;
    };

    Component::SafePointer<HeadphoneWindow> headphoneWindow;

    void notifyHeadphoneWindow()
    {
        if (headphoneWindow != nullptr)
            headphoneWindow->refresh();
    }

    void showHeadphoneWindow()
    {
        auto* content = new HeadphoneWindow (*this);
        headphoneWindow = content;
        DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = "Headphone correction";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    File autoEqIndexCache (bool all) const
    {
        return settings->getFile().getSiblingFile (all ? "Head Tracking Host AutoEq index.md"
                                                        : "Head Tracking Host AutoEq recommended.md");
    }

    struct AutoEqEntry { String name, source, path; };

    // Recommended list (results/README.md): "- [Name](./source/rig/Name%20Encoded)"
    // Full index (results/INDEX.md):        "- [Name](./source/rig/Name%20Encoded) by Source on Rig"
    static std::vector<AutoEqEntry> parseAutoEqIndex (const String& text)
    {
        std::vector<AutoEqEntry> entries;
        for (auto line : StringArray::fromLines (text))
        {
            line = line.trimEnd();
            if (! line.startsWith ("- [") || ! line.contains ("](./"))
                continue;
            AutoEqEntry e;
            e.name = line.fromFirstOccurrenceOf ("- [", false, false).upToFirstOccurrenceOf ("](./", false, false);
            const auto rest = line.fromFirstOccurrenceOf ("](./", false, false);
            if (rest.contains (") by "))
            {
                e.path   = rest.upToLastOccurrenceOf (") by ", false, false);
                e.source = rest.fromLastOccurrenceOf (") by ", false, false).trim();
            }
            else
            {
                if (! rest.endsWith (")"))
                    continue;
                e.path   = rest.dropLastCharacters (1);
                e.source = URL::removeEscapeChars (e.path.upToFirstOccurrenceOf ("/", false, false));
            }
            if (e.name.isNotEmpty() && e.path.isNotEmpty())
                entries.push_back (e);
        }
        return entries;
    }

    static String downloadText (const String& url)
    {
        auto options = URL::InputStreamOptions (URL::ParameterHandling::inAddress).withConnectionTimeoutMs (15000);
        if (auto stream = URL (url).createInputStream (options))
            return stream->readEntireStreamAsString();
        return {};
    }

    class AutoEqSearch : public Component, private ListBoxModel
    {
    public:
        explicit AutoEqSearch (MainComponent& o) : owner (o)
        {
            addAndMakeVisible (searchBox);
            searchBox.setTextToShowWhenEmpty ("Search headphones, e.g. ATH-M50x", Colours::grey);
            searchBox.onTextChange = [this] { filter(); };
            addAndMakeVisible (list);
            list.setModel (this);
            list.setRowHeight (24);
            addAndMakeVisible (status);
            status.setColour (Label::textColourId, Colours::lightgrey);
            addAndMakeVisible (saveLabel);
            saveLabel.setText ("Save to slot:", dontSendNotification);
            for (int i = 0; i < numEqSlots; ++i)
            {
                auto* b = saveButtons.add (new TextButton (String (i + 1)));
                b->setTooltip ("Save the selected headphone to slot " + String (i + 1));
                b->onClick = [this, i] { saveTo (i); };
                addAndMakeVisible (b);
            }
            addAndMakeVisible (refreshButton);
            refreshButton.onClick = [this] { loadIndex (true); };
            addAndMakeVisible (showAllButton);
            showAllButton.setButtonText ("Show all measurements");
            showAllButton.setTooltip ("Off: one recommended measurement per headphone. On: every source (oratory1990, crinacle, Rtings ...).");
            showAllButton.onClick = [this] { loadIndex (false); };
            setSize (620, 540);
            loadIndex (false);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            searchBox.setBounds (r.removeFromTop (28));
            r.removeFromTop (6);
            showAllButton.setBounds (r.removeFromTop (24).withWidth (240));
            r.removeFromTop (4);
            auto bottom = r.removeFromBottom (28);
            saveLabel.setBounds (bottom.removeFromLeft (92));
            for (auto* b : saveButtons)
            {
                b->setBounds (bottom.removeFromLeft (44));
                bottom.removeFromLeft (6);
            }
            refreshButton.setBounds (bottom.removeFromRight (110));
            r.removeFromBottom (6);
            status.setBounds (r.removeFromBottom (22));
            list.setBounds (r);
        }

    private:
        int getNumRows() override { return (int) shown.size(); }

        void paintListBoxItem (int row, Graphics& g, int width, int height, bool selected) override
        {
            if (! isPositiveAndBelow (row, (int) shown.size()))
                return;
            if (selected)
                g.fillAll (Colour (0xff1f6aa5));
            const auto& e = entries[(size_t) shown[(size_t) row]];
            g.setColour (Colours::white);
            g.setFont (FontOptions (14.0f));
            g.drawText (e.name, 6, 0, width - 230, height, Justification::centredLeft);
            g.setColour (Colours::lightgrey);
            g.drawText (e.source, width - 224, 0, 220, height, Justification::centredRight);
        }

        void loadIndex (bool forceDownload)
        {
            const bool all = showAllButton.getToggleState();
            const auto cache = owner.autoEqIndexCache (all);
            if (! forceDownload && cache.existsAsFile())
            {
                setEntries (parseAutoEqIndex (cache.loadFileAsString()), "List from cache.");
                return;
            }
            status.setText ("Downloading headphone list...", dontSendNotification);
            Component::SafePointer<AutoEqSearch> safe (this);
            const auto file = String (all ? "INDEX.md" : "README.md");
            Thread::launch ([safe, cache, file]
            {
                const auto text = downloadText (autoEqBase + file);
                MessageManager::callAsync ([safe, cache, text]
                {
                    if (safe == nullptr)
                        return;
                    auto parsed = parseAutoEqIndex (text);
                    if (parsed.empty())
                    {
                        safe->status.setText ("Could not download the list - check your internet connection.", dontSendNotification);
                        return;
                    }
                    cache.replaceWithText (text);
                    safe->setEntries (std::move (parsed), "List downloaded.");
                });
            });
        }

        void setEntries (std::vector<AutoEqEntry> e, const String& message)
        {
            entries = std::move (e);
            filter();
            status.setText (message + " " + String ((int) entries.size()) + " profiles.", dontSendNotification);
        }

        void filter()
        {
            shown.clear();
            const auto words = StringArray::fromTokens (searchBox.getText(), " ", {});
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const auto haystack = entries[i].name + " " + entries[i].source;
                bool match = true;
                for (auto& w : words)
                    if (w.isNotEmpty() && ! haystack.containsIgnoreCase (w))
                        match = false;
                if (match)
                    shown.push_back ((int) i);
            }
            list.updateContent();
            list.deselectAllRows();
            list.repaint();
        }

        void saveTo (int slot)
        {
            const int row = list.getSelectedRow();
            if (! isPositiveAndBelow (row, (int) shown.size()))
            {
                status.setText ("Select a headphone in the list first.", dontSendNotification);
                return;
            }
            const auto e = entries[(size_t) shown[(size_t) row]];
            const auto url = autoEqBase + e.path + "/" + e.path.fromLastOccurrenceOf ("/", false, false)
                             + "%20ParametricEQ.txt";
            const auto name = e.name + " (" + e.source.upToFirstOccurrenceOf (" on ", false, false) + ")";
            status.setText ("Downloading " + e.name + "...", dontSendNotification);
            Component::SafePointer<AutoEqSearch> safe (this);
            Thread::launch ([safe, url, name, slot]
            {
                const auto text = downloadText (url);
                MessageManager::callAsync ([safe, text, name, slot]
                {
                    if (safe == nullptr)
                        return;
                    if (parseAutoEq (text).isEmpty())
                    {
                        safe->status.setText ("Could not download this profile.", dontSendNotification);
                        return;
                    }
                    safe->owner.storeAutoEqSlot (slot, name, text);
                    safe->status.setText ("Saved to slot " + String (slot + 1) + ": " + name, dontSendNotification);
                });
            });
        }

        MainComponent& owner;
        TextEditor searchBox;
        ListBox list;
        Label status;
        OwnedArray<TextButton> saveButtons;
        TextButton refreshButton { "Refresh list" };
        ToggleButton showAllButton;
        Label saveLabel;
        std::vector<AutoEqEntry> entries;
        std::vector<int> shown;
    };

    void showAutoEqSearch()
    {
        DialogWindow::LaunchOptions o;
        o.content.setOwned (new AutoEqSearch (*this));
        o.dialogTitle = "AutoEQ - find your headphones";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    void showAudioSettings()
    {
        audioSettingsOpened = true;
        auto* selector = new AudioDeviceSelectorComponent (deviceManager, 0, 16, 2, 16, false, false, true, false);
        selector->setSize (540, 480);
        DialogWindow::LaunchOptions o;
        o.content.setOwned (selector);
        o.dialogTitle = "Audio settings";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    //==============================================================================
    struct Dot : public Component
    {
        Colour colour { grey };
        void paint (Graphics& g) override
        {
            g.setColour (colour);
            g.fillEllipse (getLocalBounds().toFloat().withSizeKeepingCentre (12.0f, 12.0f));
        }
    };

    std::unique_ptr<PropertiesFile> settings;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    HostEngine engine;
    std::unique_ptr<AudioPluginInstance> plugin;
    std::unique_ptr<AudioProcessorEditor> editor;
    std::unique_ptr<FileChooser> chooser;
    std::vector<PluginEntry> installedPlugins;
    AudioProcessor::BusesLayout pluginDefaultLayout;
    bool foldRearIntoSurround = false; // a 7.1 source feeds a plugin that only takes 5.1
    String activeLayoutBase;
    ToggleButton foldButton;

    KnownPluginList knownPlugins;
    std::unique_ptr<PluginDirectoryScanner> pluginScanner;
    String scanningPluginName;
    struct CallbackTimer : Timer
    {
        std::function<void()> callback;
        void timerCallback() override { if (callback) callback(); }
    } scanTimer;
    std::array<AxisParam, 3> params;
    String activeLayout, lastError, lastAddress;
    bool feedbackRisk = false, microphoneDenied = false, audioInitialised = false;
    bool safeMode = false, startupCleared = false, audioSettingsOpened = false;
    bool streamWasRunning = false, recovering = false;
    int restartAttempts = 0;
    uint32 lastCallbackCount = 0, lastCallbackTime = 0, lastRestartAttempt = 0, audioStartTime = 0, lastReconnectShown = 0;

    OSCReceiver receiver;
    int currentPort = 0;
    bool oscConnected = false;
    uint64 messagesReceived = 0, lastCountedMessages = 0, angleMessages = 0, lastCountedAngles = 0;
    uint32 lastAngleTime = 0, pluginLoadedTime = 0;

    Dot statusDot;
    Label statusLabel, detailLabel;
    Label pluginLabel { {}, "Plugin" }, portLabel { {}, "Bridgehead port" }, layoutLabel { {}, "Input" };
    ComboBox pluginBox, layoutBox;
    TextButton channelsButton { "Channels..." };
    ToggleButton speakerSimButton;
    Label routingLabel;
    ToggleButton trackingButton, autoEqButton;
    ComboBox autoEqSlotBox;
    TextButton autoEqSearchButton { "Edit..." };
    SidebarContent sidebar;
    Viewport sidebarViewport;
    Component pluginHolder;
    ComboBox zoomBox;
    Point<int> editorBaseSize;
    ToggleButton indicatorButton;
    HeadIndicator headIndicator;
    LevelMeters meters;
    std::array<float, 3> lastAngles {};
    struct SidebarText { Rectangle<int> area; String text; bool header; };
    std::vector<SidebarText> sidebarTexts;
    std::vector<Rectangle<int>> sidebarGroups, rowSeparators;
    static constexpr int sidebarWidth = 320;
    Rectangle<int> cards[3];
    static constexpr int captionWidth = 112;
    std::vector<int> inputMap; // device input channel for each plugin input channel, -1 = off
    TextButton browseButton { "Browse..." }, audioButton { "Audio settings..." };
    TextEditor portEditor;
    std::array<Label, 3> axisLabels;
    std::array<ComboBox, 3> axisBoxes;
    std::array<ToggleButton, 3> invertButtons;
    Viewport viewport;
    TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
// Dark look; toggle buttons marked with the "switch" property are drawn as on/off switches.
class HostLookAndFeel : public LookAndFeel_V4
{
public:
    HostLookAndFeel()
        : LookAndFeel_V4 (LookAndFeel_V4::ColourScheme (0xff1e1e1e, 0xff2c2c2e, 0xff2c2c2e, 0xff3a3a3c, 0xfff2f2f2,
                                                        0xff0a84ff, 0xffffffff, 0xff0a84ff, 0xfff2f2f2))
    {
       #if JUCE_WINDOWS
        setDefaultSansSerifTypefaceName ("Segoe UI");
       #elif JUCE_MAC
        setDefaultSansSerifTypefaceName ("Helvetica Neue");
       #endif
        setColour (TextButton::buttonColourId, Colour (0xff3a3a3c));
        setColour (ComboBox::backgroundColourId, Colour (0xff3a3a3c));
        setColour (ComboBox::outlineColourId, Colours::transparentBlack);
        setColour (TextEditor::backgroundColourId, Colour (0xff3a3a3c));
        setColour (TextEditor::outlineColourId, Colours::transparentBlack);
        setColour (ToggleButton::tickColourId, ui::accent);
        setColour (ScrollBar::thumbColourId, Colour (0xff5a5a5e));
    }

    int getDefaultScrollbarWidth() override { return 10; }
    Font getComboBoxFont (ComboBox&) override { return FontOptions (13.0f); }
    Font getTextButtonFont (TextButton&, int) override { return FontOptions (13.0f); }
    Font getPopupMenuFont() override { return FontOptions (13.0f); }

    void drawToggleButton (Graphics& g, ToggleButton& b, bool highlighted, bool down) override
    {
        if (! b.getProperties()["switch"])
        {
            LookAndFeel_V4::drawToggleButton (g, b, highlighted, down);
            return;
        }
        const bool on = b.getToggleState();
        auto bounds = b.getLocalBounds().toFloat();
        auto track = bounds.removeFromLeft (38.0f).withSizeKeepingCentre (38.0f, 20.0f);
        g.setColour (on ? ui::green : Colour (0xff48484a));
        g.fillRoundedRectangle (track, 10.0f);
        const float knob = 16.0f;
        const float x = on ? track.getRight() - knob - 2.0f : track.getX() + 2.0f;
        g.setColour (Colours::white.withAlpha (highlighted ? 1.0f : 0.92f));
        g.fillEllipse (x, track.getCentreY() - knob * 0.5f, knob, knob);
        g.setColour (b.findColour (ToggleButton::textColourId));
        g.setFont (FontOptions (14.0f));
        g.drawText (b.getButtonText(), bounds.withTrimmedLeft (8.0f), Justification::centredLeft);
    }
};

//==============================================================================
class HeadTrackingHostApp : public JUCEApplication
{
public:
    const String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override    { return false; }

    void initialise (const String&) override
    {
        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public DocumentWindow
    {
    public:
        explicit MainWindow (const String& name)
            : DocumentWindow (name, Colours::black, DocumentWindow::allButtons) // minimise, maximise/zoom, close
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true); // grows to fit the plugin when it is loaded
            setResizable (true, false);
            setResizeLimits (720, 480, 20000, 20000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            fitWindowToScreen (*this);
            // The native frame size is known only after the window is on screen: fit once more.
            Component::SafePointer<MainWindow> safe (this);
            MessageManager::callAsync ([safe] { if (safe != nullptr) fitWindowToScreen (*safe); });
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    HostLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (HeadTrackingHostApp)
