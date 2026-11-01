// Head Tracking Host
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Hosts the APL Virtuoso plugin and turns its "HT Yaw / HT Pitch / HT Roll" knobs with
// head tracking from Bridgehead (OSC /ypr in degrees). Turning the knobs does not crackle,
// unlike Virtuoso's own OSC head tracking input.

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_osc/juce_osc.h>

#include <array>
#include <atomic>
#include <cmath>

using namespace juce;

namespace
{
constexpr int defaultOscPort = 8000;
const std::array<const char*, 3> axisNames { "Yaw", "Pitch", "Roll" };
const std::array<float, 3> fallbackRange { 180.0f, 90.0f, 90.0f }; // ± degrees if the plugin text can't be parsed

const Colour green  (0xff1f9d55);
const Colour amber  (0xffd4a017);
const Colour red    (0xffd64545);
const Colour grey   (0xff8a8a8a);
const Colour panel  (0xff262626);

struct InputLayout
{
    const char* name;
    AudioChannelSet set;
};

std::vector<InputLayout> inputLayouts()
{
    return { { "7.1 (8 channels)",     AudioChannelSet::create7point1() },
             { "7.1.4 (12 channels)",  AudioChannelSet::create7point1point4() },
             { "5.1 (6 channels)",     AudioChannelSet::create5point1() },
             { "Stereo (2 channels)",  AudioChannelSet::stereo() } };
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

// Looks for the Virtuoso VST3 in the standard plugin folders.
File findVirtuoso()
{
    Array<File> roots;
   #if JUCE_WINDOWS
    roots.add (File::getSpecialLocation (File::globalApplicationsDirectory).getChildFile ("Common Files/VST3"));
    roots.add (File ("C:\\Program Files\\Common Files\\VST3"));
   #elif JUCE_MAC
    roots.add (File ("/Library/Audio/Plug-Ins/VST3"));
    roots.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3"));
   #else
    roots.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3"));
    roots.add (File ("/usr/lib/vst3"));
   #endif

    for (auto& root : roots)
    {
        if (! root.isDirectory())
            continue;

        for (const auto& entry : RangedDirectoryIterator (root, true, "*.vst3", File::findFilesAndDirectories))
        {
            auto f = entry.getFile();
            if (f.getFileName().containsIgnoreCase ("virtuoso"))
                return f;
        }
    }
    return {};
}
} // namespace

//==============================================================================
// Runs the plugin on the audio thread.
class HostEngine : public AudioIODeviceCallback
{
public:
    void setPlugin (AudioPluginInstance* p) { plugin = p; }

    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        if (plugin == nullptr)
            return;

        sampleRate = device->getCurrentSampleRate();
        blockSize  = device->getCurrentBufferSizeSamples();
        plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin->prepareToPlay (sampleRate, blockSize);

        const int channels = jmax (jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()),
                                   jmax (device->getActiveInputChannels().countNumberOfSetBits(),
                                         device->getActiveOutputChannels().countNumberOfSetBits()),
                                   2);
        buffer.setSize (channels, jmax (blockSize, 4096));
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
        if (p == nullptr || numSamples > buffer.getNumSamples())
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch] != nullptr)
                    FloatVectorOperations::clear (outputs[ch], numSamples);
            return;
        }

        const int pluginIns  = p->getTotalNumInputChannels();
        const int pluginOuts = p->getTotalNumOutputChannels();
        const int channels   = jmin (buffer.getNumChannels(), jmax (pluginIns, pluginOuts));

        AudioBuffer<float> block (buffer.getArrayOfWritePointers(), channels, numSamples);

        for (int ch = 0; ch < channels; ++ch)
        {
            if (ch < numInputs && ch < pluginIns && inputs[ch] != nullptr)
                FloatVectorOperations::copy (block.getWritePointer (ch), inputs[ch], numSamples);
            else
                block.clear (ch, 0, numSamples);
        }

        midi.clear();
        p->processBlock (block, midi);

        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            if (ch < pluginOuts && ch < channels)
                FloatVectorOperations::copy (outputs[ch], block.getReadPointer (ch), numSamples);
            else
                FloatVectorOperations::clear (outputs[ch], numSamples);
        }
    }

private:
    AudioPluginInstance* plugin = nullptr;
    AudioBuffer<float> buffer;
    MidiBuffer midi;
    double sampleRate = 48000.0;
    int blockSize = 512;
};

//==============================================================================
class MainComponent : public Component,
                      private OSCReceiver::Listener<OSCReceiver::MessageLoopCallback>,
                      private ComponentListener,
                      private Timer
{
public:
    MainComponent()
    {
        settings = openSettings();
        addDefaultFormatsToManager (formatManager);

        addAndMakeVisible (statusDot);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (detailLabel);
        statusLabel.setFont (FontOptions (15.0f, Font::bold));
        detailLabel.setFont (FontOptions (13.0f));
        detailLabel.setColour (Label::textColourId, Colours::lightgrey);

        for (auto* b : { &audioButton, &pluginButton })
            addAndMakeVisible (b);
        audioButton.onClick  = [this] { showAudioSettings(); };
        pluginButton.onClick = [this] { choosePlugin(); };

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

        addAndMakeVisible (invertLabel);
        for (size_t i = 0; i < invertButtons.size(); ++i)
        {
            auto& b = invertButtons[i];
            b.setButtonText (axisNames[i]);
            b.setToggleState (settings->getBoolValue (String ("invert") + axisNames[i], false), dontSendNotification);
            b.onClick = [this, i] { settings->setValue (String ("invert") + axisNames[i], invertButtons[i].getToggleState()); };
            addAndMakeVisible (b);
        }

        // Audio device (restores the last device, e.g. ASIO / FlexASIO or CoreAudio / BlackHole).
        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDevice"));
        const auto audioError = deviceManager.initialise (16, 16, savedAudio.get(), true);
        if (audioError.isNotEmpty())
            lastError = "Audio device: " + audioError;

        receiver.addListener (this);
        applyPort();

        auto pluginFile = File (settings->getValue ("pluginPath"));
        if (! pluginFile.exists())
            pluginFile = findVirtuoso();
        if (pluginFile.exists())
            loadPlugin (pluginFile);
        else
            lastError = "Virtuoso VST3 not found - click \"Choose plugin...\".";

        setSize (760, barHeight + 420);
        startTimerHz (2);
    }

    ~MainComponent() override
    {
        saveState();
        receiver.removeListener (this);
        receiver.disconnect();
        unloadPlugin();
    }

    void saveState()
    {
        if (auto xml = deviceManager.createStateXml())
            settings->setValue ("audioDevice", xml.get());
        if (plugin != nullptr)
        {
            MemoryBlock state;
            plugin->getStateInformation (state);
            settings->setValue ("pluginState", state.toBase64Encoding());
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
        auto bar = getLocalBounds().removeFromTop (barHeight).reduced (14, 10);

        auto row1 = bar.removeFromTop (24);
        statusDot.setBounds (row1.removeFromLeft (20));
        statusLabel.setBounds (row1);
        detailLabel.setBounds (bar.removeFromTop (22));
        bar.removeFromTop (8);

        auto row2 = bar.removeFromTop (28);
        audioButton.setBounds (row2.removeFromLeft (130));
        row2.removeFromLeft (8);
        pluginButton.setBounds (row2.removeFromLeft (130));
        row2.removeFromLeft (16);
        portLabel.setBounds (row2.removeFromLeft (118));
        portEditor.setBounds (row2.removeFromLeft (64));
        row2.removeFromLeft (16);
        layoutLabel.setBounds (row2.removeFromLeft (44));
        layoutBox.setBounds (row2.removeFromLeft (170));

        bar.removeFromTop (8);
        auto row3 = bar.removeFromTop (24);
        invertLabel.setBounds (row3.removeFromLeft (56));
        for (auto& b : invertButtons)
            b.setBounds (row3.removeFromLeft (80));

        if (editor != nullptr)
            editor->setTopLeftPosition (0, barHeight);
    }

private:
    static constexpr int barHeight = 140;

    //==============================================================================
    // Plugin
    void choosePlugin()
    {
        chooser = std::make_unique<FileChooser> ("Choose the APL Virtuoso VST3", findVirtuoso().getParentDirectory(), "*.vst3");
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
        deviceManager.removeAudioCallback (&engine);
        engine.setPlugin (nullptr);
        if (editor != nullptr)
        {
            editor->removeComponentListener (this);
            editor.reset();
        }
        plugin.reset();
        params = {};
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
            lastError = "Not a usable VST3 plugin: " + file.getFullPathName();
            return;
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        const double sr = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
        const int bs    = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

        String error;
        plugin = formatManager.createPluginInstance (*types[0], sr, bs, error);
        if (plugin == nullptr)
        {
            lastError = "Could not load plugin: " + error;
            return;
        }

        const bool samePlugin = settings->getValue ("pluginPath") == file.getFullPathName();
        settings->setValue ("pluginPath", file.getFullPathName());

        if (samePlugin)
        {
            MemoryBlock state;
            if (state.fromBase64Encoding (settings->getValue ("pluginState")) && state.getSize() > 0)
                plugin->setStateInformation (state.getData(), (int) state.getSize());
        }

        setPluginLayout();
        findHeadTrackingParameters();

        engine.setPlugin (plugin.get());
        deviceManager.addAudioCallback (&engine);

        if (plugin->hasEditor())
        {
            editor.reset (plugin->createEditorIfNeeded());
            if (editor != nullptr)
            {
                addAndMakeVisible (*editor);
                editor->addComponentListener (this);
                componentMovedOrResized (*editor, false, true);
            }
        }
        lastError = {};
    }

    void setPluginLayout()
    {
        auto layouts = inputLayouts();
        const int wanted = jlimit (0, (int) layouts.size() - 1, layoutBox.getSelectedItemIndex());

        // Try the chosen input layout first, then the others; output is always stereo.
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

    struct AxisParam
    {
        AudioProcessorParameter* param = nullptr;
        float low = 0.0f, high = 0.0f; // degrees at normalised 0 and 1
    };

    void findHeadTrackingParameters()
    {
        params = {};
        for (auto* p : plugin->getParameters())
        {
            const auto name = p->getName (64);
            for (size_t i = 0; i < axisNames.size(); ++i)
            {
                if (! name.containsIgnoreCase (axisNames[i]))
                    continue;
                // Prefer "HT Yaw" etc.; take a plain "Yaw" only if nothing better was found.
                if (params[i].param != nullptr && ! name.containsIgnoreCase ("HT"))
                    continue;

                auto& a = params[i];
                a.param = p;
                a.low  = p->getText (0.0f, 32).retainCharacters ("-0123456789.").getFloatValue();
                a.high = p->getText (1.0f, 32).retainCharacters ("-0123456789.").getFloatValue();
                if (std::abs (a.high - a.low) < 1.0f)
                {
                    a.low = -fallbackRange[i];
                    a.high = fallbackRange[i];
                }
            }
        }
    }

    bool allParamsFound() const
    {
        return params[0].param != nullptr && params[1].param != nullptr && params[2].param != nullptr;
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

        angleMessages++;
        lastAngleTime = Time::getMillisecondCounter();
        for (size_t i = 0; i < 3; ++i)
        {
            auto& a = params[i];
            if (a.param == nullptr)
                continue;
            const float d = invertButtons[i].getToggleState() ? -deg[i] : deg[i];
            const float norm = jlimit (0.0f, 1.0f, (d - a.low) / (a.high - a.low));
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
            c = red; status = "Virtuoso not loaded"; detail = lastError;
        }
        else if (deviceManager.getCurrentAudioDevice() == nullptr)
        {
            c = red; status = "No audio device"; detail = "Click \"Audio settings...\" and choose your input and output.";
        }
        else if (! oscConnected)
        {
            c = red; status = "Port " + String (currentPort) + " is in use";
            detail = "Close Virtuoso standalone or other programs listening on this port.";
        }
        else if (! allParamsFound())
        {
            c = red; status = "HT Yaw / HT Pitch / HT Roll not found in this plugin";
        }
        else if (receivingAngles)
        {
            status = "Head tracking active (" + String (rate) + "/s)";
            detail = "Set Head tracking in Virtuoso to Manual.  Input: " + activeLayout;
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

        if (lastError.isNotEmpty() && plugin != nullptr)
            detail = lastError;

        statusDot.colour = c;
        statusDot.repaint();
        statusLabel.setText (status, dontSendNotification);
        detailLabel.setText (detail, dontSendNotification);
    }

    //==============================================================================
    void showAudioSettings()
    {
        auto* selector = new AudioDeviceSelectorComponent (deviceManager, 0, 16, 2, 16, false, false, true, false);
        selector->setSize (520, 460);
        DialogWindow::LaunchOptions o;
        o.content.setOwned (selector);
        o.dialogTitle = "Audio settings";
        o.dialogBackgroundColour = panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void componentMovedOrResized (Component& c, bool, bool wasResized) override
    {
        if (&c == editor.get() && wasResized)
            setSize (jmax (760, editor->getWidth()), barHeight + editor->getHeight());
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
    std::array<AxisParam, 3> params;
    String activeLayout, lastError, lastAddress;

    OSCReceiver receiver;
    int currentPort = 0;
    bool oscConnected = false;
    uint64 messagesReceived = 0, lastCountedMessages = 0, angleMessages = 0, lastCountedAngles = 0;
    uint32 lastAngleTime = 0;

    Dot statusDot;
    Label statusLabel, detailLabel;
    TextButton audioButton { "Audio settings..." }, pluginButton { "Choose plugin..." };
    Label portLabel { {}, "Bridgehead port" }, layoutLabel { {}, "Input" }, invertLabel { {}, "Invert" };
    TextEditor portEditor;
    ComboBox layoutBox;
    std::array<ToggleButton, 3> invertButtons;

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
            setContentOwned (new MainComponent(), true);
            setResizable (false, false);
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
