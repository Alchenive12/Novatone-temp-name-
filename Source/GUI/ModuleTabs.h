#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <functional>

//==============================================================================
/**
    NovaTone Multi-Theme Alchenive Studio Drawing Kit & LookAndFeel.
    Provides zero-allocation vector cards, 5 themeable rotary knob styles with
    semantic ring roles ("accent", "mint", "cyan"), and crisp pill buttons.
*/
namespace HarmonyEngineUI
{
    juce::Font font(float height, bool bold = true, float kerning = 0.06f);

    void paintChassis(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawLiftedPanel(juce::Graphics& g, juce::Rectangle<int> area, float cornerRadius);
    void drawInsetGlass(juce::Graphics& g, juce::Rectangle<int> area, float cornerRadius);
    void drawGlassGloss(juce::Graphics& g, juce::Rectangle<int> area, float cornerRadius);
    void drawInsetWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius);
    void paintBypassVeil(juce::Graphics& g, juce::Rectangle<int> body);
    void drawCardTitle(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);
    void drawIcon(juce::Graphics& g, const juce::String& icon, juce::Rectangle<float> area, juce::Colour colour);
    juce::Path segmentPath(juce::Rectangle<float> bounds, const juce::String& segment, float cornerRadius);
    juce::Font caption(float height);
    juce::Typeface::Ptr regularTypeface();
    juce::Typeface::Ptr semiBoldTypeface();

    constexpr int headerHeight = 0;
}

//==============================================================================
class HarmonyEngineLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static HarmonyEngineLookAndFeel& shared();

    void syncThemeColours();

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
        float sliderPosProportional, float rotaryStartAngle,
        float rotaryEndAngle, juce::Slider& slider) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
        bool shouldDrawButtonAsHighlighted,
        bool shouldDrawButtonAsDown) override;

    juce::Font getLabelFont(juce::Label& label) override;
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
        bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
        float sliderPos, float minSliderPos, float maxSliderPos,
        juce::Slider::SliderStyle style, juce::Slider& slider) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
        int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override;
    juce::Font getComboBoxFont(juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
        bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
        const juce::String& text, const juce::String& shortcutKeyText,
        const juce::Drawable* icon, const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
        bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    static void makePowerButton(juce::ToggleButton& button);

    /** LinearBar slider drawn as a value field: caption left, value right, fill from the origin. */
    void drawValueField(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, juce::Slider& slider);

private:
    HarmonyEngineLookAndFeel();

    void drawPowerButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down);
    void drawPillButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down);
};

//==============================================================================
/**
    User-friendly Rotary Knob with a bold title, subtitle/role hint, and
    customizable semantic accent ring role ("accent", "mint", "cyan").
*/
class RotaryKnob : public juce::Component
{
public:
    RotaryKnob(juce::AudioProcessorValueTreeState& apvts,
        const juce::String& parameterID,
        const juce::String& labelText,
        const juce::String& subtitleText = {},
        juce::Colour customRingColour = juce::Colour(),
        const juce::String& semanticRingRole = {});
    ~RotaryKnob() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void refreshThemeColours();
    void setSuggestedValue(double value);
    void setTooltipText(const juce::String& tip) { slider.setTooltip(tip); }

private:
    juce::Slider slider;
    juce::Label label;
    juce::String subtitle;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RotaryKnob)
};

//==============================================================================
class ChoiceSelector : public juce::Component
{
public:
    ChoiceSelector(juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterID,
        const juce::String& caption, const juce::String& tooltip);
    ~ChoiceSelector() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String caption;
    juce::ComboBox box;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChoiceSelector)
};

//==============================================================================
class ModuleHeader : public juce::Component
{
public:
    ModuleHeader(juce::AudioProcessorValueTreeState& apvts, const juce::String& moduleId,
        const juce::String& title, const juce::String& subtitle);
    ~ModuleHeader() override;

    void setTrailingComponent(juce::Component* component, int width = 116, int height = 26);
    bool isBypassed() const { return powerButton.getToggleState(); }

    std::function<void()> onBypassChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String title, subtitle;
    juce::ToggleButton powerButton;
    juce::AudioProcessorValueTreeState::ButtonAttachment powerAttachment;
    juce::Component* trailingComponent = nullptr;
    int trailingWidth = 116, trailingHeight = 26;
    bool lastBypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleHeader)
};

//==============================================================================
class ModuleTab : public juce::Component
{
public:
    ModuleTab(juce::AudioProcessorValueTreeState& apvts, const juce::String& moduleId,
        const juce::String& title, const juce::String& subtitle);

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

protected:
    juce::Rectangle<int> getBodyBounds() const;
    virtual void paintBody(juce::Graphics&) {}
    virtual void layoutBody(juce::Rectangle<int> body) = 0;

    ModuleHeader header;
    juce::Rectangle<int> bypassVeilArea; ///< Where the "bypassed" veil is drawn (defaults to the body).
};