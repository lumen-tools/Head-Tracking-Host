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

using namespace juce;

#if JUCE_MAC
void requestMicrophoneAccess (std::function<void (bool)> callback); // MicrophonePermission.mm
#endif

namespace
{
constexpr int defaultOscPort = 9000; // Bridgehead profile "IEM Scene Rotator YPR"
constexpr int minWidth = 900;
constexpr int barHeight = 190;
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
// Runs the plugin on the audio thread.
class HostEngine : public AudioIODeviceCallback
{
public:
    void setPlugin (AudioPluginInstance* p) { plugin = p; }
    void setMuted (bool m) { muted = m; }
    static constexpr int maxChannels = 32;
    // Which device input channel feeds each plugin input channel (-1 = silent).
    void setBypass (bool b) { bypass = b; }

    // Stereo downmix gains per plugin input channel, used while bypassed (set while stopped).
    void setDownmixGains (const std::vector<std::pair<float, float>>& gains)
    {
        for (int i = 0; i < maxChannels; ++i)
            downmix[(size_t) i] = i < (int) gains.size() ? gains[(size_t) i] : std::pair<float, float> { 0.0f, 0.0f };
    }

    void setInputSource (int pluginChannel, int deviceChannel)
    {
        if (isPositiveAndBelow (pluginChannel, maxChannels))
            inputMap[(size_t) pluginChannel] = deviceChannel;
    }

    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        sampleRate = device->getCurrentSampleRate();
        const int blockSize = device->getCurrentBufferSizeSamples();

        // Soft start after every device (re)start: short silence, then fade in.
        silenceSamples = (int) (0.15 * sampleRate);
        fadeSamples    = (int) (2.0 * sampleRate);  // 2 s fade-in
        fadePosition   = 0;

        const int deviceChannels = jmax (device->getActiveInputChannels().countNumberOfSetBits(),
                                         device->getActiveOutputChannels().countNumberOfSetBits());
        int channels = jmax (deviceChannels, 2);

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
        if (p == nullptr || muted || numSamples > buffer.getNumSamples())
        {
            clearOutputs (outputs, numOutputs, numSamples);
            return;
        }

        const int pluginIns  = p->getTotalNumInputChannels();
        const int pluginOuts = p->getTotalNumOutputChannels();
        const int channels   = jmin (buffer.getNumChannels(), jmax (pluginIns, pluginOuts));

        AudioBuffer<float> block (buffer.getArrayOfWritePointers(), channels, numSamples);

        for (int ch = 0; ch < channels; ++ch)
        {
            const int src = ch < maxChannels ? inputMap[(size_t) ch].load() : ch;

            if (isPositiveAndBelow (src, numInputs) && ch < pluginIns && inputs[src] != nullptr)
                FloatVectorOperations::copy (block.getWritePointer (ch), inputs[src], numSamples);
            else
                block.clear (ch, 0, numSamples);
        }

        if (bypass)
        {
            // Dry signal for A/B comparisons: the routed input, downmixed to stereo.
            writeDownmix (block, jmin (channels, pluginIns), outputs, numOutputs, numSamples);
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
    std::atomic<bool> muted { false }, bypass { false }, deviceError { false };
    std::atomic<uint32> callbackCount { 0 };
    std::array<std::pair<float, float>, maxChannels> downmix {};
    struct InputMap
    {
        InputMap() { for (int i = 0; i < maxChannels; ++i) channels[(size_t) i] = i; }
        std::atomic<int>& operator[] (size_t i) { return channels[i]; }
        std::array<std::atomic<int>, maxChannels> channels;
    };
    InputMap inputMap;
    AudioBuffer<float> buffer;
    MidiBuffer midi;
    double sampleRate = 48000.0;
    int silenceSamples = 0, fadeSamples = 1, fadePosition = 0;
};

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

        addAndMakeVisible (statusDot);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (detailLabel);
        statusLabel.setFont (FontOptions (15.0f, Font::bold));
        detailLabel.setFont (FontOptions (13.0f));
        detailLabel.setColour (Label::textColourId, Colours::lightgrey);

        // Row 2: plugin, audio, port, input layout
        addAndMakeVisible (pluginLabel);
        addAndMakeVisible (pluginBox);
        pluginBox.setTextWhenNothingSelected ("Choose a plugin...");
        pluginBox.onChange = [this] { pluginBoxChanged(); };
        addAndMakeVisible (browseButton);
        browseButton.onClick = [this] { browseForPlugin(); };
        addAndMakeVisible (audioButton);
        audioButton.onClick = [this] { showAudioSettings(); };

        addAndMakeVisible (portLabel);
        addAndMakeVisible (portEditor);
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

        addAndMakeVisible (layoutLabel);
        addAndMakeVisible (layoutBox);
        int id = 1;
        for (auto& l : inputLayouts())
            layoutBox.addItem (l.name, id++);
        layoutBox.setSelectedItemIndex (jlimit (0, layoutBox.getNumItems() - 1, settings->getIntValue ("layout", 0)),
                                        dontSendNotification);
        layoutBox.onChange = [this] { settings->setValue ("layout", layoutBox.getSelectedItemIndex()); applyLayout(); };

        addAndMakeVisible (bypassButton);
        bypassButton.setClickingTogglesState (true);
        bypassButton.setColour (TextButton::buttonOnColourId, amber.darker (0.3f));
        bypassButton.setTooltip ("Hear the input without the plugin (7.1 is downmixed to stereo) for A/B comparisons.");
        bypassButton.onClick = [this] { engine.setBypass (bypassButton.getToggleState()); };

        addAndMakeVisible (channelsButton);
        channelsButton.onClick = [this] { showChannelRouting(); };
        addAndMakeVisible (routingLabel);
        routingLabel.setColour (Label::textColourId, Colours::lightgrey);
        loadInputMap();

        // Row 3: which plugin parameter is yaw / pitch / roll
        for (size_t i = 0; i < 3; ++i)
        {
            axisLabels[i].setText (String (axisNames[i]) + ":", dontSendNotification);
            axisLabels[i].setJustificationType (Justification::centredRight);
            addAndMakeVisible (axisLabels[i]);

            axisBoxes[i].setTextWhenNothingSelected ("- none -");
            axisBoxes[i].onChange = [this, i] { axisBoxChanged (i); };
            addAndMakeVisible (axisBoxes[i]);

            invertButtons[i].setButtonText ("Invert");
            invertButtons[i].onClick = [this, i] { settings->setValue (pluginKey (String ("invert") + axisNames[i]),
                                                                       invertButtons[i].getToggleState()); };
            addAndMakeVisible (invertButtons[i]);
        }

        addAndMakeVisible (viewport);
        viewport.setScrollBarsShown (true, true, true, true);

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

        refreshPluginList();
        const File lastPlugin (settings->getValue ("pluginPath"));
        if (safeMode)
            pluginBox.setSelectedId (0, dontSendNotification); // choosing it again loads it
        else if (lastPlugin.exists())
            loadPlugin (lastPlugin);

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
        g.fillAll (Colours::black);
        g.setColour (panel);
        g.fillRect (getLocalBounds().removeFromTop (barHeight));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromTop (barHeight).reduced (14, 10);

        auto row1 = bar.removeFromTop (24);
        statusDot.setBounds (row1.removeFromLeft (20));
        statusLabel.setBounds (row1);
        detailLabel.setBounds (bar.removeFromTop (22));
        bar.removeFromTop (8);

        auto row2 = bar.removeFromTop (28);
        pluginLabel.setBounds (row2.removeFromLeft (52));
        pluginBox.setBounds (row2.removeFromLeft (210));
        row2.removeFromLeft (6);
        browseButton.setBounds (row2.removeFromLeft (82));
        row2.removeFromLeft (14);
        audioButton.setBounds (row2.removeFromLeft (128));
        row2.removeFromLeft (14);
        portLabel.setBounds (row2.removeFromLeft (112));
        portEditor.setBounds (row2.removeFromLeft (58));
        row2.removeFromLeft (14);
        layoutLabel.setBounds (row2.removeFromLeft (42));
        layoutBox.setBounds (row2.removeFromLeft (jmin (175, row2.getWidth())));

        bar.removeFromTop (10);
        auto row3 = bar.removeFromTop (28);
        const int column = row3.getWidth() / 3;
        for (size_t i = 0; i < 3; ++i)
        {
            auto c = row3.removeFromLeft (column).withTrimmedRight (12);
            axisLabels[i].setBounds (c.removeFromLeft (48));
            invertButtons[i].setBounds (c.removeFromRight (72));
            axisBoxes[i].setBounds (c.withTrimmedRight (6));
        }

        bar.removeFromTop (8);
        auto row4 = bar.removeFromTop (26);
        bypassButton.setBounds (row4.removeFromLeft (110));
        row4.removeFromLeft (10);
        channelsButton.setBounds (row4.removeFromLeft (110));
        row4.removeFromLeft (10);
        routingLabel.setBounds (row4);

        viewport.setBounds (area);
    }

private:
    //==============================================================================
    // Window size follows the plugin editor, limited to the screen.
    void updateSize()
    {
        int w = minWidth, h = 360;
        if (editor != nullptr)
        {
            w = jmax (minWidth, editor->getWidth());
            h = editor->getHeight();
        }

        auto screen = Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        if (auto* top = getTopLevelComponent(); top != nullptr && top->isOnDesktop())
            if (auto* d = Desktop::getInstance().getDisplays().getDisplayForRect (top->getScreenBounds()))
                screen = d->userArea;

        const int maxW = screen.getWidth() - 20;
        const int maxH = screen.getHeight() - 60; // room for the title bar
        const int width  = jmin (w, maxW);
        int height = jmin (barHeight + h, maxH);
        if (width < w && height < maxH)
            height = jmin (height + viewport.getScrollBarThickness(), maxH);
        setSize (width, height);
    }

    void componentMovedOrResized (Component& c, bool, bool wasResized) override
    {
        if (&c == editor.get() && wasResized)
            updateSize();
    }

    //==============================================================================
    // Plugin selection
    String pluginKey (const String& name) const
    {
        return "plugin." + File (settings->getValue ("pluginPath")).getFileNameWithoutExtension().removeCharacters (" .") + "." + name;
    }

    void refreshPluginList()
    {
        installedPlugins = findInstalledPlugins();
        pluginBox.clear (dontSendNotification);
        int id = 1;
        for (auto& f : installedPlugins)
            pluginBox.addItem (f.getFileNameWithoutExtension(), id++);

        const File current (settings->getValue ("pluginPath"));
        const int index = installedPlugins.indexOf (current);
        if (index >= 0)
            pluginBox.setSelectedItemIndex (index, dontSendNotification);
        else if (current.exists())
            pluginBox.setText (current.getFileNameWithoutExtension(), dontSendNotification);
    }

    void pluginBoxChanged()
    {
        const int index = pluginBox.getSelectedItemIndex();
        if (isPositiveAndBelow (index, installedPlugins.size()))
            loadPlugin (installedPlugins[index]);
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
        if (editor != nullptr)
        {
            editor->removeComponentListener (this);
            viewport.setViewedComponent (nullptr, false);
            editor.reset();
        }
        plugin.reset();
        params = {};
        for (auto& box : axisBoxes)
            box.clear (dontSendNotification);
    }

    void loadPlugin (const File& file)
    {
        if (plugin != nullptr)
            saveState();
        unloadPlugin();

        OwnedArray<PluginDescription> types;
        for (auto* format : formatManager.getFormats())
            if (format->fileMightContainThisPluginType (file.getFullPathName()))
                format->findAllTypesForFile (types, file.getFullPathName());

        if (types.isEmpty())
        {
            lastError = "Not a usable VST3 plugin: " + file.getFileName();
            deviceManager.addAudioCallback (&engine);
            updateSize();
            return;
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        const double sr = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
        const int bs    = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

        String error;
        plugin = formatManager.createPluginInstance (*types[0], sr, bs, error);
        if (plugin == nullptr)
        {
            lastError = "Could not load " + file.getFileName() + ": " + error;
            deviceManager.addAudioCallback (&engine);
            updateSize();
            return;
        }

        settings->setValue ("pluginPath", file.getFullPathName());
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
                viewport.setViewedComponent (editor.get(), false);
                editor->addComponentListener (this);
            }
        }
        lastError = {};
        updateSize();
    }

    void setPluginLayout()
    {
        auto layouts = inputLayouts();
        const int wanted = jlimit (0, (int) layouts.size() - 1, layoutBox.getSelectedItemIndex());

        std::vector<int> order { wanted };
        for (int i = 0; i < (int) layouts.size(); ++i)
            if (i != wanted)
                order.push_back (i);

        for (int i : order)
        {
            AudioProcessor::BusesLayout l;
            l.inputBuses.add (layouts[(size_t) i].set);
            l.outputBuses.add (AudioChannelSet::stereo());
            if (plugin->setBusesLayout (l))
            {
                activeLayout = layouts[(size_t) i].name;
                updateDownmix();
                return;
            }
        }
        activeLayout = "plugin default (" + String (plugin->getTotalNumInputChannels()) + " in)";
        updateDownmix();
    }

    // Common stereo downmix: front L/R at 0 dB, centre and all other channels at -3 dB to their
    // side, LFE left out; scaled down so typical material does not clip.
    void updateDownmix()
    {
        std::vector<std::pair<float, float>> gains;
        const auto names = pluginChannelNames();
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
    }

    void applyLayout()
    {
        if (plugin == nullptr)
            return;
        deviceManager.removeAudioCallback (&engine);
        setPluginLayout();
        deviceManager.addAudioCallback (&engine);
    }

    //==============================================================================
    // Yaw / pitch / roll parameter selection (saved per plugin)
    struct AxisParam
    {
        AudioProcessorParameter* param = nullptr;
        float low = 0.0f, high = 0.0f; // degrees at normalised 0 and 1
    };

    void setupAxisSelection()
    {
        const auto& all = plugin->getParameters();
        for (size_t i = 0; i < 3; ++i)
        {
            auto& box = axisBoxes[i];
            box.clear (dontSendNotification);
            for (int p = 0; p < all.size(); ++p)
                box.addItem (String (p) + " - " + all[p]->getName (64), p + 1);

            // Saved choice, otherwise guess by name ("HT Yaw" preferred over plain "Yaw").
            const auto saved = settings->getValue (pluginKey (String ("param") + axisNames[i]));
            int chosen = -1;
            for (int p = 0; p < all.size() && chosen < 0; ++p)
                if (saved.isNotEmpty() && all[p]->getName (64) == saved)
                    chosen = p;
            if (saved.isEmpty())
            {
                for (int p = 0; p < all.size(); ++p)
                {
                    const auto name = all[p]->getName (64);
                    if (name.containsIgnoreCase (axisNames[i]) && (chosen < 0 || name.containsIgnoreCase ("HT")))
                        chosen = p;
                }
            }
            box.setSelectedId (chosen + 1, dontSendNotification);
            selectAxisParam (i, chosen);

            const auto invertKey = pluginKey (String ("invert") + axisNames[i]);
            invertButtons[i].setToggleState (settings->getBoolValue (invertKey, defaultInvert[i]), dontSendNotification);
        }
    }

    void axisBoxChanged (size_t axis)
    {
        if (plugin == nullptr)
            return;
        const int p = axisBoxes[axis].getSelectedId() - 1;
        selectAxisParam (axis, p);
        const auto& all = plugin->getParameters();
        settings->setValue (pluginKey (String ("param") + axisNames[axis]),
                            isPositiveAndBelow (p, all.size()) ? all[p]->getName (64) : String());
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
        if (port == currentPort && oscConnected)
            return;
        receiver.disconnect();
        currentPort = port;
        oscConnected = receiver.connect (port);
        settings->setValue ("oscPort", port);
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
        {
            auto& a = params[i];
            if (a.param == nullptr)
                return;
            float d = invertButtons[i].getToggleState() ? -rawDegrees : rawDegrees;
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
        const bool stalled = device != nullptr && streamWasRunning && now - lastCallbackTime > 2000;
        const bool lost    = device == nullptr && streamWasRunning;
        const bool stillDown = recovering && now - lastCallbackTime > 2000;

        static constexpr uint32 backoff[] = { 3000, 10000, 30000 };
        const auto wait = backoff[jmin (restartAttempts, 2)];

        if (inGrace || restartAttempts >= 5 || now - lastRestartAttempt < wait)
            return;
        if (! (stalled || lost || stillDown || (error && streamWasRunning)))
            return;

        lastRestartAttempt = now;
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

        Colour c = green;
        String status, detail;

        if (plugin == nullptr && safeMode)
        {
            c = amber; status = "Safe start - the last start did not complete";
            detail = "The saved audio device and plugin were not opened. Choose them again - your settings are kept.";
        }
        else if (plugin == nullptr)
        {
            c = lastError.isNotEmpty() ? red : amber;
            status = "Choose a plugin";
            detail = lastError.isNotEmpty() ? lastError : "Pick it from the list or click \"Browse...\".";
        }
        else if (recovering && restartAttempts >= 5 && now - lastCallbackTime > 2000)
        {
            c = red; status = "Audio device not responding";
            detail = "Open \"Audio settings...\" and choose the device again.";
        }
        else if (recovering && now - lastCallbackTime > 1000)
        {
            c = amber; status = "Reconnecting audio device...";
            detail = "The audio stream stopped (e.g. power saving or a sample rate change). Retrying automatically.";
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
        else if (bypassButton.getToggleState())
        {
            c = amber; status = "Bypassed - you hear the dry input";
            detail = "Click \"Bypass (dry)\" again to hear the plugin.";
        }
        else if (feedbackRisk)
        {
            c = red; status = "Output muted: input and output are the same loopback device";
            detail = "In \"Audio settings...\" choose your headphones or interface as output.";
        }
        else if (! oscConnected)
        {
            c = red; status = "Port " + String (currentPort) + " is in use";
            detail = "Close other programs listening on this port (e.g. a standalone renderer with OSC head tracking).";
        }
        else if (mappedAxes() == 0)
        {
            c = amber; status = "Choose the plugin parameters for Yaw, Pitch and Roll";
        }
        else if (receivingAngles)
        {
            status = "Head tracking active (" + String (rate) + "/s)";
            detail = "Turn off the plugin's own head tracking input (e.g. Virtuoso: Manual).  Input: " + activeLayout;
        }
        else if (receivingAnything)
        {
            c = amber; status = "Receiving " + lastAddress + " - but no yaw/pitch/roll";
            detail = "Choose the Bridgehead profile \"IEM Scene Rotator YPR\".";
        }
        else
        {
            c = amber; status = "Waiting for Bridgehead on port " + String (currentPort);
            detail = "Bridgehead profile: IEM Scene Rotator YPR.  Input: " + activeLayout;
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

        routingLabel.setText (inputMap.empty() ? "Input routing: 1:1" : "Input routing: custom", dontSendNotification);
    }

    StringArray pluginChannelNames() const
    {
        StringArray names;
        if (plugin != nullptr && plugin->getBusCount (true) > 0)
        {
            const auto layout = plugin->getBus (true, 0)->getCurrentLayout();
            for (int i = 0; i < layout.size(); ++i)
                names.add (AudioChannelSet::getAbbreviatedChannelTypeName (layout.getTypeOfChannel (i)));
        }
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
        o.resizable = false;
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
        o.resizable = false;
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
    Array<File> installedPlugins;
    std::array<AxisParam, 3> params;
    String activeLayout, lastError, lastAddress;
    bool feedbackRisk = false, microphoneDenied = false, audioInitialised = false;
    bool safeMode = false, startupCleared = false, audioSettingsOpened = false;
    bool streamWasRunning = false, recovering = false;
    int restartAttempts = 0;
    uint32 lastCallbackCount = 0, lastCallbackTime = 0, lastRestartAttempt = 0, audioStartTime = 0;

    OSCReceiver receiver;
    int currentPort = 0;
    bool oscConnected = false;
    uint64 messagesReceived = 0, lastCountedMessages = 0, angleMessages = 0, lastCountedAngles = 0;
    uint32 lastAngleTime = 0;

    Dot statusDot;
    Label statusLabel, detailLabel;
    Label pluginLabel { {}, "Plugin" }, portLabel { {}, "Bridgehead port" }, layoutLabel { {}, "Input" };
    ComboBox pluginBox, layoutBox;
    TextButton channelsButton { "Channels..." }, bypassButton { "Bypass (dry)" };
    Label routingLabel;
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
            : DocumentWindow (name, Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setResizable (false, false);
            setContentOwned (new MainComponent(), true); // window follows the content size
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    LookAndFeel_V4 lookAndFeel { LookAndFeel_V4::getDarkColourScheme() };
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (HeadTrackingHostApp)
