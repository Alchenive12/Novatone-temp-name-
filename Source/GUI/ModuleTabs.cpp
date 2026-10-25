#include "ModuleTabs.h"
#include "NovaToneFonts.h"
#include "HarmonyEngineColours.h"
#include <cmath>
#include <limits>

//==============================================================================
// MULTI-THEME ALCHENIVE STUDIO DRAWING KIT
// (Zero-allocation vector shadows eliminate heap Image blurs during live resize/drag)
//==============================================================================
namespace HarmonyEngineUI
{
    namespace
    {
        juce::Typeface::Ptr loadTypeface(const char* data, int size)
        {
            return juce::Typeface::createSystemTypefaceFor(data, (size_t)size);
        }
    }

    juce::Typeface::Ptr regularTypeface()
    {
        static juce::Typeface::Ptr tf = loadTypeface(NovaToneFonts::InterRegular_ttf, NovaToneFonts::InterRegular_ttfSize);
        return tf;
    }

    juce::Typeface::Ptr semiBoldTypeface()
    {
        static juce::Typeface::Ptr tf = loadTypeface(NovaToneFonts::InterSemiBold_ttf, NovaToneFonts::InterSemiBold_ttfSize);
        return tf;
    }

    juce::Font font(float height, bool bold, float kerning)
    {
        // Inter is already tuned for small UI sizes; the old wide all-caps tracking is
        // capped so labels read as modern UI text rather than engraved panel lettering.
        auto tf = bold ? semiBoldTypeface() : regularTypeface();
        auto options = tf != nullptr ? juce::FontOptions(tf) : juce::FontOptions();
        return juce::Font(options.withHeight(height * 1.06f)
                                 .withKerningFactor(juce::jlimit(-0.02f, 0.06f, kerning)));
    }

    juce::Font caption(float height)
    {
        return font(height, true, 0.06f);
    }

    void paintChassis(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        g.setColour(HarmonyEngineColours::background);
        g.fillRect(bounds);
    }

    void drawLiftedPanel(juce::Graphics& g, juce::Rectangle<int> areaInt, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (areaInt.isEmpty()) return;

        const auto area = areaInt.toFloat();
        const float r = juce::jmin(cornerRadius, 10.0f);
        if (isLightChassis)
        {
            g.setColour(juce::Colours::black.withAlpha(0.05f));
            g.fillRoundedRectangle(area.translated(0.0f, 1.0f), r);
        }
        g.setColour(panel);
        g.fillRoundedRectangle(area, r);
        g.setColour(hairline());
        g.drawRoundedRectangle(area.reduced(0.5f), r, 1.0f);
    }

    void drawInsetGlass(juce::Graphics& g, juce::Rectangle<int> areaInt, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (areaInt.isEmpty()) return;

        const auto area = areaInt.toFloat();
        const float r = juce::jmin(cornerRadius, 10.0f);
        g.setColour(graphBackground);
        g.fillRoundedRectangle(area, r);
        g.setColour(hairline());
        g.drawRoundedRectangle(area.reduced(0.5f), r, 1.0f);
    }

    void drawGlassGloss(juce::Graphics&, juce::Rectangle<int>, float) {}

    void drawInsetWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (area.isEmpty()) return;

        g.setColour(graphBackground);
        g.fillRoundedRectangle(area, cornerRadius);
        g.setColour(hairline());
        g.drawRoundedRectangle(area.reduced(0.5f), cornerRadius, 1.0f);
    }

    void drawCardTitle(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
    {
        g.setFont(caption(10.5f));
        g.setColour(HarmonyEngineColours::textSecondary);
        g.drawText(title.toUpperCase(), area, juce::Justification::centredLeft, true);
    }

    void paintBypassVeil(juce::Graphics& g, juce::Rectangle<int> body)
    {
        using namespace HarmonyEngineColours;
        const auto veil = body.toFloat();
        g.setColour(backgroundDeep.withAlpha(0.72f));
        g.fillRoundedRectangle(veil, 10.0f);

        const auto f = font(12.5f, true);
        const juce::String text("Pitch correction bypassed");
        const float w = juce::GlyphArrangement::getStringWidth(f, text) + 36.0f;
        const auto badge = juce::Rectangle<float>(w, 30.0f).withCentre(veil.getCentre());
        g.setColour(panelLight);
        g.fillRoundedRectangle(badge, 15.0f);
        g.setColour(hairline());
        g.drawRoundedRectangle(badge.reduced(0.5f), 15.0f, 1.0f);

        g.setFont(f);
        g.setColour(textPrimary);
        g.drawText(text, badge, juce::Justification::centred);
    }

    juce::Path segmentPath(juce::Rectangle<float> b, const juce::String& segment, float r)
    {
        const bool first = segment == "first", last = segment == "last", middle = segment == "middle";
        juce::Path p;
        if (middle)
            p.addRectangle(b);
        else
            p.addRoundedRectangle(b.getX(), b.getY(), b.getWidth(), b.getHeight(), r, r,
                                  !last, !first, !last, !first);
        return p;
    }

    void drawIcon(juce::Graphics& g, const juce::String& icon, juce::Rectangle<float> area, juce::Colour colour)
    {
        const auto c = area.getCentre();
        const float s = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        juce::Path p;
        const juce::PathStrokeType stroke(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(colour);

        if (icon == "undo" || icon == "redo")
        {
            const float dir = icon == "undo" ? 1.0f : -1.0f;
            const float rad = s * 0.62f;
            p.addCentredArc(c.x, c.y + s * 0.12f, rad, rad, 0.0f,
                            dir > 0 ? -juce::MathConstants<float>::halfPi * 1.15f : juce::MathConstants<float>::halfPi * 1.15f,
                            dir > 0 ? juce::MathConstants<float>::halfPi * 0.95f : -juce::MathConstants<float>::halfPi * 0.95f, true);
            g.strokePath(p, stroke);
            const auto tip = juce::Point<float>(c.x - dir * rad * 0.93f, c.y + s * 0.12f - rad * 0.38f);
            juce::Path head;
            head.startNewSubPath(tip.translated(dir * s * 0.02f, -s * 0.42f));
            head.lineTo(tip);
            head.lineTo(tip.translated(dir * s * 0.44f, s * 0.06f));
            g.strokePath(head, stroke);
        }
        else if (icon == "dock")
        {
            const auto r = juce::Rectangle<float>(s * 1.5f, s * 1.3f).withCentre(c);
            g.drawRoundedRectangle(r, 2.0f, 1.4f);
            g.fillRect(r.withTop(r.getBottom() - r.getHeight() * 0.36f).reduced(1.5f, 1.5f));
        }
        else if (icon == "menu")
        {
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse(juce::Rectangle<float>(3.2f, 3.2f).withCentre({ c.x + (float)i * s * 0.55f, c.y }));
        }
        else if (icon == "chevronLeft" || icon == "chevronRight")
        {
            const float d = icon == "chevronLeft" ? 1.0f : -1.0f;
            p.startNewSubPath(c.x + d * s * 0.22f, c.y - s * 0.42f);
            p.lineTo(c.x - d * s * 0.22f, c.y);
            p.lineTo(c.x + d * s * 0.22f, c.y + s * 0.42f);
            g.strokePath(p, stroke);
        }
    }
}

using namespace HarmonyEngineUI;

//==============================================================================
// LOOK AND FEEL
//==============================================================================
HarmonyEngineLookAndFeel& HarmonyEngineLookAndFeel::shared()
{
    static HarmonyEngineLookAndFeel instance;
    return instance;
}

HarmonyEngineLookAndFeel::HarmonyEngineLookAndFeel()
{
    syncThemeColours();
}

void HarmonyEngineLookAndFeel::syncThemeColours()
{
    using namespace HarmonyEngineColours;
    setColour(juce::Slider::textBoxTextColourId, textPrimary);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, cyanAccent.withAlpha(0.35f));
    setColour(juce::TextEditor::textColourId, textPrimary);
    setColour(juce::TextEditor::highlightColourId, cyanAccent.withAlpha(0.35f));
    setColour(juce::CaretComponent::caretColourId, cyanBright);
    setColour(juce::TextButton::textColourOffId, textPrimary);
    setColour(juce::TextButton::textColourOnId, textPrimary);
    setColour(juce::ComboBox::textColourId, textPrimary);
    setColour(juce::AlertWindow::backgroundColourId, panelLight);
    setColour(juce::AlertWindow::textColourId, textPrimary);
    setColour(juce::AlertWindow::outlineColourId, cyanAccent);
    setColour(juce::TextEditor::backgroundColourId, isLightChassis ? background : graphBackground);
    setColour(juce::TextEditor::outlineColourId, knobOutline);
    setColour(juce::TextEditor::focusedOutlineColourId, cyanAccent);
}

void HarmonyEngineLookAndFeel::makePowerButton(juce::ToggleButton& button)
{
    button.getProperties().set("powerButton", true);
    button.setLookAndFeel(&shared());
}

juce::Typeface::Ptr HarmonyEngineLookAndFeel::getTypefaceForFont(const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        if (auto tf = f.isBold() ? semiBoldTypeface() : regularTypeface())
            return tf;
    return LookAndFeel_V4::getTypefaceForFont(f);
}

juce::Font HarmonyEngineLookAndFeel::getLabelFont(juce::Label& label)
{
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return font(12.0f, true, 0.0f);
    return font(12.0f, true, 0.0f);
}

juce::Font HarmonyEngineLookAndFeel::getComboBoxFont(juce::ComboBox&) { return font(12.5f, true, 0.0f); }
juce::Font HarmonyEngineLookAndFeel::getPopupMenuFont() { return font(13.0f, false, 0.0f); }

namespace
{
    float comboCaptionWidth(const juce::ComboBox& box)
    {
        const auto cap = box.getProperties()["caption"].toString();
        if (cap.isEmpty()) return 0.0f;
        const float capW  = juce::GlyphArrangement::getStringWidth(font(12.0f, false), cap) + 6.0f;
        const float textW = juce::GlyphArrangement::getStringWidth(font(12.5f, true), box.getText());
        // Drop the caption rather than truncate the value on narrow layouts.
        return (10.0f + capW + textW + 26.0f <= (float)box.getWidth()) ? capW : 0.0f;
    }
}

void HarmonyEngineLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
    int, int, int, int, juce::ComboBox& box)
{
    using namespace HarmonyEngineColours;
    const auto b = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height).reduced(0.5f);
    const float corner = 6.0f;
    const bool hovered = box.isMouseOver(true) || box.hasKeyboardFocus(true);
    const bool ghost = box.getProperties()["ghost"];

    auto fill = ghost ? (hovered ? panelLight : juce::Colours::transparentBlack) : panelLight;
    if (!ghost && hovered) fill = fill.brighter(isLightChassis ? 0.02f : 0.06f);
    if (isButtonDown) fill = panelLight.darker(0.1f);
    g.setColour(fill);
    g.fillRoundedRectangle(b, corner);
    g.setColour(hovered ? textPrimary.withAlpha(0.16f) : hairline());
    g.drawRoundedRectangle(b.reduced(0.5f), corner, 1.0f);

    const auto cap = box.getProperties()["caption"].toString();
    if (cap.isNotEmpty() && comboCaptionWidth(box) > 0.0f)
    {
        g.setFont(font(12.0f, false));
        g.setColour(textSecondary);
        g.drawText(cap, b.withTrimmedLeft(10.0f), juce::Justification::centredLeft, false);
    }

    const float cx = b.getRight() - 12.0f, cy = b.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 3.5f, cy - 1.75f);
    chevron.lineTo(cx, cy + 1.75f);
    chevron.lineTo(cx + 3.5f, cy - 1.75f);
    g.setColour(textSecondary);
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HarmonyEngineLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    const int left = 10 + juce::roundToInt(comboCaptionWidth(box));
    label.setBounds(left, 1, box.getWidth() - left - 22, box.getHeight() - 2);
    label.setBorderSize({});
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
}

void HarmonyEngineLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    using namespace HarmonyEngineColours;
    const auto b = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    g.setColour(isLightChassis ? panelLight : panelLight.brighter(0.03f));
    g.fillRect(b);
    g.setColour(hairline().withMultipliedAlpha(1.6f));
    g.drawRect(b, 1.0f);
}

void HarmonyEngineLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
    bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool,
    const juce::String& text, const juce::String& shortcutKeyText, const juce::Drawable*, const juce::Colour*)
{
    using namespace HarmonyEngineColours;
    if (isSeparator)
    {
        g.setColour(hairline().withMultipliedAlpha(1.4f));
        g.fillRect(area.reduced(10, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }

    auto r = area.toFloat().reduced(4.0f, 1.0f);
    if (isHighlighted && isActive)
    {
        g.setColour(textPrimary.withAlpha(0.08f));
        g.fillRoundedRectangle(r, 5.0f);
    }
    if (isTicked)
    {
        g.setColour(cyanAccent);
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({ r.getX() + 11.0f, r.getCentreY() }));
    }

    g.setColour(isActive ? textPrimary : textSecondary.withAlpha(0.5f));
    g.setFont(getPopupMenuFont());
    g.drawText(text, r.withTrimmedLeft(24.0f).withTrimmedRight(10.0f), juce::Justification::centredLeft, true);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setFont(font(12.0f, false));
        g.setColour(textSecondary);
        g.drawText(shortcutKeyText, r.withTrimmedRight(10.0f), juce::Justification::centredRight, true);
    }
}

void HarmonyEngineLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
    bool highlighted, bool down)
{
    using namespace HarmonyEngineColours;
    const auto b = button.getLocalBounds().toFloat().reduced(0.5f);
    const float corner = 6.0f;
    const auto segment = button.getProperties()["segment"].toString();
    const bool enabled = button.isEnabled();

    if (button.getProperties()["accentButton"])
    {
        auto fill = accent;
        if (highlighted) fill = accentBright;
        if (down) fill = accentDeep;
        g.setColour(fill.withMultipliedAlpha(enabled ? 1.0f : 0.4f));
        g.fillRoundedRectangle(b, corner);
        return;
    }

    const bool ghost = button.getProperties()["ghost"];
    auto fill = ghost ? juce::Colours::transparentBlack : panelLight;
    if (highlighted && enabled) fill = panelLight.brighter(isLightChassis ? 0.02f : 0.07f);
    if (down) fill = panelLight.darker(0.12f);

    const auto path = segment.isNotEmpty() ? segmentPath(b, segment, corner) : [&] { juce::Path p; p.addRoundedRectangle(b, corner); return p; }();
    g.setColour(fill);
    g.fillPath(path);
    if (!ghost || highlighted)
    {
        g.setColour(hairline());
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }
}

void HarmonyEngineLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    using namespace HarmonyEngineColours;
    const bool accentBtn = button.getProperties()["accentButton"];
    auto colour = accentBtn ? (isLightChassis ? juce::Colours::white : juce::Colour(0xff111317))
                            : (highlighted ? textPrimary : textPrimary.withAlpha(0.88f));
    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(0.38f);

    const auto icon = button.getProperties()["icon"].toString();
    if (icon.isNotEmpty())
    {
        drawIcon(g, icon, button.getLocalBounds().toFloat().reduced(6.0f), colour);
        return;
    }

    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(colour);
    g.drawText(button.getButtonText(), button.getLocalBounds().reduced(6, 0), juce::Justification::centred, false);
}

juce::Font HarmonyEngineLookAndFeel::getTextButtonFont(juce::TextButton&, int) { return font(12.0f, true, 0.0f); }

void HarmonyEngineLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float rotStart, float rotEnd, juce::Slider& slider)
{
    using namespace HarmonyEngineColours;
    constexpr float pi = juce::MathConstants<float>::pi;

    const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(5.0f);
    const float size = juce::jmin(area.getWidth(), area.getHeight());
    if (size < 16.0f) return;

    const auto c = area.getCentre();
    const float r = size * 0.5f;
    const float angle = rotStart + sliderPos * (rotEnd - rotStart);

    juce::Colour ringCol = cyanAccent;
    const juce::String role = slider.getProperties()["ringRole"].toString();
    if (role == "accent")      ringCol = accent;
    else if (role == "mint")   ringCol = mintAccent;
    else if (role == "cyan")   ringCol = cyanAccent;
    else if (slider.getProperties().contains("ringColour"))
        ringCol = juce::Colour((juce::uint32)(int)slider.getProperties()["ringColour"]);

    //==========================================================================
    // STYLE 1: HARMONY ENGINE (Vintage Ivory Cap + Knurled Gold Skirt)
    //==========================================================================
    if (activeKnobStyle == KnobStyle::VintageIvory)
    {
        const auto wellBounds = juce::Rectangle<float>(size, size).withCentre(c);
        juce::ColourGradient wellFill(panel.darker(0.12f), c.x, wellBounds.getY(), panelLight, c.x, wellBounds.getBottom(), false);
        g.setGradientFill(wellFill);
        g.fillEllipse(wellBounds);

        const float ringR = r * 0.855f;
        const float ringW = juce::jmax(2.5f, r * 0.085f);
        juce::Path groove;
        groove.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, rotEnd, true);
        g.setColour(graphBackground.withAlpha(0.9f));
        g.strokePath(groove, juce::PathStrokeType(ringW + 1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (sliderPos > 0.002f)
        {
            juce::Path value;
            value.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, angle, true);
            g.setColour(glow.withAlpha(0.35f));
            g.strokePath(value, juce::PathStrokeType(ringW * 2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(accent);
            g.strokePath(value, juce::PathStrokeType(ringW * 1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(accentBright);
            g.strokePath(value, juce::PathStrokeType(ringW * 0.55f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float bodyR = r * 0.66f;
        const auto bodyBounds = juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(c);

        // Zero-allocation vector shadow
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillEllipse(bodyBounds.expanded(1.5f).translated(0.0f, 2.5f));

        juce::ColourGradient skirt(accentBright, bodyBounds.getX(), bodyBounds.getY(), accentDeep, bodyBounds.getRight(), bodyBounds.getBottom(), false);
        skirt.addColour(0.5, accent);
        g.setGradientFill(skirt);
        g.fillEllipse(bodyBounds);

        const int ridges = 36;
        for (int k = 0; k < ridges; ++k)
        {
            const float a = angle + juce::MathConstants<float>::twoPi * (float)k / (float)ridges;
            const float facing = 0.5f + 0.5f * std::cos(a + pi * 0.25f);
            g.setColour((k % 2 == 0) ? accentDeep.darker(0.2f).withAlpha(0.45f)
                                     : juce::Colours::white.withAlpha(0.15f + 0.35f * facing));
            g.drawLine({ c.getPointOnCircumference(bodyR * 0.82f, a), c.getPointOnCircumference(bodyR * 0.98f, a) }, 0.9f);
        }

        const float capR = bodyR * 0.78f;
        const auto capBounds = juce::Rectangle<float>(capR * 2.0f, capR * 2.0f).withCentre(c);
        juce::ColourGradient capFill(juce::Colours::white, c.x - capR * 0.35f, c.y - capR * 0.5f,
            panel.darker(0.14f), c.x + capR * 0.35f, c.y + capR * 0.95f, true);
        capFill.addColour(0.35, panelLight);
        g.setGradientFill(capFill);
        g.fillEllipse(capBounds);
        g.setColour(knobOutline);
        g.drawEllipse(capBounds, 1.0f);

        const auto p1 = c.getPointOnCircumference(capR * 0.28f, angle);
        const auto p2 = c.getPointOnCircumference(capR * 0.88f, angle);
        g.setColour(accentDeep);
        g.drawLine({ p1, p2 }, 2.2f);
        return;
    }

    //==========================================================================
    // STYLE 2: THE CONDUCTOR (Minimalist Open-Ring Brass Gauge)
    //==========================================================================
    if (activeKnobStyle == KnobStyle::OrchestralRing)
    {
        const float ringR = r * 0.82f;
        const float ringW = juce::jmax(2.2f, r * 0.075f);

        const auto discBounds = juce::Rectangle<float>(ringR * 1.92f, ringR * 1.92f).withCentre(c);
        juce::ColourGradient discFill(panelLight, c.x, discBounds.getY(), graphBackground, c.x, discBounds.getBottom(), false);
        g.setGradientFill(discFill);
        g.fillEllipse(discBounds);

        g.setColour(knobOutline.brighter(0.2f));
        g.drawEllipse(discBounds, 1.2f);

        if (sliderPos > 0.002f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, angle, true);
            g.setColour(ringCol.withAlpha(0.28f));
            g.strokePath(valueArc, juce::PathStrokeType(ringW * 2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(ringCol.brighter(0.2f));
            g.strokePath(valueArc, juce::PathStrokeType(ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const auto p1 = c.getPointOnCircumference(ringR * 0.45f, angle);
        const auto p2 = c.getPointOnCircumference(ringR * 0.90f, angle);
        g.setColour(cyanBright);
        g.drawLine({ p1, p2 }, 2.2f);
        return;
    }

    //==========================================================================
    // STYLE 3: THE DRUM MASTER (3D Anodized Spherical Stage Console Knob)
    //==========================================================================
    if (activeKnobStyle == KnobStyle::AnodizedStage)
    {
        const float ringR = r * 0.86f;
        const float ringW = juce::jmax(2.6f, r * 0.085f);

        juce::Path groove;
        groove.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, rotEnd, true);
        g.setColour(graphBackground.darker(0.3f));
        g.strokePath(groove, juce::PathStrokeType(ringW + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (sliderPos > 0.002f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, angle, true);
            g.setColour(ringCol.withAlpha(0.22f));
            g.strokePath(valueArc, juce::PathStrokeType(ringW * 2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(ringCol);
            g.strokePath(valueArc, juce::PathStrokeType(ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour(textSecondary.withAlpha(0.45f));
        g.fillEllipse(c.x - 1.2f, c.y - r * 0.96f - 1.2f, 2.4f, 2.4f);

        const float bodyR = r * 0.70f;
        const auto bodyBounds = juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(c);

        g.setColour(juce::Colours::black.withAlpha(0.38f));
        g.fillEllipse(bodyBounds.expanded(1.5f).translated(0.0f, 3.0f));

        juce::ColourGradient sphereFill(panelLight.brighter(0.28f), c.x - bodyR * 0.25f, c.y - bodyR * 0.35f,
                                        backgroundDeep.darker(0.3f), c.x + bodyR * 0.35f, c.y + bodyR * 0.65f, true);
        sphereFill.addColour(0.45, panel);
        g.setGradientFill(sphereFill);
        g.fillEllipse(bodyBounds);

        g.setColour(knobOutline.brighter(0.15f));
        g.drawEllipse(bodyBounds, 1.1f);

        const auto p1 = c.getPointOnCircumference(bodyR * 0.32f, angle);
        const auto p2 = c.getPointOnCircumference(bodyR * 0.86f, angle);
        g.setColour(ringCol.withAlpha(0.35f));
        g.drawLine({ p1, p2 }, 4.0f);
        g.setColour(juce::Colours::white);
        g.drawLine({ p1, p2 }, 2.0f);
        return;
    }

    //==========================================================================
    // STYLE 4 & 5: NOVATONE STUDIO & AMPPIRE - flat thin-ring dial
    //==========================================================================
    {
        const float ringR = r * 0.88f;
        const float ringW = juce::jmax(2.5f, r * 0.075f);
        const float alpha = slider.isEnabled() ? 1.0f : 0.45f;

        juce::Path track;
        track.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, rotEnd, true);
        g.setColour(panelLight.brighter(isLightChassis ? 0.0f : 0.10f).withMultipliedAlpha(alpha));
        g.strokePath(track, juce::PathStrokeType(ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Bipolar parameters (e.g. formant) fill from the 12 o'clock position.
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float origin = bipolar ? rotStart + (float)slider.valueToProportionOfLength(0.0) * (rotEnd - rotStart) : rotStart;
        if (std::abs(angle - origin) > 0.01f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, juce::jmin(origin, angle), juce::jmax(origin, angle), true);
            g.setColour(ringCol.withMultipliedAlpha(alpha));
            g.strokePath(valueArc, juce::PathStrokeType(ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float bodyR = ringR - ringW - juce::jmax(3.0f, r * 0.10f);
        const auto body = juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(c);
        g.setColour(panelLight.withMultipliedAlpha(alpha));
        g.fillEllipse(body);
        g.setColour(hairline());
        g.drawEllipse(body.reduced(0.5f), 1.0f);

        const auto p1 = c.getPointOnCircumference(bodyR * 0.38f, angle);
        const auto p2 = c.getPointOnCircumference(bodyR * 0.80f, angle);
        g.setColour(textPrimary.withMultipliedAlpha(alpha));
        g.drawLine({ p1, p2 }, 2.0f);
    }
}

void HarmonyEngineLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    if (button.getProperties()["powerButton"])
        drawPowerButton(g, button, highlighted, down);
    else
        drawPillButton(g, button, highlighted, down);
}

void HarmonyEngineLookAndFeel::drawPowerButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    using namespace HarmonyEngineColours;
    constexpr float pi = juce::MathConstants<float>::pi;

    const bool on = !button.getToggleState();
    const auto b = button.getLocalBounds().toFloat().reduced(0.5f);
    const float d = juce::jmin(b.getWidth(), b.getHeight());
    if (d < 8.0f) return;

    const auto face = juce::Rectangle<float>(d, d).withCentre(b.getCentre());
    g.setColour(on ? mintAccent.withAlpha(0.16f) : (highlighted ? panelLight.brighter(0.06f) : panelLight));
    g.fillRoundedRectangle(face, 6.0f);
    g.setColour(on ? mintAccent.withAlpha(0.35f) : hairline());
    g.drawRoundedRectangle(face.reduced(0.5f), 6.0f, 1.0f);

    const float iconR = d * 0.22f;
    juce::Path glyph;
    glyph.addCentredArc(face.getCentreX(), face.getCentreY() + 0.5f, iconR, iconR, 0.0f, pi * 0.25f, pi * 1.75f, true);
    glyph.startNewSubPath(face.getCentreX(), face.getCentreY() - iconR * 1.25f);
    glyph.lineTo(face.getCentreX(), face.getCentreY() - iconR * 0.15f);

    g.setColour(on ? mintAccent : textSecondary);
    g.strokePath(glyph, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HarmonyEngineLookAndFeel::drawPillButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    using namespace HarmonyEngineColours;
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool engaged = button.getToggleState();
    const bool enabled = button.isEnabled();
    const float corner = 6.0f;
    const auto segment = button.getProperties()["segment"].toString();
    const bool chip = button.getProperties()["chip"];

    juce::Colour activeCol = cyanAccent;
    const auto role = button.getProperties()["pillRole"].toString();
    if (role == "accent") activeCol = accent;
    else if (role == "mint") activeCol = mintAccent;

    juce::Path shape = segment.isNotEmpty() ? segmentPath(bounds, segment, corner)
                                            : [&] { juce::Path p; p.addRoundedRectangle(bounds, corner); return p; }();

    auto fill = panelLight;
    if (engaged)
        fill = segment.isNotEmpty() ? (isLightChassis ? activeCol.withAlpha(0.22f) : activeCol.withAlpha(0.20f))
                                    : activeCol.withAlpha(0.14f);
    else if (highlighted && enabled)
        fill = panelLight.brighter(isLightChassis ? 0.02f : 0.07f);
    if (down) fill = fill.darker(0.1f);

    g.setColour(fill);
    g.fillPath(shape);
    g.setColour(engaged ? activeCol.withAlpha(segment.isNotEmpty() ? 0.0f : 0.35f) : hairline());
    g.strokePath(shape, juce::PathStrokeType(1.0f));

    auto textArea = bounds;
    auto textCol = engaged ? (isLightChassis ? activeCol.darker(0.45f) : activeCol.brighter(0.25f))
                           : (highlighted ? textPrimary : textSecondary);
    if (!enabled) textCol = textCol.withMultipliedAlpha(0.4f);

    g.setFont(font(12.0f, true, 0.0f));
    if (chip)
    {
        // Status chip: small LED dot + label, centred as a group.
        const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), button.getButtonText());
        const float groupW = tw + 12.0f;
        const float x0 = bounds.getCentreX() - groupW * 0.5f;
        g.setColour(engaged ? activeCol : textSecondary.withAlpha(0.45f));
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({ x0 + 3.0f, bounds.getCentreY() }));
        textArea = juce::Rectangle<float>(x0 + 12.0f, bounds.getY(), tw + 2.0f, bounds.getHeight());
        g.setColour(textCol);
        g.drawText(button.getButtonText(), textArea, juce::Justification::centredLeft, false);
        return;
    }

    g.setColour(textCol);
    g.drawText(button.getButtonText(), textArea.reduced(4.0f, 0.0f), juce::Justification::centred, false);
}

void HarmonyEngineLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style == juce::Slider::LinearBar && slider.getProperties()["field"])
    {
        drawValueField(g, x, y, width, height, sliderPos, slider);
        return;
    }

    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    using namespace HarmonyEngineColours;
    const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height);
    const float thumbR = juce::jmin(6.0f, area.getHeight() * 0.32f);

    const auto groove = area.withSizeKeepingCentre(area.getWidth() - 2.0f * thumbR, 3.0f);
    g.setColour(panelLight.brighter(isLightChassis ? 0.0f : 0.10f).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(groove, 1.5f);

    juce::Colour fillCol = cyanAccent;
    const auto role = slider.getProperties()["ringRole"].toString();
    if (role == "accent") fillCol = accent;
    else if (role == "mint") fillCol = mintAccent;

    const float clampedX = juce::jlimit(groove.getX(), groove.getRight(), sliderPos);
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float originX = bipolar ? groove.getX() + groove.getWidth() * (float)slider.valueToProportionOfLength(0.0) : groove.getX();
    g.setColour(fillCol.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(juce::Rectangle<float>(juce::jmin(originX, clampedX), groove.getY(),
                                                  std::abs(clampedX - originX), groove.getHeight()), 1.5f);

    const auto c = juce::Point<float>(clampedX, area.getCentreY());
    g.setColour(juce::Colours::black.withAlpha(0.25f * alpha));
    g.fillEllipse(juce::Rectangle<float>(2.0f * thumbR + 2.0f, 2.0f * thumbR + 2.0f).withCentre(c.translated(0.0f, 0.5f)));
    g.setColour((isLightChassis ? juce::Colours::white : textPrimary).withMultipliedAlpha(alpha));
    g.fillEllipse(juce::Rectangle<float>(2.0f * thumbR, 2.0f * thumbR).withCentre(c));
}

void HarmonyEngineLookAndFeel::drawValueField(juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, juce::Slider& slider)
{
    using namespace HarmonyEngineColours;
    const bool enabled = slider.isEnabled();
    const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(0.5f);
    const float r = 6.0f;

    g.setColour(panelLight.withMultipliedAlpha(enabled ? 1.0f : 0.6f));
    g.fillRoundedRectangle(area, r);

    juce::Colour fillCol = cyanAccent;
    const auto role = slider.getProperties()["ringRole"].toString();
    if (role == "accent") fillCol = accent;
    else if (role == "mint") fillCol = mintAccent;

    if (enabled)
    {
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float originX = bipolar ? area.getX() + area.getWidth() * (float)slider.valueToProportionOfLength(0.0) : area.getX();
        const float px = juce::jlimit(area.getX(), area.getRight(), sliderPos);
        juce::Graphics::ScopedSaveState state(g);
        juce::Path clip;
        clip.addRoundedRectangle(area, r);
        g.reduceClipRegion(clip);
        g.setColour(fillCol.withAlpha(isLightChassis ? 0.28f : 0.22f));
        g.fillRect(juce::Rectangle<float>(juce::jmin(originX, px), area.getY(), std::abs(px - originX), area.getHeight()));
        g.setColour(fillCol);
        g.fillRect(juce::Rectangle<float>(1.5f, area.getHeight()).withCentre({ px, area.getCentreY() }));
    }

    g.setColour(slider.isMouseOverOrDragging() && enabled ? textPrimary.withAlpha(0.18f) : hairline());
    g.drawRoundedRectangle(area.reduced(0.5f), r, 1.0f);

    auto text = area.reduced(9.0f, 0.0f);
    g.setFont(font(12.0f, false));
    g.setColour(textSecondary.withMultipliedAlpha(enabled ? 1.0f : 0.6f));
    g.drawText(slider.getProperties()["caption"].toString(), text, juce::Justification::centredLeft, false);
    g.setFont(font(12.0f, true));
    g.setColour(textPrimary.withMultipliedAlpha(enabled ? 1.0f : 0.4f));
    g.drawText(enabled ? slider.getTextFromValue(slider.getValue()) : juce::String("-"), text, juce::Justification::centredRight, false);
}

//==============================================================================
// ROTARY KNOB
//==============================================================================
RotaryKnob::RotaryKnob(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
    const juce::String& labelText, const juce::String& subtitleText,
    juce::Colour customRingColour, const juce::String& semanticRingRole)
    : subtitle(subtitleText), attachment(apvts, paramID, slider)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 18);
    slider.setLookAndFeel(&HarmonyEngineLookAndFeel::shared());
    slider.setMouseDragSensitivity(200);
    slider.setColour(juce::Slider::textBoxTextColourId, HarmonyEngineColours::textSecondary);
    if (!customRingColour.isTransparent())
        slider.getProperties().set("ringColour", (int)customRingColour.getARGB());
    if (semanticRingRole.isNotEmpty())
        slider.getProperties().set("ringRole", semanticRingRole);
    addAndMakeVisible(slider);

    label.setText(labelText, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    label.setFont(font(12.5f, true, 0.0f));
    label.setBorderSize({});
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

RotaryKnob::~RotaryKnob() { slider.setLookAndFeel(nullptr); }

void RotaryKnob::refreshThemeColours()
{
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    slider.setColour(juce::Slider::textBoxTextColourId, HarmonyEngineColours::textSecondary);
    repaint();
}

void RotaryKnob::setSuggestedValue(double value)
{
    const juce::var proportion = std::isfinite(value)
        ? juce::var(juce::jlimit(0.0, 1.0, slider.valueToProportionOfLength(value)))
        : juce::var();
    if (slider.getProperties()["suggestion"] == proportion) return;
    slider.getProperties().set("suggestion", proportion);
    slider.repaint();
}

void RotaryKnob::paint(juce::Graphics& g)
{
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    if (subtitle.isNotEmpty() && getHeight() >= 112)
    {
        g.setFont(font(11.0f, false));
        g.setColour(HarmonyEngineColours::textSecondary);
        g.drawText(subtitle, getLocalBounds().withTrimmedTop(16).removeFromTop(15), juce::Justification::centred, false);
    }
}

void RotaryKnob::resized()
{
    auto bounds = getLocalBounds();
    label.setBounds(bounds.removeFromTop(16));
    if (subtitle.isNotEmpty() && getHeight() >= 112)
        bounds.removeFromTop(17);
    else
        bounds.removeFromTop(2);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, juce::jmax(40, bounds.getWidth()), 18);
    slider.setBounds(bounds);
}

//==============================================================================
// CHOICE SELECTOR
//==============================================================================
ChoiceSelector::ChoiceSelector(juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterID,
    const juce::String& captionText, const juce::String& tooltip)
    : caption(captionText)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(parameterID)))
        box.addItemList(choice->choices, 1);
    box.setLookAndFeel(&HarmonyEngineLookAndFeel::shared());
    box.setTooltip(tooltip);
    box.getProperties().set("caption", captionText);
    addAndMakeVisible(box);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(apvts, parameterID, box);
}

ChoiceSelector::~ChoiceSelector() { box.setLookAndFeel(nullptr); }

void ChoiceSelector::paint(juce::Graphics&) {}

void ChoiceSelector::resized() { box.setBounds(getLocalBounds()); }

//==============================================================================
// MODULE HEADER (now just the module's bypass switch) + TAB BASE
//==============================================================================
ModuleHeader::ModuleHeader(juce::AudioProcessorValueTreeState& apvts, const juce::String& moduleId,
    const juce::String& titleText, const juce::String& subtitleText)
    : title(titleText), subtitle(subtitleText),
    powerAttachment(apvts, moduleId + "_bypass", powerButton)
{
    HarmonyEngineLookAndFeel::makePowerButton(powerButton);
    powerButton.setTooltip("Pitch correction on/off");
    addAndMakeVisible(powerButton);

    lastBypassed = powerButton.getToggleState();
    powerButton.onStateChange = [this]
    {
        if (isBypassed() == lastBypassed) return;
        lastBypassed = isBypassed();
        repaint();
        if (onBypassChanged) onBypassChanged();
    };
}

ModuleHeader::~ModuleHeader()
{
    powerButton.setLookAndFeel(nullptr);
}

void ModuleHeader::setTrailingComponent(juce::Component* component, int width, int height)
{
    trailingComponent = component;
    trailingWidth = width;
    trailingHeight = height;
    if (component != nullptr) addAndMakeVisible(component);
    resized();
}

void ModuleHeader::paint(juce::Graphics&) {}

void ModuleHeader::resized()
{
    auto r = getLocalBounds();
    const int d = juce::jmin(r.getWidth(), r.getHeight());
    powerButton.setBounds(r.removeFromLeft(d).withSizeKeepingCentre(d, d));
    if (trailingComponent != nullptr)
        trailingComponent->setBounds(r.removeFromRight(trailingWidth).withSizeKeepingCentre(trailingWidth, trailingHeight));
}

ModuleTab::ModuleTab(juce::AudioProcessorValueTreeState& apvts, const juce::String& moduleId,
    const juce::String& title, const juce::String& subtitle)
    : header(apvts, moduleId, title, subtitle)
{
    addAndMakeVisible(header);
    header.onBypassChanged = [this] { repaint(); };
}

juce::Rectangle<int> ModuleTab::getBodyBounds() const
{
    return getLocalBounds().reduced(12, 10);
}

void ModuleTab::paint(juce::Graphics& g)
{
    paintChassis(g, getLocalBounds());
    paintBody(g);
}

void ModuleTab::paintOverChildren(juce::Graphics& g)
{
    if (header.isBypassed())
        paintBypassVeil(g, bypassVeilArea.isEmpty() ? getBodyBounds() : bypassVeilArea);
}

void ModuleTab::resized()
{
    layoutBody(getBodyBounds());
}
