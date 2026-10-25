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

    themeBox.setTooltip("Switch UI Skin across Alchenive plugins (NovaTone, Harmony Engine, The Drum Master, The Conductor, AmpPire)");
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
    presetBox.setTooltip("NovaTone Presets stored in " + PresetManager::getPresetDirectory().getFullPathName());
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
    auto* window = new juce::AlertWindow("SAVE NOVATONE PRESET", "Name your vocal preset:", juce::MessageBoxIconType::NoIcon, this);
    window->setLookAndFeel(&HarmonyEngineLookAndFeel::shared());
    const auto current = presets.getCurrentPresetName();
    window->addTextEditor("name", current == PresetManager::initPresetName ? juce::String() : current);
    window->addButton("SAVE", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("CANCEL", 0, juce::KeyPress(juce::KeyPress::escapeKey));

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

    juce::ColourGradient strip(panelLight, 0.0f, b.getY(), panel, 0.0f, b.getBottom(), false);
    g.setGradientFill(strip);
    g.fillRect(b);

    g.setColour(cyanAccent.withAlpha(0.55f));
    g.fillRect(b.withTop(b.getBottom() - 1.5f));

    // NOVATONE brand badge
    auto brand = brandArea.toFloat();
    const auto brandFont = HarmonyEngineUI::font(18.0f, true, 0.28f);
    const float brandWidth = juce::GlyphArrangement::getStringWidth(brandFont, "NOVATONE") + 10.0f;
    auto title = brand.removeFromLeft(brandWidth);

    g.setFont(brandFont);
    if (isLightChassis)
    {
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawText("NOVATONE", title.translated(0.0f, 1.0f), juce::Justification::centredLeft);
        g.setGradientFill(juce::ColourGradient(accentBright, 0.0f, title.getY(), accentDeep, 0.0f, title.getBottom(), false));
    }
    else
    {
        g.setGradientFill(juce::ColourGradient(cyanBright, 0.0f, title.getY(), accentBright, title.getRight(), title.getBottom(), false));
    }
    g.drawText("NOVATONE", title, juce::Justification::centredLeft);

    brand.removeFromLeft(12.0f);
    g.setColour(knobOutline);
    g.fillRect(juce::Rectangle<float>(1.0f, 16.0f).withCentre({ brand.getX(), brand.getCentreY() }));
    brand.removeFromLeft(12.0f);

    g.setFont(HarmonyEngineUI::font(10.0f, true, 0.22f));
    g.setColour(textSecondary);
    g.drawText("STUDIO PITCH, TIME & FORMANT EDITOR", brand, juce::Justification::centredLeft, true);

    g.setFont(HarmonyEngineUI::font(9.5f, true, 0.22f));
    g.setColour(textSecondary);
    g.drawText("THEME",  themeBox.getBounds().translated(-56, 0).withWidth(50),  juce::Justification::centredRight);
    g.drawText("PRESET", presetBox.getBounds().translated(-58, 0).withWidth(52), juce::Justification::centredRight);
}

void PresetBar::resized()
{
    auto r = getLocalBounds().reduced(18, 9);
    saveAsButton.setBounds(r.removeFromRight(80));
    r.removeFromRight(6);
    saveButton.setBounds(r.removeFromRight(62));
    r.removeFromRight(10);
    presetBox.setBounds(r.removeFromRight(176));
    r.removeFromRight(60); // "PRESET" label
    themeBox.setBounds(r.removeFromRight(158));
    r.removeFromRight(58); // "THEME" label
    brandArea = r;
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

    auto bars = area.withTrimmedBottom(11.0f);
    const float barHeight = (bars.getHeight() - 3.0f) * 0.5f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto track = juce::Rectangle<float>(bars.getX(), bars.getY() + (float)ch * (barHeight + 3.0f), bars.getWidth(), barHeight);
        g.setColour(graphBackground);
        g.fillRoundedRectangle(track, 2.5f);

        const float level = meter.levelDb[ch];
        if (level > meterFloorDb)
        {
            const auto lit = track.reduced(1.0f).withRight(xFor(level));
            juce::ColourGradient fill(cyanAccent, track.getX(), 0.0f, juce::Colour(0xffef4444), xFor(meterCeilDb), 0.0f, false);
            fill.addColour((xFor(-12.0f) - track.getX()) / track.getWidth(), mintAccent);
            fill.addColour((xFor(-3.0f) - track.getX()) / track.getWidth(), accent);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(lit, 2.0f);
        }

        if (meter.holdDb[ch] > meterFloorDb)
        {
            g.setColour(meter.holdDb[ch] > 0.0f ? juce::Colour(0xffef4444) : juce::Colours::white);
            g.fillRect(juce::Rectangle<float>(1.5f, barHeight - 2.0f).withCentre({ xFor(meter.holdDb[ch]), track.getCentreY() }));
        }
    }

    g.setColour(knobOutline);
    g.drawVerticalLine(juce::roundToInt(xFor(0.0f)), bars.getY(), bars.getBottom());
    g.setFont(HarmonyEngineUI::font(8.0f, true, 0.04f));
    g.setColour(textSecondary);
    for (int db : { -48, -24, -12, -6, 0 })
        g.drawText(juce::String(db), juce::Rectangle<float>(24.0f, 10.0f).withCentre({ xFor((float)db), area.getBottom() - 5.0f }), juce::Justification::centred);
}

void MasterBar::paintKnobCaption(juce::Graphics& g, const juce::Slider& knob, const juce::String& caption, bool captionLeft)
{
    using namespace HarmonyEngineColours;
    const auto k = knob.getBounds();
    auto area = captionLeft ? juce::Rectangle<int>(k.getX() - 68, k.getY(), 62, k.getHeight())
                            : juce::Rectangle<int>(k.getRight() + 6, k.getY(), 62, k.getHeight());
    const auto just = captionLeft ? juce::Justification::centredRight : juce::Justification::centredLeft;

    g.setFont(HarmonyEngineUI::font(9.5f, true, 0.22f));
    g.setColour(textSecondary);
    g.drawText(caption, area.removeFromTop(area.getHeight() / 2), just);
    const double v = knob.getValue();
    g.setFont(HarmonyEngineUI::font(11.0f, true, 0.04f));
    g.setColour(textPrimary);
    g.drawText((v > 0.0 ? "+" : "") + juce::String(v, 1) + " dB", area, just);
}

void MasterBar::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto b = getLocalBounds().toFloat();

    juce::ColourGradient strip(panel, 0.0f, b.getY(), backgroundDeep, 0.0f, b.getBottom(), false);
    g.setGradientFill(strip);
    g.fillRect(b);

    g.setColour(knobOutline);
    g.fillRect(b.withHeight(1.0f));

    paintKnobCaption(g, inputKnob, "INPUT", false);
    paintKnobCaption(g, outputKnob, "OUTPUT", true);
    paintMeter(g, inputMeter);
    paintMeter(g, outputMeter);
}

void MasterBar::resized()
{
    auto r = getLocalBounds().reduced(18, 2);
    inputKnob.setBounds(r.removeFromLeft(48));
    r.removeFromLeft(72);
    inputMeter.area = r.removeFromLeft(juce::jmin(320, r.getWidth() / 2 - 40)).reduced(0, 8);

    outputKnob.setBounds(r.removeFromRight(48));
    r.removeFromRight(72);
    outputMeter.area = r.removeFromRight(inputMeter.area.getWidth()).withHeight(inputMeter.area.getHeight()).withY(inputMeter.area.getY());
}

//==============================================================================
// STANDALONE NOVATONE EDITOR
//==============================================================================
NovaToneAudioProcessorEditor::NovaToneAudioProcessorEditor(NovaToneAudioProcessor& p)
    : AudioProcessorEditor(&p),
    audioProcessor(p),
    presetBar(p.getPresetManager(), p.apvts, [this]
    {
        workstationView.refreshTheme();
        repaint();
    }),
    workstationView(p.apvts, p.getPitchTraceQueue()),
    masterBar(p.apvts, p.getInputPeakMeter(), p.getOutputPeakMeter())
{
    addAndMakeVisible(presetBar);
    addAndMakeVisible(workstationView);
    addAndMakeVisible(masterBar);

    setResizable(true, true);
    setResizeLimits(920, 720, 1680, 1200);
    setSize(1040, 820);
}

NovaToneAudioProcessorEditor::~NovaToneAudioProcessorEditor() = default;

void NovaToneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(HarmonyEngineColours::background);
}

void NovaToneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    presetBar.setBounds(bounds.removeFromTop(48));
    masterBar.setBounds(bounds.removeFromBottom(52));
    workstationView.setBounds(bounds);
}