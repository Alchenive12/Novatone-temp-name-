#include "ModuleTabs.h"
#include "HarmonyEngineColours.h"
#include <cmath>
#include <limits>

//==============================================================================
// MULTI-THEME ALCHENIVE STUDIO DRAWING KIT
// (Zero-allocation vector shadows eliminate heap Image blurs during live resize/drag)
//==============================================================================
namespace HarmonyEngineUI
{
    juce::Font font(float height, bool bold, float kerning)
    {
        return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain)
            .withKerningFactor(kerning));
    }

    void paintChassis(juce::Graphics& g, juce::Rectangle<int> boundsInt)
    {
        using namespace HarmonyEngineColours;
        const auto b = boundsInt.toFloat();

        juce::ColourGradient base(background, b.getCentreX(), b.getY(), backgroundDeep, b.getCentreX(), b.getBottom(), false);
        g.setGradientFill(base);
        g.fillRect(boundsInt);

        const float haloAlpha = isLightChassis ? 0.22f : 0.09f;
        juce::ColourGradient halo(glow.withAlpha(haloAlpha), b.getCentreX(), b.getY() - b.getHeight() * 0.08f,
            glow.withAlpha(0.0f), b.getCentreX(), b.getY() + b.getHeight() * 0.65f, true);
        g.setGradientFill(halo);
        g.fillRect(boundsInt);

        juce::ColourGradient vignette(juce::Colours::transparentBlack, b.getCentreX(), b.getCentreY(),
            juce::Colours::black.withAlpha(isLightChassis ? 0.06f : 0.24f), b.getX(), b.getY(), true);
        g.setGradientFill(vignette);
        g.fillRect(boundsInt);
    }

    void drawLiftedPanel(juce::Graphics& g, juce::Rectangle<int> areaInt, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (areaInt.isEmpty()) return;

        const auto area = areaInt.toFloat();

        // Zero-allocation analytical multi-layer drop shadow (replaces heap-allocating juce::DropShadow::drawForPath)
        const float baseShadowAlpha = isLightChassis ? 0.045f : 0.14f;
        g.setColour(juce::Colours::black.withAlpha(baseShadowAlpha));
        g.fillRoundedRectangle(area.expanded(2.0f, 2.0f).translated(0.0f, 4.0f), cornerRadius + 2.0f);
        g.setColour(juce::Colours::black.withAlpha(baseShadowAlpha * 1.4f));
        g.fillRoundedRectangle(area.expanded(0.8f, 0.8f).translated(0.0f, 2.0f), cornerRadius + 1.0f);

        juce::Path panelPath;
        panelPath.addRoundedRectangle(area, cornerRadius);

        juce::ColourGradient fill(panelLight, area.getCentreX(), area.getY(),
            panel, area.getCentreX(), area.getBottom(), false);
        g.setGradientFill(fill);
        g.fillPath(panelPath);

        // Top specular rim highlight
        juce::Path rim;
        rim.addRoundedRectangle(area.reduced(0.75f), juce::jmax(1.0f, cornerRadius - 0.75f));
        const float rimAlpha = isLightChassis ? 0.90f : 0.14f;
        juce::ColourGradient rimLight(juce::Colours::white.withAlpha(rimAlpha), area.getCentreX(), area.getY(),
            juce::Colours::white.withAlpha(0.0f), area.getCentreX(), area.getY() + juce::jmin(28.0f, area.getHeight() * 0.45f), false);
        g.setGradientFill(rimLight);
        g.strokePath(rim, juce::PathStrokeType(1.1f));

        g.setColour(knobOutline.withAlpha(isLightChassis ? 0.75f : 0.80f));
        g.strokePath(panelPath, juce::PathStrokeType(1.0f));
    }

    void drawInsetGlass(juce::Graphics& g, juce::Rectangle<int> areaInt, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (areaInt.isEmpty()) return;

        const auto area = areaInt.toFloat();
        juce::Path outer;
        outer.addRoundedRectangle(area, cornerRadius);

        if (isLightChassis)
        {
            juce::ColourGradient bezel(accentBright, area.getX(), area.getY(), accentDeep, area.getX(), area.getBottom(), false);
            bezel.addColour(0.45, accent);
            g.setGradientFill(bezel);
            g.fillPath(outer);

            const auto inner = area.reduced(2.0f);
            juce::Path glass;
            glass.addRoundedRectangle(inner, juce::jmax(1.0f, cornerRadius - 2.0f));
            g.setColour(graphBackground);
            g.fillPath(glass);
        }
        else
        {
            g.setColour(graphBackground);
            g.fillPath(outer);
            g.setColour(graphOutline);
            g.strokePath(outer, juce::PathStrokeType(1.4f));
        }
    }

    void drawGlassGloss(juce::Graphics& g, juce::Rectangle<int> areaInt, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (areaInt.isEmpty() || !isLightChassis) return;

        const auto inner = areaInt.toFloat().reduced(2.5f);
        juce::Path glass;
        glass.addRoundedRectangle(inner, juce::jmax(1.0f, cornerRadius - 2.5f));
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(glass);

        const float glossHeight = inner.getHeight() * 0.35f;
        juce::ColourGradient gloss(juce::Colours::white.withAlpha(0.06f), inner.getX(), inner.getY(),
            juce::Colours::white.withAlpha(0.0f), inner.getX(), inner.getY() + glossHeight, false);
        g.setGradientFill(gloss);
        g.fillRect(inner.withHeight(glossHeight));
    }

    void drawInsetWell(juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius)
    {
        using namespace HarmonyEngineColours;
        if (area.isEmpty()) return;

        g.setColour(graphBackground);
        g.fillRoundedRectangle(area, cornerRadius);
        g.setColour(graphOutline);
        g.drawRoundedRectangle(area, cornerRadius, 1.0f);
    }

    void paintBypassVeil(juce::Graphics& g, juce::Rectangle<int> body)
    {
        using namespace HarmonyEngineColours;
        const auto veil = body.toFloat().expanded(4.0f);
        g.setColour(backgroundDeep.withAlpha(0.76f));
        g.fillRoundedRectangle(veil, 12.0f);

        const auto f = font(12.0f, true, 0.35f);
        const float w = juce::GlyphArrangement::getStringWidth(f, "PITCH ENGINE BYPASSED") + 40.0f;
        const auto badge = juce::Rectangle<float>(w, 30.0f).withCentre(veil.getCentre());
        g.setColour(panelLight);
        g.fillRoundedRectangle(badge, 15.0f);
        g.setColour(accent);
        g.drawRoundedRectangle(badge, 15.0f, 1.2f);

        g.setFont(f);
        g.setColour(textPrimary);
        g.drawText("PITCH ENGINE BYPASSED", badge, juce::Justification::centred);
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

juce::Font HarmonyEngineLookAndFeel::getLabelFont(juce::Label& label)
{
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        return font(12.0f, true, 0.04f);
    return font(11.5f, true, 0.14f);
}

juce::Font HarmonyEngineLookAndFeel::getComboBoxFont(juce::ComboBox&) { return font(11.5f, true, 0.08f); }
juce::Font HarmonyEngineLookAndFeel::getPopupMenuFont() { return font(12.5f, false, 0.04f); }

void HarmonyEngineLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
    int, int, int, int, juce::ComboBox& box)
{
    using namespace HarmonyEngineColours;
    const auto b = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height).reduced(1.0f);
    const float corner = 6.0f;

    const auto fieldBg = isLightChassis ? panel.darker(isButtonDown ? 0.08f : 0.03f)
                                        : (isButtonDown ? graphBackground.darker(0.2f) : graphBackground);
    g.setColour(fieldBg);
    g.fillRoundedRectangle(b, corner);

    const bool hovered = box.hasKeyboardFocus(true) || box.isMouseOver(true);
    g.setColour(hovered ? cyanAccent : knobOutline);
    g.drawRoundedRectangle(b, corner, 1.2f);

    const float cx = b.getRight() - 12.0f, cy = b.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.5f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(isLightChassis ? accentDeep : cyanBright);
    g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HarmonyEngineLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(10, 1, box.getWidth() - 28, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
}

void HarmonyEngineLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    using namespace HarmonyEngineColours;
    const auto b = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    g.setColour(panelLight);
    g.fillRect(b);
    g.setColour(cyanAccent.withAlpha(0.65f));
    g.drawRect(b, 1.0f);
}

void HarmonyEngineLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
    bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool,
    const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    using namespace HarmonyEngineColours;
    if (isSeparator)
    {
        g.setColour(knobOutline);
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }

    auto r = area.toFloat().reduced(3.0f, 1.0f);
    if (isHighlighted && isActive)
    {
        g.setColour(cyanAccent.withAlpha(0.25f));
        g.fillRoundedRectangle(r, 4.0f);
    }
    if (isTicked)
    {
        g.setColour(mintAccent);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({ r.getX() + 10.0f, r.getCentreY() }));
    }

    g.setColour(isActive ? textPrimary : textSecondary.withAlpha(0.45f));
    g.setFont(getPopupMenuFont());
    g.drawText(text, r.withTrimmedLeft(22.0f), juce::Justification::centredLeft, true);
}

void HarmonyEngineLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
    bool highlighted, bool down)
{
    using namespace HarmonyEngineColours;
    const auto b = button.getLocalBounds().toFloat().reduced(1.5f);
    const float corner = (activeKnobStyle == KnobStyle::AnodizedStage) ? b.getHeight() * 0.48f : 6.0f;

    const bool isAccentBtn = button.getProperties()["accentButton"];
    if (isAccentBtn)
    {
        juce::ColourGradient fill(accentBright.withAlpha(down ? 0.78f : 0.92f), b.getX(), b.getY(),
                                  accentDeep.withAlpha(0.92f), b.getX(), b.getBottom(), false);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(b, corner);
        g.setColour(accentBright);
        g.drawRoundedRectangle(b, corner, 1.0f);
        return;
    }

    const auto baseFill = isLightChassis
        ? (down ? panel.darker(0.08f) : (highlighted ? panelLight.brighter(0.04f) : panelLight))
        : (down ? graphBackground : (highlighted ? panelLight.brighter(0.12f) : panelLight));

    g.setColour(baseFill);
    g.fillRoundedRectangle(b, corner);
    g.setColour(highlighted ? cyanAccent.withAlpha(0.85f) : knobOutline);
    g.drawRoundedRectangle(b, corner, 1.0f);
}

juce::Font HarmonyEngineLookAndFeel::getTextButtonFont(juce::TextButton&, int) { return font(10.5f, true, 0.14f); }

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
    // STYLE 4 & 5: NOVATONE STUDIO & AMPPIRE VALVE RIG
    //==========================================================================
    const float ringR = r * 0.86f;
    const float ringW = juce::jmax(3.2f, r * 0.11f);
    juce::Path groove;
    groove.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, rotEnd, true);
    g.setColour(graphBackground);
    g.strokePath(groove, juce::PathStrokeType(ringW + 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (sliderPos > 0.002f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc(c.x, c.y, ringR, ringR, 0.0f, rotStart, angle, true);
        g.setColour(ringCol.withAlpha(0.26f));
        g.strokePath(valueArc, juce::PathStrokeType(ringW * 2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(ringCol);
        g.strokePath(valueArc, juce::PathStrokeType(ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float bodyR = r * 0.68f;
    const auto bodyBounds = juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre(c);

    g.setColour(juce::Colours::black.withAlpha(0.34f));
    g.fillEllipse(bodyBounds.expanded(1.5f).translated(0.0f, 2.5f));

    juce::ColourGradient capFill(panelLight.brighter(0.18f), c.x, bodyBounds.getY(),
                                 backgroundDeep, c.x, bodyBounds.getBottom(), false);
    g.setGradientFill(capFill);
    g.fillEllipse(bodyBounds);

    g.setColour(knobOutline.brighter(0.18f));
    g.drawEllipse(bodyBounds, 1.2f);

    const auto p1 = c.getPointOnCircumference(bodyR * 0.28f, angle);
    const auto p2 = c.getPointOnCircumference(bodyR * 0.82f, angle);
    g.setColour(ringCol.brighter(0.35f));
    g.drawLine({ p1, p2 }, 2.2f);

    if (!slider.isEnabled())
    {
        g.setColour(backgroundDeep.withAlpha(0.55f));
        g.fillEllipse(area);
    }
}

void HarmonyEngineLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    if (button.getProperties()["powerButton"])
        drawPowerButton(g, button, highlighted, down);
    else
        drawPillButton(g, button, highlighted, down);
}

void HarmonyEngineLookAndFeel::drawPowerButton(juce::Graphics& g, juce::ToggleButton& button, bool, bool down)
{
    using namespace HarmonyEngineColours;
    constexpr float pi = juce::MathConstants<float>::pi;

    const bool on = !button.getToggleState();
    const auto b = button.getLocalBounds().toFloat();
    const float d = juce::jmin(b.getWidth(), b.getHeight()) - 4.0f;
    if (d < 8.0f) return;

    const auto c = b.getCentre();
    const auto face = juce::Rectangle<float>(d, d).withCentre(c.translated(0.0f, down ? 0.5f : 0.0f));

    const auto activeCol = isLightChassis ? accent : mintAccent;
    g.setColour(on ? activeCol.withAlpha(0.22f) : (isLightChassis ? panel : graphBackground));
    g.fillEllipse(face);
    g.setColour(on ? activeCol : knobOutline);
    g.drawEllipse(face, 1.5f);

    const float iconR = d * 0.24f;
    juce::Path glyph;
    glyph.addCentredArc(face.getCentreX(), face.getCentreY() + 1.0f, iconR, iconR, 0.0f, pi * 0.23f, pi * 1.77f, true);
    glyph.startNewSubPath(face.getCentreX(), face.getCentreY() - iconR * 1.2f);
    glyph.lineTo(face.getCentreX(), face.getCentreY());

    g.setColour(on ? (isLightChassis ? accentDeep : activeCol.brighter(0.3f)) : textSecondary);
    g.strokePath(glyph, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HarmonyEngineLookAndFeel::drawPillButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    using namespace HarmonyEngineColours;
    const auto bounds = button.getLocalBounds().toFloat().reduced(1.5f).translated(0.0f, down ? 0.5f : 0.0f);
    const bool engaged = button.getToggleState();
    const float corner = (activeKnobStyle == KnobStyle::AnodizedStage) ? bounds.getHeight() * 0.48f : 6.0f;

    if (engaged)
    {
        juce::Colour activeCol = cyanAccent;
        if (button.getProperties().contains("pillRole"))
        {
            const auto role = button.getProperties()["pillRole"].toString();
            if (role == "accent") activeCol = accent;
            else if (role == "mint") activeCol = mintAccent;
        }
        else if (button.getProperties().contains("pillColour"))
        {
            activeCol = accent;
        }

        if (isLightChassis)
        {
            juce::ColourGradient leaf(accentBright, bounds.getX(), bounds.getY(), accentDeep, bounds.getX(), bounds.getBottom(), false);
            leaf.addColour(0.5, accent);
            g.setGradientFill(leaf);
            g.fillRoundedRectangle(bounds, corner);
        }
        else
        {
            g.setColour(activeCol.withAlpha(0.24f));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(activeCol);
            g.drawRoundedRectangle(bounds, corner, 1.4f);
        }
    }
    else
    {
        g.setColour(isLightChassis
            ? (highlighted ? panelLight.brighter(0.04f) : panel)
            : (highlighted ? panelLight.brighter(0.10f) : graphBackground));
        g.fillRoundedRectangle(bounds, corner);
        g.setColour(highlighted ? cyanAccent.withAlpha(0.6f) : knobOutline);
        g.drawRoundedRectangle(bounds, corner, 1.0f);
    }

    g.setColour(engaged ? textPrimary : (highlighted ? textPrimary : textSecondary));
    g.setFont(font(10.5f, true, 0.14f));
    g.drawText(button.getButtonText(), bounds, juce::Justification::centred);
}

void HarmonyEngineLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    using namespace HarmonyEngineColours;
    const float alpha = slider.isEnabled() ? 1.0f : 0.4f;
    const auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height);
    const float thumbR = juce::jmin(7.5f, area.getHeight() * 0.38f);

    const auto groove = area.withSizeKeepingCentre(area.getWidth() - 2.0f * thumbR, 5.0f);
    g.setColour((isLightChassis ? panel.darker(0.12f) : graphBackground).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(groove, 2.5f);

    const float clampedX = juce::jlimit(groove.getX(), groove.getRight(), sliderPos);
    const auto fillRect = groove.withRight(clampedX);
    g.setColour(cyanAccent.withMultipliedAlpha(0.80f * alpha));
    g.fillRoundedRectangle(fillRect, 2.5f);

    g.setColour(knobOutline.withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(groove, 2.5f, 1.0f);

    const auto c = juce::Point<float>(clampedX, area.getCentreY());
    g.setColour(cyanBright.withMultipliedAlpha(alpha));
    g.fillEllipse(c.x - thumbR, c.y - thumbR, 2.0f * thumbR, 2.0f * thumbR);
    g.setColour(juce::Colours::white.withMultipliedAlpha(alpha));
    g.fillEllipse(c.x - thumbR * 0.4f, c.y - thumbR * 0.4f, 0.8f * thumbR, 0.8f * thumbR);
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
    if (!customRingColour.isTransparent())
        slider.getProperties().set("ringColour", (int)customRingColour.getARGB());
    if (semanticRingRole.isNotEmpty())
        slider.getProperties().set("ringRole", semanticRingRole);
    addAndMakeVisible(slider);

    label.setText(labelText, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    label.setFont(font(11.5f, true, 0.16f));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

RotaryKnob::~RotaryKnob() { slider.setLookAndFeel(nullptr); }

void RotaryKnob::refreshThemeColours()
{
    label.setColour(juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    slider.setColour(juce::Slider::textBoxTextColourId, HarmonyEngineColours::textPrimary);
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
    if (subtitle.isNotEmpty() && getHeight() >= 116)
    {
        g.setFont(font(9.0f, false, 0.06f));
        g.setColour(HarmonyEngineColours::textSecondary);
        g.drawText(subtitle, getLocalBounds().removeFromTop(32).withTrimmedTop(16), juce::Justification::centred, false);
    }
}

void RotaryKnob::resized()
{
    auto bounds = getLocalBounds();
    label.setBounds(bounds.removeFromTop(17));
    if (subtitle.isNotEmpty() && getHeight() >= 116)
        bounds.removeFromTop(14);
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
    addAndMakeVisible(box);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(apvts, parameterID, box);
}

ChoiceSelector::~ChoiceSelector() { box.setLookAndFeel(nullptr); }

void ChoiceSelector::paint(juce::Graphics& g)
{
    g.setFont(font(10.0f, true, 0.2f));
    g.setColour(HarmonyEngineColours::textSecondary);
    g.drawText(caption, getLocalBounds().withWidth(getWidth() - 110), juce::Justification::centredRight);
}

void ChoiceSelector::resized() { box.setBounds(getLocalBounds().removeFromRight(102)); }

//==============================================================================
// MODULE HEADER + TAB BASE
//==============================================================================
ModuleHeader::ModuleHeader(juce::AudioProcessorValueTreeState& apvts, const juce::String& moduleId,
    const juce::String& titleText, const juce::String& subtitleText)
    : title(titleText), subtitle(subtitleText),
    powerAttachment(apvts, moduleId + "_bypass", powerButton)
{
    HarmonyEngineLookAndFeel::makePowerButton(powerButton);
    powerButton.setTooltip("Power: switch " + titleText + " on or off");
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

void ModuleHeader::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto strip = getLocalBounds().reduced(14, 4);
    drawLiftedPanel(g, strip, 10.0f);

    auto text = strip.toFloat().reduced(10.0f, 0.0f);
    text.removeFromLeft((float)powerButton.getWidth() + 12.0f);
    if (trailingComponent != nullptr)
        text.removeFromRight((float)trailingComponent->getWidth() + 12.0f);

    const bool bypassed = isBypassed();
    const auto titleFont = font(14.0f, true, 0.22f);
    const float titleWidth = juce::GlyphArrangement::getStringWidth(titleFont, title) + 6.0f;
    const auto titleArea = text.removeFromLeft(titleWidth);

    g.setFont(titleFont);
    g.setColour(bypassed ? textSecondary : (isLightChassis ? accentDeep : cyanBright));
    g.drawText(title, titleArea, juce::Justification::centredLeft);

    text.removeFromLeft(10.0f);
    g.setColour(knobOutline);
    g.fillRect(juce::Rectangle<float>(1.0f, 14.0f).withCentre({ text.getX(), text.getCentreY() }));
    text.removeFromLeft(10.0f);

    g.setFont(font(10.0f, true, 0.22f));
    g.setColour(textSecondary);
    g.drawText(subtitle, text, juce::Justification::centredLeft, true);
}

void ModuleHeader::resized()
{
    auto strip = getLocalBounds().reduced(14, 4).reduced(10, 0);
    powerButton.setBounds(strip.removeFromLeft(28).withSizeKeepingCentre(28, 28));
    if (trailingComponent != nullptr)
        trailingComponent->setBounds(strip.removeFromRight(trailingWidth).withSizeKeepingCentre(trailingWidth, trailingHeight));
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
    return getLocalBounds().withTrimmedTop(headerHeight).reduced(14, 8);
}

void ModuleTab::paint(juce::Graphics& g)
{
    paintChassis(g, getLocalBounds());
    paintBody(g);
}

void ModuleTab::paintOverChildren(juce::Graphics& g)
{
    if (header.isBypassed())
        paintBypassVeil(g, getBodyBounds());
}

void ModuleTab::resized()
{
    header.setBounds(getLocalBounds().removeFromTop(headerHeight));
    layoutBody(getBodyBounds());
}