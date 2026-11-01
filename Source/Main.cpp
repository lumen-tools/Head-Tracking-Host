// Head Tracking Host
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hosts a VST3 plugin (e.g. APL Virtuoso) and turns three of its parameters (yaw, pitch, roll)
// with head tracking from Bridgehead (OSC /ypr in degrees). Driving the plugin's own knobs
// avoids the crackling some renderers produce on their OSC head tracking input.

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_osc/juce_osc.h>

#include <array>
#include <atomic>
#include <cmath>

using namespace juce;

namespace
{
constexpr int defaultOscPort = 8000;
constexpr int minWidth = 900;
constexpr int barHeight = 190;
const std::array<const char*, 3> axisNames { "Yaw", "Pitch", "Roll" };
const std::array<float, 3> fallbackRange { 180.0f, 90.0f, 90.0f }; // +/- degrees if the plugin text can't be parsed
const std::array<bool, 3> defaultInvert { false, true, false };     // matches Bridgehead "DaVinci Resolve" -> Virtuoso

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

std::unique_ptr<PropertiesFile> openSettings()
{
    PropertiesFile::Options o;
    o.applicationName     = "Head Tracking Host";
    o.folderName          = "Head Tracking Host";
    o.filenameSuffix      = ".settings";
    o.osxLibrarySubFolder = "Application Support";
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
    void setSwapSurrounds (bool s) { swapSurrounds = s; }
    bool wasTripped() const { return tripped; }
    void resetTrip() { loudSamples = 0; fadePosition = 0; tripped = false; }

    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        sampleRate = device->getCurrentSampleRate();
        const int blockSize = device->getCurrentBufferSizeSamples();

        // Soft start after every device (re)start: short silence, then fade in.
        silenceSamples = (int) (0.15 * sampleRate);
        fadeSamples    = (int) (0.35 * sampleRate);
        fadePosition   = 0;
        tripped        = false;
        loudSamples    = 0;
        tripSamples    = (int) (0.5 * sampleRate);

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

    void audioDeviceStopped() override
    {
        if (plugin != nullptr)
            plugin->releaseResources();
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const AudioIODeviceCallbackContext&) override
    {
        auto* p = plugin;
        if (p == nullptr || muted || tripped || numSamples > buffer.getNumSamples())
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
            // Optional fix for the common Windows 7.1 order: side (4/5) <-> rear (6/7) surrounds.
            int src = ch;
            if (swapSurrounds && numInputs >= 8 && pluginIns >= 8 && ch >= 4 && ch <= 7)
                src = ch < 6 ? ch + 2 : ch - 2;

            if (src < numInputs && ch < pluginIns && inputs[src] != nullptr)
                FloatVectorOperations::copy (block.getWritePointer (ch), inputs[src], numSamples);
            else
                block.clear (ch, 0, numSamples);
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
        checkRunaway (outputs, numOutputs, numSamples);
    }

private:
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

    // Emergency stop: if the output clips continuously (typical for feedback), mute until the
    // audio device is restarted (e.g. after changing the audio settings).
    void checkRunaway (float* const* outputs, int numOutputs, int numSamples)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                peak = jmax (peak, FloatVectorOperations::findMaximum (outputs[ch], numSamples),
                             -FloatVectorOperations::findMinimum (outputs[ch], numSamples));

        loudSamples = peak >= 1.0f ? loudSamples + numSamples : 0;
        if (loudSamples > tripSamples)
        {
            tripped = true;
            clearOutputs (outputs, numOutputs, numSamples);
        }
    }

    AudioPluginInstance* plugin = nullptr;
    std::atomic<bool> muted { false }, tripped { false }, swapSurrounds { false };
    int loudSamples = 0, tripSamples = 24000;
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
        audioButton.onClick = [this] { engine.resetTrip(); showAudioSettings(); };

        addAndMakeVisible (portLabel);
        addAndMakeVisible (portEditor);
        portEditor.setInputRestrictions (5, "0123456789");
        portEditor.setJustification (Justification::centred);
        portEditor.setText (String (settings->getIntValue ("oscPort", defaultOscPort)));
        portEditor.onReturnKey = portEditor.onFocusLost = [this] { applyPort(); };

        addAndMakeVisible (layoutLabel);
        addAndMakeVisible (layoutBox);
        int id = 1;
        for (auto& l : inputLayouts())
            layoutBox.addItem (l.name, id++);
        layoutBox.setSelectedItemIndex (jlimit (0, layoutBox.getNumItems() - 1, settings->getIntValue ("layout", 0)),
                                        dontSendNotification);
        layoutBox.onChange = [this] { settings->setValue ("layout", layoutBox.getSelectedItemIndex()); applyLayout(); };

        addAndMakeVisible (swapButton);
        swapButton.setButtonText ("Swap side/rear surrounds (Lss <-> Lrs, Rss <-> Rrs)");
        swapButton.setTooltip ("Fixes sources whose 7.1 side and rear surround channels arrive swapped (common on Windows).");
        swapButton.setToggleState (settings->getBoolValue ("swapSurrounds", false), dontSendNotification);
        engine.setSwapSurrounds (swapButton.getToggleState());
        swapButton.onClick = [this]
        {
            engine.setSwapSurrounds (swapButton.getToggleState());
            settings->setValue ("swapSurrounds", swapButton.getToggleState());
        };

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

        // Audio device (restores the last device, e.g. ASIO / FlexASIO or CoreAudio / BlackHole).
        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDevice"));
        const auto audioError = deviceManager.initialise (16, 16, savedAudio.get(), true);
        if (audioError.isNotEmpty())
            lastError = "Audio device: " + audioError;
        deviceManager.addChangeListener (this);
        deviceManager.addAudioCallback (&engine);
        checkFeedback();

        receiver.addListener (this);
        applyPort();

        refreshPluginList();
        const File lastPlugin (settings->getValue ("pluginPath"));
        if (lastPlugin.exists())
            loadPlugin (lastPlugin);

        startTimerHz (2);
    }

    ~MainComponent() override
    {
        saveState();
        deviceManager.removeChangeListener (this);
        receiver.removeListener (this);
        receiver.disconnect();
        unloadPlugin();
        deviceManager.removeAudioCallback (&engine);
    }

    void saveState()
    {
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
        swapButton.setBounds (bar.removeFromTop (24).withWidth (420));

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
                return;
            }
        }
        activeLayout = "plugin default (" + String (plugin->getTotalNumInputChannels()) + " in)";
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
    void changeListenerCallback (ChangeBroadcaster*) override { checkFeedback(); }

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

    void oscMessageReceived (const OSCMessage& m) override
    {
        ++messagesReceived;
        lastAddress = m.getAddressPattern().toString();

        if (m.size() != 3)
            return;
        std::array<float, 3> deg {};
        for (int i = 0; i < 3; ++i)
        {
            if (m[i].isFloat32())     deg[(size_t) i] = m[i].getFloat32();
            else if (m[i].isInt32())  deg[(size_t) i] = (float) m[i].getInt32();
            else return;
        }

        ++angleMessages;
        lastAngleTime = Time::getMillisecondCounter();
        for (size_t i = 0; i < 3; ++i)
        {
            auto& a = params[i];
            if (a.param == nullptr)
                continue;
            float d = invertButtons[i].getToggleState() ? -deg[i] : deg[i];
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
    void timerCallback() override
    {
        const auto now = Time::getMillisecondCounter();
        const int rate = (int) (angleMessages - lastCountedAngles) * 2;
        lastCountedAngles = angleMessages;
        const bool receivingAngles = now - lastAngleTime < 1000;
        const bool receivingAnything = messagesReceived != lastCountedMessages;
        lastCountedMessages = messagesReceived;

        Colour c = green;
        String status, detail;

        if (plugin == nullptr)
        {
            c = lastError.isNotEmpty() ? red : amber;
            status = "Choose a plugin";
            detail = lastError.isNotEmpty() ? lastError : "Pick it from the list or click \"Browse...\".";
        }
        else if (deviceManager.getCurrentAudioDevice() == nullptr)
        {
            c = red; status = "No audio device"; detail = "Click \"Audio settings...\" and choose your input and output.";
        }
        else if (engine.wasTripped())
        {
            c = red; status = "Output muted: continuous overload (feedback?)";
            detail = "Make sure the output is not your loopback device. Clicking \"Audio settings...\" unmutes with a soft fade-in.";
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
            detail = "Choose the Bridgehead profile \"DaVinci Resolve\".";
        }
        else
        {
            c = amber; status = "Waiting for Bridgehead on port " + String (currentPort);
            detail = "Bridgehead profile: DaVinci Resolve.  Input: " + activeLayout;
        }

        statusDot.colour = c;
        statusDot.repaint();
        statusLabel.setText (status, dontSendNotification);
        detailLabel.setText (detail, dontSendNotification);
    }

    //==============================================================================
    void showAudioSettings()
    {
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
    bool feedbackRisk = false;

    OSCReceiver receiver;
    int currentPort = 0;
    bool oscConnected = false;
    uint64 messagesReceived = 0, lastCountedMessages = 0, angleMessages = 0, lastCountedAngles = 0;
    uint32 lastAngleTime = 0;

    Dot statusDot;
    Label statusLabel, detailLabel;
    Label pluginLabel { {}, "Plugin" }, portLabel { {}, "Bridgehead port" }, layoutLabel { {}, "Input" };
    ComboBox pluginBox, layoutBox;
    ToggleButton swapButton;
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
