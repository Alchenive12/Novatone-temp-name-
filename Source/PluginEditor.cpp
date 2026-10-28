// ============================================================================
// Source/PluginEditor.cpp
// ============================================================================
#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// PRESET BAR (WITH 5-THEME ALCHENIVE STUDIO SELECTOR)
//==============================================================================
PresetBar::PresetBar(PresetManager& manager, juce::AudioProcessorValueTreeState& apvts,
                     std::function<void()> onThemeChangedCallback)
    : presets(manager), state(apvts), onThemeChanged(std::move(onThemeChangedCallback))
{
    auto& laf = HarmonyEngineLookAndFeel::shared();
    for (juce::Component* c : { static_cast<juce::Component*>(&themeBox),
                                static_cast<juce::Component*>(&presetBox),
                                static_cast<juce::Component*>(&saveButton),
                                static_cast<juce::Component*>(&saveAsButton) })
    {
        c->setLookAndFeel(&laf);
        addAndMakeVisible(c);
    }

    if (auto* themeChoice = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter("ui_theme")))
        themeBox.addItemList(themeChoice->choices, 1);
    else
        themeBox.addItemList({ "NovaTone Studio", "Harmony Engine", "The Drum Master", "The Conductor", "AmpPire Valve" }, 1);

    themeBox.setTooltip("Switch the UI skin across Alchenive plugins (NovaTone, Harmony Engine, The Drum Master, The Conductor, AmpPire)");
    themeBox.getProperties().set("caption", "Theme");

    for (auto* b : { &tuneTabButton, &harmonyTabButton, &choirTabButton })
    {
        b->setLookAndFeel(&laf);
        b->setRadioGroupId(0x7ab);
        b->setClickingTogglesState(true);
        b->getProperties().set("pillRole", "cyan");
        addAndMakeVisible(b);
    }
    tuneTabButton.getProperties().set("segment", "first");
    harmonyTabButton.getProperties().set("segment", "middle");
    choirTabButton.getProperties().set("segment", "last");
    choirTabButton.setTooltip("Mix AI singers over your harmony parts while the song plays");
    choirTabButton.onClick = [this] { if (onTabChanged) onTabChanged(2); };
    tuneTabButton.setToggleState(true, juce::dontSendNotification);
    tuneTabButton.setTooltip("Pitch & time editing");
    harmonyTabButton.setTooltip("Chord-aware harmonies from your vocal, following the beat on the sidechain");
    tuneTabButton.onClick    = [this] { if (onTabChanged) onTabChanged(0); };
    harmonyTabButton.onClick = [this] { if (onTabChanged) onTabChanged(1); };
    themeBox.getProperties().set("ghost", true);
    presetBox.getProperties().set("caption", "Preset");
    saveButton.getProperties().set("ghost", true);
    saveAsButton.getProperties().set("ghost", true);
    if (state.getParameter("ui_theme") != nullptr)
        themeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(state, "ui_theme", themeBox);
    else
        themeBox.setSelectedId(1, juce::dontSendNotification);

    themeBox.onChange = [this]
    {
        const int idx = juce::jmax(0, themeBox.getSelectedItemIndex());
        HarmonyEngineColours::applyTheme(idx);
        HarmonyEngineLookAndFeel::shared().syncThemeColours();
        if (onThemeChanged)
            onThemeChanged();
    };

    presetBox.setTextWhenNothingSelected(PresetManager::initPresetName);
    presetBox.setTooltip("Presets are stored in " + PresetManager::getPresetDirectory().getFullPathName());
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedItemIndex();
        if (index >= 0)
            presets.loadPreset(presetBox.getItemText(index));
    };

    saveButton.setTooltip("Save over the current preset");
    saveButton.onClick = [this] { save(); };
    saveAsButton.setTooltip("Save as a new NovaTone preset");
    saveAsButton.onClick = [this] { saveAs(); };

    state.state.addListener(this);
    applyCurrentThemeFromState(false); // no callback: the editor's other members aren't constructed yet
    refresh();
}

PresetBar::~PresetBar()
{
    state.state.removeListener(this);
    cancelPendingUpdate();
    themeBox.setLookAndFeel(nullptr);
    presetBox.setLookAndFeel(nullptr);
    saveButton.setLookAndFeel(nullptr);
    tuneTabButton.setLookAndFeel(nullptr);
    harmonyTabButton.setLookAndFeel(nullptr);
    choirTabButton.setLookAndFeel(nullptr);
    saveAsButton.setLookAndFeel(nullptr);
}

void PresetBar::applyCurrentThemeFromState(bool notify)
{
    if (auto* p = state.getRawParameterValue("ui_theme"))
    {
        const int idx = juce::jlimit(0, 4, juce::roundToInt(p->load(std::memory_order_relaxed)));
        HarmonyEngineColours::applyTheme(idx);
        HarmonyEngineLookAndFeel::shared().syncThemeColours();
        if (notify && onThemeChanged)
            onThemeChanged();
    }
}

void PresetBar::refresh()
{
    const auto current = presets.getCurrentPresetName();

    presetBox.clear(juce::dontSendNotification);
    presetBox.addItem(PresetManager::initPresetName, 1);
    const auto names = presets.getPresetNames();
    if (!names.isEmpty())
        presetBox.addSeparator();
    for (int i = 0; i < names.size(); ++i)
        presetBox.addItem(names[i], i + 2);

    if (current == PresetManager::initPresetName)
        presetBox.setSelectedId(1, juce::dontSendNotification);
    else if (names.contains(current))
        presetBox.setSelectedId(names.indexOf(current) + 2, juce::dontSendNotification);
    else
        presetBox.setText(current, juce::dontSendNotification);
}

void PresetBar::save()
{
    const auto current = presets.getCurrentPresetName();
    if (current == PresetManager::initPresetName || !presets.getPresetNames().contains(current))
    {
        saveAs();
        return;
    }
    presets.savePreset(current);
    refresh();
}

void PresetBar::saveAs()
{
    auto* window = new juce::AlertWindow("Save preset", "Name your vocal preset:", juce::MessageBoxIconType::NoIcon, this);
    window->setLookAndFeel(&HarmonyEngineLookAndFeel::shared());
    const auto current = presets.getCurrentPresetName();
    window->addTextEditor("name", current == PresetManager::initPresetName ? juce::String() : current);
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<PresetBar> safeThis(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, window](int result)
    {
        window->setLookAndFeel(nullptr);
        if (result != 1 || safeThis == nullptr)
            return;

        const auto name = window->getTextEditorContents("name");
        if (!safeThis->presets.savePreset(name))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset not saved",
                "Choose a different name (not empty and not \"" + juce::String(PresetManager::initPresetName) + "\").");
        safeThis->refresh();
    }), true);
}

void PresetBar::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto b = getLocalBounds().toFloat();

    g.setColour(backgroundDeep);
    g.fillRect(b);
    g.setColour(hairline());
    g.fillRect(b.withTop(b.getBottom() - 1.0f));

    // Logo mark: rounded tile with a stylised pitch curve snapping onto a note line.
    auto brand = brandArea.toFloat();
    const auto mark = brand.removeFromLeft(26.0f).withSizeKeepingCentre(26.0f, 26.0f);
    g.setGradientFill(juce::ColourGradient(cyanAccent, mark.getX(), mark.getY(), accent, mark.getRight(), mark.getBottom(), false));
    g.fillRoundedRectangle(mark, 7.0f);
    {
        juce::Path curve;
        const auto m = mark.reduced(5.5f, 7.0f);
        curve.startNewSubPath(m.getX(), m.getBottom());
        curve.cubicTo(m.getX() + m.getWidth() * 0.30f, m.getBottom(), m.getX() + m.getWidth() * 0.35f, m.getY(),
                      m.getX() + m.getWidth() * 0.62f, m.getY());
        curve.lineTo(m.getRight(), m.getY());
        g.setColour(juce::Colour(0xff0f1116).withAlpha(0.88f));
        g.strokePath(curve, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    brand.removeFromLeft(10.0f);
    g.setFont(HarmonyEngineUI::font(17.0f, true, -0.01f));
    g.setColour(textPrimary);
    const float nameW = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), "NovaTone") + 2.0f;
    g.drawText("NovaTone", brand.removeFromLeft(nameW), juce::Justification::centredLeft);


}

void PresetBar::setActiveTab(int tab)
{
    tuneTabButton.setToggleState(tab == 0, juce::dontSendNotification);
    harmonyTabButton.setToggleState(tab == 1, juce::dontSendNotification);
    choirTabButton.setToggleState(tab == 2, juce::dontSendNotification);
}

void PresetBar::resized()
{
    auto r = getLocalBounds().reduced(16, 10);
    themeBox.setBounds(r.removeFromRight(juce::jmin(220, r.getWidth() / 5)));

    // Brand (logo + name) then the Tune | Harmony | Choir switch
    auto tabs = r.withLeft(r.getX() + 26 + 10 + 92).withWidth(3 * 80);
    tuneTabButton.setBounds(tabs.removeFromLeft(80));
    harmonyTabButton.setBounds(tabs.removeFromLeft(80));
    choirTabButton.setBounds(tabs);

    // Preset group is centred in the bar.
    const int groupW = 300 + 8 + 56 + 4 + 72;
    auto group = getLocalBounds().withSizeKeepingCentre(groupW, r.getHeight());
    if (group.getX() < choirTabButton.getRight() + 16) group.setX(choirTabButton.getRight() + 16);   // never under the tabs
    presetBox.setBounds(group.removeFromLeft(300));
    group.removeFromLeft(8);
    saveButton.setBounds(group.removeFromLeft(56));
    group.removeFromLeft(4);
    saveAsButton.setBounds(group.removeFromLeft(72));

    brandArea = r.withRight(tuneTabButton.getX() - 8);
}

//==============================================================================
// MASTER BAR (INPUT / OUTPUT)
//==============================================================================
namespace
{
    constexpr float meterFloorDb = -60.0f, meterCeilDb = 6.0f;

    void setUpGainKnob(juce::Slider& knob, const juce::String& tooltip)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        knob.setLookAndFeel(&HarmonyEngineLookAndFeel::shared());
        knob.setMouseDragSensitivity(200);
        knob.setDoubleClickReturnValue(true, 0.0);
        knob.setTooltip(tooltip + " (double-click: 0 dB)");
    }
}

MasterBar::MasterBar(juce::AudioProcessorValueTreeState& apvts,
    NovaToneAudioProcessor::PeakMeter& inputMeterSource, NovaToneAudioProcessor::PeakMeter& outputMeterSource)
    : inputSource(inputMeterSource), outputSource(outputMeterSource),
    inputAttachment(apvts, "global_input", inputKnob),
    outputAttachment(apvts, "global_output", outputKnob)
{
    setUpGainKnob(inputKnob, "Input gain into NovaTone");
    setUpGainKnob(outputKnob, "Output gain after NovaTone");
    addAndMakeVisible(inputKnob);
    addAndMakeVisible(outputKnob);
    startTimerHz(30);
}

MasterBar::~MasterBar()
{
    stopTimer();
    inputKnob.setLookAndFeel(nullptr);
    outputKnob.setLookAndFeel(nullptr);
}

void MasterBar::updateMeter(Meter& meter, NovaToneAudioProcessor::PeakMeter& source, float dt, double nowMs)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        const float peakDb = juce::Decibels::gainToDecibels(source[(size_t)ch].exchange(0.0f, std::memory_order_relaxed), -100.0f);
        meter.levelDb[ch] = juce::jmax(peakDb, meter.levelDb[ch] - 26.0f * dt);
        if (peakDb >= meter.holdDb[ch] || nowMs > meter.holdUntilMs[ch])
        {
            meter.holdDb[ch] = juce::jmax(peakDb, meter.levelDb[ch]);
            meter.holdUntilMs[ch] = nowMs + 1500.0;
        }
    }
}

void MasterBar::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float dt = lastTickMs > 0.0 ? juce::jlimit(0.001f, 0.2f, (float)((nowMs - lastTickMs) * 0.001)) : 1.0f / 30.0f;
    lastTickMs = nowMs;

    updateMeter(inputMeter, inputSource, dt, nowMs);
    updateMeter(outputMeter, outputSource, dt, nowMs);
    repaint(inputMeter.area.expanded(4));
    repaint(outputMeter.area.expanded(4));
}

void MasterBar::paintMeter(juce::Graphics& g, const Meter& meter)
{
    using namespace HarmonyEngineColours;
    auto area = meter.area.toFloat();
    if (area.isEmpty()) return;

    auto xFor = [&](float db) { return area.getX() + area.getWidth() * juce::jlimit(0.0f, 1.0f, (db - meterFloorDb) / (meterCeilDb - meterFloorDb)); };

    const float barHeight = 4.0f, gap = 3.0f;
    auto bars = area.withSizeKeepingCentre(area.getWidth(), barHeight * 2.0f + gap);
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto track = juce::Rectangle<float>(bars.getX(), bars.getY() + (float)ch * (barHeight + gap), bars.getWidth(), barHeight);
        g.setColour(panelLight.brighter(isLightChassis ? 0.0f : 0.06f));
        g.fillRoundedRectangle(track, 2.0f);

        const float level = meter.levelDb[ch];
        if (level > meterFloorDb)
        {
            const auto lit = track.withRight(xFor(level));
            juce::ColourGradient fill(mintAccent, track.getX(), 0.0f, juce::Colour(0xffef4444), xFor(meterCeilDb), 0.0f, false);
            fill.addColour((xFor(-12.0f) - track.getX()) / track.getWidth(), mintAccent);
            fill.addColour((xFor(-3.0f) - track.getX()) / track.getWidth(), accent);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(lit, 2.0f);
        }

        if (meter.holdDb[ch] > meterFloorDb)
        {
            g.setColour(meter.holdDb[ch] > 0.0f ? juce::Colour(0xffef4444) : textPrimary.withAlpha(0.8f));
            g.fillRect(juce::Rectangle<float>(1.5f, barHeight).withCentre({ xFor(meter.holdDb[ch]), track.getCentreY() }));
        }
    }

    // 0 dB reference tick
    g.setColour(textSecondary.withAlpha(0.6f));
    g.fillRect(juce::Rectangle<float>(1.0f, bars.getHeight() + 6.0f).withCentre({ xFor(0.0f), bars.getCentreY() }));
}

void MasterBar::paintKnobCaption(juce::Graphics& g, const juce::Slider& knob, const juce::String& caption, bool captionLeft)
{
    using namespace HarmonyEngineColours;
    const auto k = knob.getBounds();
    auto area = captionLeft ? juce::Rectangle<int>(k.getX() - 64, k.getY(), 58, k.getHeight())
                            : juce::Rectangle<int>(k.getRight() + 6, k.getY(), 58, k.getHeight());
    const auto just = captionLeft ? juce::Justification::centredRight : juce::Justification::centredLeft;
    area = area.withSizeKeepingCentre(area.getWidth(), 30);

    g.setFont(HarmonyEngineUI::font(11.0f, false));
    g.setColour(textSecondary);
    g.drawText(caption, area.removeFromTop(14), just);
    const double v = knob.getValue();
    g.setFont(HarmonyEngineUI::font(12.0f, true));
    g.setColour(textPrimary);
    g.drawText((v > 0.05 ? "+" : "") + juce::String(v, 1) + " dB", area, just);
}

void MasterBar::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto b = getLocalBounds().toFloat();

    g.setColour(backgroundDeep);
    g.fillRect(b);
    g.setColour(hairline());
    g.fillRect(b.withHeight(1.0f));

    paintKnobCaption(g, inputKnob, "Input", false);
    paintKnobCaption(g, outputKnob, "Output", true);
    paintMeter(g, inputMeter);
    paintMeter(g, outputMeter);
}

void MasterBar::setCentreComponent(juce::Component* c)
{
    centreComponent = c;
    if (c != nullptr)
        addAndMakeVisible(c);
    resized();
}

void MasterBar::resized()
{
    auto r = getLocalBounds().reduced(16, 0);
    inputKnob.setBounds(r.removeFromLeft(34).withSizeKeepingCentre(34, 34));
    r.removeFromLeft(70);
    const int meterW = juce::jmin(180, r.getWidth() / 4);
    inputMeter.area = r.removeFromLeft(meterW).withSizeKeepingCentre(meterW, 16);

    outputKnob.setBounds(r.removeFromRight(34).withSizeKeepingCentre(34, 34));
    r.removeFromRight(70);
    outputMeter.area = r.removeFromRight(meterW).withSizeKeepingCentre(meterW, 16);

    if (centreComponent != nullptr)
        centreComponent->setBounds(r.withSizeKeepingCentre(juce::jmin(240, r.getWidth() - 24), r.getHeight()));
}

//==============================================================================
// STANDALONE NOVATONE EDITOR
//==============================================================================
namespace
{
    // Fonts are resolved through the *default* LookAndFeel, so the shared one is installed
    // as default while any NovaTone editor is open (ref-counted for multiple instances).
    int& defaultLookAndFeelUsers() { static int n = 0; return n; }
}

NovaToneAudioProcessorEditor::NovaToneAudioProcessorEditor(NovaToneAudioProcessor& p)
    : AudioProcessorEditor(&p),
    audioProcessor(p),
    presetBar(p.getPresetManager(), p.apvts, [this]
    {
        workstationView.refreshTheme();
        harmonyView.refreshTheme();
        choirView.refreshTheme();
        repaint();
    }),
    workstationView(p.apvts, p.getPitchTraceQueue()),
    harmonyView(p.apvts, p.getHarmonyEngine(), p.getHarmonyExporter(), [&p] { return p.isSidechainConnected(); }, p.getChoir(), p.getChoirRecorder()),
    choirView(p.apvts, p.getHarmonyEngine(), p.getChoirRecorder(), p.getChoir()),
    masterBar(p.apvts, p.getInputPeakMeter(), p.getOutputPeakMeter())
{
    if (defaultLookAndFeelUsers()++ == 0)
    {
        juce::LookAndFeel::setDefaultLookAndFeel(&HarmonyEngineLookAndFeel::shared());
        juce::Typeface::clearTypefaceCache();
    }
    ownsDefaultLookAndFeel = true;
    setLookAndFeel(&HarmonyEngineLookAndFeel::shared());

    addAndMakeVisible(presetBar);
    addAndMakeVisible(workstationView);
    addChildComponent(harmonyView);
    addChildComponent(choirView);
    addAndMakeVisible(masterBar);
    presetBar.onTabChanged = [this](int tab)
    {
        activeTab = tab;
        workstationView.setVisible(tab == 0);
        harmonyView.setVisible(tab == 1);
        choirView.setVisible(tab == 2);
    };
    choirView.onSendToTune = [this, &p](const juce::File& f, juce::int64 start, double rate)
    {
        if (auto* poly = p.apvts.getParameter("tune_polyphonic")) poly->setValueNotifyingHost(1.0f);
        const double hostRate = p.getSampleRate() > 0 ? p.getSampleRate() : rate;
        p.getPolyClip().loadFile(f, (juce::int64) std::llround((double) start * hostRate / rate));
        presetBar.setActiveTab(0);
        if (presetBar.onTabChanged) presetBar.onTabChanged(0);
    };
    masterBar.setCentreComponent(&workstationView.getTunerComponent());
    workstationView.setClipSession(&p.getPolyClip());

    setResizable(true, true);
    setResizeLimits(920, 700, 1920, 1200);
    setSize(1100, 800);
}

NovaToneAudioProcessorEditor::~NovaToneAudioProcessorEditor()
{
    masterBar.setCentreComponent(nullptr);
    setLookAndFeel(nullptr);
    if (ownsDefaultLookAndFeel && --defaultLookAndFeelUsers() == 0)
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void NovaToneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(HarmonyEngineColours::background);
}

void NovaToneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    presetBar.setBounds(bounds.removeFromTop(52));
    masterBar.setBounds(bounds.removeFromBottom(48));
    workstationView.setBounds(bounds);
    harmonyView.setBounds(bounds);
    choirView.setBounds(bounds);
}
