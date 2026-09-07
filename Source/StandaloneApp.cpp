#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JucePlugin_Build_Standalone

namespace juce
{

class NinjamStandalonePluginHolder : public StandalonePluginHolder
{
public:
    using StandalonePluginHolder::StandalonePluginHolder;
};

class NinjamAudioSettingsComponent : public Component
{
public:
    NinjamAudioSettingsComponent (NinjamStandalonePluginHolder& pluginHolder,
                                  AudioDeviceManager& deviceManagerToUse,
                                  int maxAudioInputChannels,
                                  int maxAudioOutputChannels)
        : owner (pluginHolder),
          deviceSelector (deviceManagerToUse,
                          1, maxAudioInputChannels,
                          0, maxAudioOutputChannels,
                          true,
                          (pluginHolder.processor.get() != nullptr && pluginHolder.processor->producesMidi()),
                          true, false),
          shouldMuteLabel ("Feedback Loop:", "Feedback Loop:"),
          shouldMuteButton ("Mute audio input")
    {
        setOpaque (true);

        shouldMuteButton.setClickingTogglesState (true);
        shouldMuteButton.getToggleStateValue().referTo (owner.shouldMuteInput);

        addAndMakeVisible (deviceSelector);

        if (owner.getProcessorHasPotentialFeedbackLoop())
        {
            addAndMakeVisible (shouldMuteButton);
            addAndMakeVisible (shouldMuteLabel);
            shouldMuteLabel.attachToComponent (&shouldMuteButton, true);
        }

        processor = dynamic_cast<NinjamVst3AudioProcessor*> (owner.processor.get());

        if (processor != nullptr)
        {
            addAndMakeVisible (mtcOutLabel);
            mtcOutLabel.setText ("MTC Out:", dontSendNotification);
            mtcOutLabel.attachToComponent (&mtcToggle, true);

            addAndMakeVisible (mtcToggle);
            mtcToggle.setClickingTogglesState (true);
            mtcToggle.setButtonText ("Enable");
            mtcToggle.setToggleState (processor->isMtcOutputEnabled(), dontSendNotification);
            mtcToggle.onClick = [this]
            {
                if (processor != nullptr)
                    processor->setMtcOutputEnabled (mtcToggle.getToggleState());
            };

            addAndMakeVisible (frameRateLabel);
            frameRateLabel.setText ("MTC Frame Rate:", dontSendNotification);
            frameRateLabel.attachToComponent (&frameRateBox, true);

            addAndMakeVisible (frameRateBox);
            frameRateBox.addItem ("24 fps", 1);
            frameRateBox.addItem ("25 fps", 2);
            frameRateBox.addItem ("29.97 df", 3);
            frameRateBox.addItem ("30 fps", 4);

            const int currentRate = processor->getMtcFrameRate();
            int selectedId = 4;
            if (currentRate == 24) selectedId = 1;
            else if (currentRate == 25) selectedId = 2;
            else if (currentRate == 2997) selectedId = 3;
            frameRateBox.setSelectedId (selectedId, dontSendNotification);

            frameRateBox.onChange = [this]
            {
                if (processor == nullptr)
                    return;

                const int id = frameRateBox.getSelectedId();
                int rate = 30;
                if (id == 1) rate = 24;
                else if (id == 2) rate = 25;
                else if (id == 3) rate = 2997;
                processor->setMtcFrameRate (rate);
            };
        }
    }

    void paint (Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        const ScopedValueSetter<bool> scope (isResizing, true);

        auto r = getLocalBounds().reduced (10);
        const auto itemHeight = deviceSelector.getItemHeight();
        const auto separatorHeight = (itemHeight >> 1);
        auto makeControlBounds = [separatorHeight, itemHeight] (Rectangle<int> row)
        {
            const auto controlX = row.getX() + roundToInt (row.getWidth() * 0.35f);
            const auto controlW = roundToInt (row.getWidth() * 0.60f);
            const auto controlY = row.getY() + separatorHeight;
            return Rectangle<int> (controlX, controlY, controlW, itemHeight);
        };

        if (owner.getProcessorHasPotentialFeedbackLoop())
        {
            auto feedbackRow = r.removeFromTop (itemHeight);
            shouldMuteButton.setBounds (makeControlBounds (feedbackRow));
            r.removeFromTop (separatorHeight);
        }

        Rectangle<int> mtcArea;
        if (processor != nullptr)
            mtcArea = r.removeFromBottom ((itemHeight + separatorHeight) * 2);

        deviceSelector.setBounds (r);

        if (processor != nullptr)
        {
            auto mtcToggleRow = mtcArea.removeFromTop (itemHeight);
            mtcToggle.setBounds (makeControlBounds (mtcToggleRow));
            mtcArea.removeFromTop (separatorHeight);

            auto frameRateRow = mtcArea.removeFromTop (itemHeight);
            frameRateBox.setBounds (makeControlBounds (frameRateRow));
        }
    }

    void childBoundsChanged (Component* childComp) override
    {
        if (! isResizing && childComp == &deviceSelector)
            setToRecommendedSize();
    }

    void setToRecommendedSize()
    {
        int extraHeight = 0;

        if (owner.getProcessorHasPotentialFeedbackLoop())
        {
            const auto itemHeight = deviceSelector.getItemHeight();
            const auto separatorHeight = (itemHeight >> 1);
            extraHeight += itemHeight + separatorHeight;
        }

        if (processor != nullptr)
        {
            const auto itemHeight = deviceSelector.getItemHeight();
            const auto separatorHeight = (itemHeight >> 1);
            extraHeight += (itemHeight + separatorHeight) * 2;
        }

        setSize (getWidth(), deviceSelector.getHeight() + extraHeight + 20);
    }

private:
    NinjamStandalonePluginHolder& owner;
    AudioDeviceSelectorComponent deviceSelector;
    Label shouldMuteLabel;
    ToggleButton shouldMuteButton;
    Label mtcOutLabel;
    ToggleButton mtcToggle;
    Label frameRateLabel;
    ComboBox frameRateBox;
    NinjamVst3AudioProcessor* processor = nullptr;
    bool isResizing = false;
};

class NinjamStandaloneFilterWindow : public DocumentWindow
{
public:
    using PluginInOuts = StandalonePluginHolder::PluginInOuts;

    NinjamStandaloneFilterWindow (const String& title,
                                  Colour backgroundColour,
                                  PropertySet* settingsToUse,
                                  bool takeOwnershipOfSettings,
                                  const String& preferredDefaultDeviceName = String(),
                                  const AudioDeviceManager::AudioDeviceSetup* preferredSetupOptions = nullptr,
                                  const Array<PluginInOuts>& constrainToConfiguration = {},
                                  bool autoOpenMidiDevices = false)
        : DocumentWindow (title, backgroundColour, DocumentWindow::minimiseButton | DocumentWindow::closeButton)
    {
        pluginHolder = std::make_unique<NinjamStandalonePluginHolder> (settingsToUse,
                                                                       takeOwnershipOfSettings,
                                                                       preferredDefaultDeviceName,
                                                                       preferredSetupOptions,
                                                                       constrainToConfiguration,
                                                                       autoOpenMidiDevices);

       #if JUCE_IOS || JUCE_ANDROID
        setTitleBarHeight (0);
       #else
        setUsingNativeTitleBar (true);
       #endif

        updateContent();
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    AudioProcessor* getAudioProcessor() const
    {
        return pluginHolder != nullptr ? pluginHolder->processor.get() : nullptr;
    }

    NinjamStandalonePluginHolder* getPluginHolder() const
    {
        return pluginHolder.get();
    }

    void closeButtonPressed() override
    {
        if (pluginHolder != nullptr)
        {
            pluginHolder->stopPlaying();
            if (auto* processor = dynamic_cast<NinjamVst3AudioProcessor*> (getAudioProcessor()))
                processor->beginStandaloneShutdown();
            pluginHolder->savePluginState();
        }

        clearContentComponent();

        JUCEApplicationBase::quit();
    }

    void resized() override
    {
        DocumentWindow::resized();
    }

private:
    class MainContentComponent : public Component,
                                 private ComponentListener
    {
    public:
        explicit MainContentComponent (NinjamStandaloneFilterWindow& filterWindow)
            : owner (filterWindow)
        {
            if (auto* processor = owner.getAudioProcessor())
                editor.reset (processor->hasEditor() ? processor->createEditorIfNeeded()
                                                     : new GenericAudioProcessorEditor (*processor));

            if (editor != nullptr)
            {
                if (auto* ninjamEditor = dynamic_cast<NinjamVst3AudioProcessorEditor*> (editor.get()))
                {
                    ninjamEditor->setStandaloneOptionsMenuHandler (
                        [this] (Component* anchorComponent)
                        {
                            owner.showStandaloneOptionsMenu (anchorComponent);
                        });
                }

                editor->addComponentListener (this);
                addAndMakeVisible (editor.get());
                handleMovedOrResized();
            }
        }

        ~MainContentComponent() override
        {
            if (editor != nullptr)
            {
                if (auto* ninjamEditor = dynamic_cast<NinjamVst3AudioProcessorEditor*> (editor.get()))
                    ninjamEditor->setStandaloneOptionsMenuHandler ({});

                editor->removeComponentListener (this);
                if (owner.getPluginHolder() != nullptr && owner.getPluginHolder()->processor != nullptr)
                    owner.getPluginHolder()->processor->editorBeingDeleted (editor.get());
                editor = nullptr;
            }
        }

        void resized() override
        {
            if (editor != nullptr)
                editor->setTopLeftPosition (0, 0);
        }

    private:
        void componentMovedOrResized (Component&, bool, bool) override
        {
            handleMovedOrResized();
        }

        void handleMovedOrResized()
        {
            if (editor == nullptr)
                return;

            const auto rect = editor->getLocalArea (this, editor->getLocalBounds());
            setSize (rect.getWidth(), rect.getHeight());
        }

        NinjamStandaloneFilterWindow& owner;
        std::unique_ptr<AudioProcessorEditor> editor;
    };

public:
    void showAudioSettingsDialog()
    {
        if (pluginHolder == nullptr || pluginHolder->processor == nullptr)
            return;

        DialogWindow::LaunchOptions o;

        int maxNumInputs = jmax (0, pluginHolder->getNumInputChannels());
        int maxNumOutputs = jmax (0, pluginHolder->getNumOutputChannels());

        if (auto* bus = pluginHolder->processor->getBus (true, 0))
            maxNumInputs = jmax (0, bus->getDefaultLayout().size());

        if (auto* bus = pluginHolder->processor->getBus (false, 0))
            maxNumOutputs = jmax (0, bus->getDefaultLayout().size());

        auto content = std::make_unique<NinjamAudioSettingsComponent> (*pluginHolder,
                                                                       pluginHolder->deviceManager,
                                                                       maxNumInputs,
                                                                       maxNumOutputs);
        content->setSize (520, 620);
        content->setToRecommendedSize();

        o.content.setOwned (content.release());
        o.dialogTitle = TRANS ("Audio/MIDI Settings");
        o.dialogBackgroundColour = o.content->getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void resetToDefaultState()
    {
        if (pluginHolder == nullptr)
            return;

        pluginHolder->stopPlaying();
        clearContentComponent();
        pluginHolder->deletePlugin();

        if (auto* props = pluginHolder->settings.get())
            props->removeValue ("filterState");

        pluginHolder->createPlugin();
        updateContent();
        pluginHolder->startPlaying();
    }

    void updateContent()
    {
        setContentOwned (new MainContentComponent (*this), true);
    }

    void handleMenuResult (int result)
    {
        if (pluginHolder == nullptr)
            return;

        switch (result)
        {
            case 1:  showAudioSettingsDialog(); break;
            case 2:  pluginHolder->askUserToSaveState(); break;
            case 3:  pluginHolder->askUserToLoadState(); break;
            case 4:  resetToDefaultState(); break;
            default: break;
        }
    }

    static void menuCallback (int result, NinjamStandaloneFilterWindow* button)
    {
        if (button != nullptr && result != 0)
            button->handleMenuResult (result);
    }

    void showStandaloneOptionsMenu (Component* anchorComponent)
    {
        PopupMenu m;
        m.addItem (1, TRANS ("Audio/MIDI Settings..."));
        m.addSeparator();
        m.addItem (2, TRANS ("Save current state..."));
        m.addItem (3, TRANS ("Load a saved state..."));
        m.addSeparator();
        m.addItem (4, TRANS ("Reset to default state"));

        auto options = PopupMenu::Options();
        if (anchorComponent != nullptr)
            options = options.withTargetComponent (anchorComponent);

        m.showMenuAsync (options,
                         ModalCallbackFunction::forComponent (menuCallback, this));
    }

    std::unique_ptr<NinjamStandalonePluginHolder> pluginHolder;
};

class NinjamStandaloneApp final : public JUCEApplication, private Timer
{
public:
    const String getApplicationName() override              { return JucePlugin_Name; }
    const String getApplicationVersion() override           { return NINJAM_DISPLAY_VERSION; }
    bool moreThanOneInstanceAllowed() override              { return true; }
    void anotherInstanceStarted (const String&) override    {}

    void initialise (const String&) override
    {
        PropertiesFile::Options options;
        options.applicationName = JucePlugin_Name;
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_WINDOWS
        options.folderName = File::getSpecialLocation (File::userDocumentsDirectory)
                                .getChildFile ("NINJAMplus")
                                .getFullPathName();
       #elif JUCE_LINUX || JUCE_BSD
        options.folderName = "~/.config";
       #else
        options.folderName = JucePlugin_Name;
       #endif

        appProperties.setStorageParameters (options);

        // Allow per-instance settings (e.g. a second test client with its own username).
        const String overrideDir = SystemStats::getEnvironmentVariable ("NINJAMPLUS_SETTINGS_DIR", {});
        if (overrideDir.isNotEmpty())
        {
            options.folderName = overrideDir;
            appProperties.setStorageParameters (options);
        }

        // Migrate audio-device settings from old Roaming location to Documents/NINJAMplus.
        // Only runs once: if the new file already exists we skip.
       #if JUCE_WINDOWS
        {
            File newFile = options.getDefaultFile();
            if (! newFile.existsAsFile())
            {
                PropertiesFile::Options oldOpts;
                oldOpts.applicationName     = JucePlugin_Name;
                oldOpts.filenameSuffix      = ".settings";
                oldOpts.folderName          = "";
                oldOpts.osxLibrarySubFolder = "Application Support";
                File oldFile = oldOpts.getDefaultFile();
                if (oldFile.existsAsFile())
                {
                    PropertiesFile oldProps (oldOpts);
                    PropertiesFile newProps (options);
                    const auto keys = oldProps.getAllProperties().getAllKeys();
                    for (const auto& key : keys)
                        newProps.setValue (key, oldProps.getValue (key));
                    newProps.saveIfNeeded();
                }
            }
        }
       #endif

        mainWindow.reset (createWindow());

        // Explicitly enabled local-file diagnostics; no network control listener.
        const auto diagnosticPath = SystemStats::getEnvironmentVariable ("NINJAMPLUS_DIAGNOSTIC_DIR", {});
        if (File::isAbsolutePath (diagnosticPath))
        {
            diagnosticDirectory = File (diagnosticPath);
            if (diagnosticDirectory.createDirectory())
            {
                mainWindow->getPluginHolder()->shouldMuteInput = true;
                if (auto* processor = dynamic_cast<NinjamVst3AudioProcessor*> (mainWindow->getAudioProcessor()))
                {
                    processor->setTransmitLocal (false);
                    processor->setLocalMonitorEnabled (false);
                    processor->setMetronomeMuted (true);
                }
                startTimer (250);
            }
        }

        // First-run check: if firstrun has not been set, show the audio settings dialog
        // so the user can configure their audio device before doing anything else.
        if (auto* settings = appProperties.getUserSettings())
        {
            if (! settings->containsKey ("firstrun"))
            {
                settings->setValue ("firstrun", 0);
                settings->saveIfNeeded();
                mainWindow->showAudioSettingsDialog();
            }
        }
    }

    void shutdown() override
    {
        stopTimer();
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    NinjamStandaloneFilterWindow* createWindow()
    {
       #ifdef JucePlugin_PreferredChannelConfigurations
        StandalonePluginHolder::PluginInOuts channels[] = { JucePlugin_PreferredChannelConfigurations };
       #endif

        return new NinjamStandaloneFilterWindow (getApplicationName(),
                                                 LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                                 appProperties.getUserSettings(),
                                                 false,
                                                 {},
                                                 nullptr
                                                #ifdef JucePlugin_PreferredChannelConfigurations
                                                 , Array<StandalonePluginHolder::PluginInOuts> (channels, numElementsInArray (channels))
                                                #else
                                                 , {}
                                                #endif
                                                #if JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
                                                 , false
                                                #endif
                                                 );
    }

    ApplicationProperties appProperties;
    File diagnosticDirectory;
    String lastDiagnosticId, lastDiagnosticResult;

    void timerCallback() override
    {
        auto* processor = mainWindow != nullptr
            ? dynamic_cast<NinjamVst3AudioProcessor*> (mainWindow->getAudioProcessor()) : nullptr;
        if (processor == nullptr)
            return;
        const auto commandFile = diagnosticDirectory.getChildFile ("command.json");
        if (commandFile.existsAsFile())
        {
            const auto command = JSON::parse (commandFile.loadFileAsString());
            if (auto* object = command.getDynamicObject())
            {
                if (commandFile.deleteFile())
                {
                    lastDiagnosticId = object->getProperty ("id").toString();
                    const auto action = object->getProperty ("action").toString();
                    lastDiagnosticResult = "accepted";
                    if (action == "connect")
                    {
                        const auto server = object->getProperty ("server").toString().trim();
                        const auto user = object->getProperty ("user").toString().trim();
                        if (server.isEmpty() || user.isEmpty())
                            lastDiagnosticResult = "server and user required";
                        else
                        {
                            processor->setTransmitLocal (false);
                            processor->connectToServer (server, user, {});
                        }
                    }
                    else if (action == "disconnect")
                        processor->disconnectFromServer();
                    else if (action == "openVideo")
                        processor->launchVideoSessionAsync();
                    else if (action == "recordStart")
                        lastDiagnosticResult = processor->startSessionRecording (
                            diagnosticDirectory.getNonexistentChildFile ("capture", {}, false))
                            ? "accepted" : "recording could not start";
                    else if (action == "recordStop")
                        lastDiagnosticResult = processor->stopSessionRecording()
                            ? "accepted" : "recording could not stop";
                    else if (action == "hotspot" && object->getProperty ("enabled").isBool())
                        processor->setMobileHotspotModeEnabled ((bool) object->getProperty ("enabled"));
                    else
                        lastDiagnosticResult = "unsupported action";
                }
            }
        }
        DynamicObject::Ptr status = new DynamicObject();
        status->setProperty ("at", Time::currentTimeMillis());
        status->setProperty ("version", getApplicationVersion());
        status->setProperty ("executable", File::getSpecialLocation (File::currentExecutableFile).getFullPathName());
        status->setProperty ("commandId", lastDiagnosticId);
        status->setProperty ("commandResult", lastDiagnosticResult);
        status->setProperty ("connectionStatus", processor->getClient().GetStatus());
        status->setProperty ("connectionError", String::fromUTF8 (processor->getClient().GetErrorStr()));
        status->setProperty ("bpm", processor->getBPM());
        status->setProperty ("bpi", processor->getBPI());
        status->setProperty ("hotspot", processor->isMobileHotspotModeEnabled());
        status->setProperty ("inputMuted", (bool) mainWindow->getPluginHolder()->shouldMuteInput.getValue());
        status->setProperty ("transmittingLocal", processor->isTransmittingLocal());
        status->setProperty ("sync", processor->getSyncDiagnosticState());
        status->setProperty ("recording", processor->isSessionRecording());
        status->setProperty ("recordingStatus", processor->getSessionRecordingStatus());
        status->setProperty ("recordingPath", processor->getSessionRecordingFile().getFullPathName());
        Array<var> users;
        for (const auto& user : processor->getConnectedUsers())
        {
            DynamicObject::Ptr entry = new DynamicObject();
            entry->setProperty ("name", user.name);
            entry->setProperty ("index", user.index);
            entry->setProperty ("peakL", processor->getUserPeak (user.index, 0));
            entry->setProperty ("peakR", processor->getUserPeak (user.index, 1));
            users.add (var (entry.get()));
        }
        status->setProperty ("users", users);
        if (auto* device = mainWindow->getPluginHolder()->deviceManager.getCurrentAudioDevice())
        {
            status->setProperty ("audioDevice", device->getName());
            status->setProperty ("sampleRate", device->getCurrentSampleRate());
            status->setProperty ("blockSize", device->getCurrentBufferSizeSamples());
            status->setProperty ("outputLatencySamples", device->getOutputLatencyInSamples());
        }
        TemporaryFile snapshot (diagnosticDirectory.getChildFile ("status.json"));
        if (snapshot.getFile().replaceWithText (JSON::toString (var (status.get()))))
            snapshot.overwriteTargetFileWithTemporary();
    }

    std::unique_ptr<NinjamStandaloneFilterWindow> mainWindow;
};

} // namespace juce

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new juce::NinjamStandaloneApp();
}

#endif
