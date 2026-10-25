/*
  ==============================================================================

    HarmonyEngineColours.h
    "NovaTone" Multi-Plugin Alchenive Studio Theme System.

    Supports 5 instant-switchable themes across all translation units via C++20
    inline namespace colour state:
      0. NovaTone Studio  - Neutral Graphite, Electric Cyan & Warm Amber
      1. Harmony Engine   — Vintage Pearl Ivory & Metallic Gold
      2. The Drum Master  — Theatrical Noir Stage, Seafoam Mint & Crimson Coral
      3. The Conductor    — Concert Hall Espresso, Champagne Brass & Warm Tungsten
      4. AmpPire Valve    — Obsidian Tolex/Carbon, Valve Amber & Ruby Jewel

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace HarmonyEngineColours
{
    enum class ThemeId : int
    {
        NovaStudio    = 0,
        HarmonyEngine = 1,
        DrumMaster    = 2,
        TheConductor  = 3,
        AmpPire       = 4
    };

    enum class KnobStyle : int
    {
        StudioModern   = 0,
        VintageIvory   = 1,
        AnodizedStage  = 2,
        OrchestralRing = 3,
        BoutiqueValve  = 4
    };

    inline ThemeId   activeThemeId   = ThemeId::NovaStudio;
    inline KnobStyle activeKnobStyle = KnobStyle::StudioModern;
    inline bool      isLightChassis  = false;

    //==========================================================================
    // Dynamic Semantic Palette (C++17/20 inline shared across translation units)
    inline juce::Colour background           (0xff0f1116);
    inline juce::Colour backgroundDeep       (0xff0a0b0f);
    inline juce::Colour panel                (0xff161920);
    inline juce::Colour panelLight           (0xff1f232c);

    inline juce::Colour accent               (0xffff8a3d);
    inline juce::Colour accentBright         (0xffffa766);
    inline juce::Colour accentDeep           (0xffd9682a);

    inline juce::Colour cyanAccent           (0xff38bdf8);
    inline juce::Colour cyanBright           (0xff7dd3fc);
    inline juce::Colour cyanDeep             (0xff0ea5e9);

    inline juce::Colour mintAccent           (0xff2dd4bf);
    inline juce::Colour glow                 (0xff38bdf8);

    inline juce::Colour textPrimary          (0xffe8eaef);
    inline juce::Colour textSecondary        (0xff8b92a1);

    inline juce::Colour knobOutline          (0xff2a2f3a);
    inline juce::Colour shadowColour         (0x66000000);

    inline juce::Colour graphBackground      (0xff0b0d11);
    inline juce::Colour graphOutline         (0xff22262f);
    inline juce::Colour graphGridLine        (0xff171a21);
    inline juce::Colour graphGridLineBright  (0xff252a34);
    inline juce::Colour vuArc                (0xff38bdf8);

    inline juce::Colour pitchTraceBackground  (0xff0b0d11);
    inline juce::Colour pitchTraceUncorrected (0xff6b7383);
    inline juce::Colour pitchTraceHeroCore    (0xffffffff);
    inline juce::Colour pitchTraceHeroGold    (0xff38bdf8);
    inline juce::Colour pitchTraceHeroHalo    (0xff0ea5e9);

    inline juce::Colour pianoWhiteKey        (0xffd6dae2);
    inline juce::Colour pianoWhiteKeyShadow  (0xffb4bac6);
    inline juce::Colour pianoBlackKey        (0xff1a1d24);
    inline juce::Colour pianoBlackKeyGleam   (0xff2a2f3a);
    inline juce::Colour pianoKeyActiveGlow   (0xff2dd4bf);
    inline juce::Colour pianoKeyActiveGold   (0xffff8a3d);

    inline void applyTheme(ThemeId id) noexcept
    {
        activeThemeId = id;

        switch (id)
        {
            case ThemeId::HarmonyEngine: // Vintage White / Pearl Ivory & Metallic Gold
                activeKnobStyle       = KnobStyle::VintageIvory;
                isLightChassis        = true;
                background            = juce::Colour(0xfffdfbf7);
                backgroundDeep        = juce::Colour(0xfff5f0e6);
                panel                 = juce::Colour(0xfff0eade);
                panelLight            = juce::Colour(0xfffaf6ec);
                accent                = juce::Colour(0xffd4af37);
                accentBright          = juce::Colour(0xffe9cf6b);
                accentDeep            = juce::Colour(0xff9c7a1f);
                cyanAccent            = juce::Colour(0xffc99e2c);
                cyanBright            = juce::Colour(0xffe9cf6b);
                cyanDeep              = juce::Colour(0xff8c6a15);
                mintAccent            = juce::Colour(0xffd4af37);
                glow                  = juce::Colour(0xfffae58e);
                textPrimary           = juce::Colour(0xff333333);
                textSecondary         = juce::Colour(0xff6b6459);
                knobOutline           = juce::Colour(0xffc9bfa8);
                shadowColour          = juce::Colour(0x33000000);
                graphBackground       = juce::Colour(0xff2e2a22);
                graphOutline          = juce::Colour(0xffd4af37);
                graphGridLine         = juce::Colour(0xff4a453a);
                graphGridLineBright   = juce::Colour(0xff8c7640);
                vuArc                 = juce::Colour(0xfffae58e);
                pitchTraceBackground  = juce::Colour(0xff2e2a22);
                pitchTraceUncorrected = juce::Colour(0xff9a948a);
                pitchTraceHeroCore    = juce::Colour(0xfffffdf6);
                pitchTraceHeroGold    = juce::Colour(0xffd4af37);
                pitchTraceHeroHalo    = juce::Colour(0xfffae58e);
                pianoWhiteKey         = juce::Colour(0xfffdfbf7);
                pianoWhiteKeyShadow   = juce::Colour(0xffe4dcc8);
                pianoBlackKey         = juce::Colour(0xff333333);
                pianoBlackKeyGleam    = juce::Colour(0xff55524a);
                pianoKeyActiveGlow    = juce::Colour(0xfffae58e);
                pianoKeyActiveGold    = juce::Colour(0xffd4af37);
                break;

            case ThemeId::DrumMaster: // "The Drum Master" Noir Stage, Seafoam Mint & Stage Crimson
                activeKnobStyle       = KnobStyle::AnodizedStage;
                isLightChassis        = false;
                background            = juce::Colour(0xff10131a);
                backgroundDeep        = juce::Colour(0xff090b0f);
                panel                 = juce::Colour(0xff12151d);
                panelLight            = juce::Colour(0xff1a1f2b);
                accent                = juce::Colour(0xffec4858); // Stage Crimson / Coral
                accentBright          = juce::Colour(0xfff87171);
                accentDeep            = juce::Colour(0xffbe123c);
                cyanAccent            = juce::Colour(0xff5ce1b8); // Radar Seafoam Mint
                cyanBright            = juce::Colour(0xff86efac);
                cyanDeep              = juce::Colour(0xff10b981);
                mintAccent            = juce::Colour(0xffe5d5b5); // Warm Bone Ivory
                glow                  = juce::Colour(0xff5ce1b8);
                textPrimary           = juce::Colour(0xffe8e6e1);
                textSecondary         = juce::Colour(0xff7c8ba1);
                knobOutline           = juce::Colour(0xff262c3a);
                shadowColour          = juce::Colour(0x88000000);
                graphBackground       = juce::Colour(0xff0b0d12);
                graphOutline          = juce::Colour(0xff262c3a);
                graphGridLine         = juce::Colour(0xff161b24);
                graphGridLineBright   = juce::Colour(0xff293142);
                vuArc                 = juce::Colour(0xff5ce1b8);
                pitchTraceBackground  = juce::Colour(0xff0b0d12);
                pitchTraceUncorrected = juce::Colour(0xff64748b);
                pitchTraceHeroCore    = juce::Colour(0xffffffff);
                pitchTraceHeroGold    = juce::Colour(0xff5ce1b8);
                pitchTraceHeroHalo    = juce::Colour(0xff10b981);
                pianoWhiteKey         = juce::Colour(0xffe5e0d5);
                pianoWhiteKeyShadow   = juce::Colour(0xffc7c0b0);
                pianoBlackKey         = juce::Colour(0xff161922);
                pianoBlackKeyGleam    = juce::Colour(0xff2a3040);
                pianoKeyActiveGlow    = juce::Colour(0xff5ce1b8);
                pianoKeyActiveGold    = juce::Colour(0xffec4858);
                break;

            case ThemeId::TheConductor: // "The Conductor" Concert Hall Espresso, Champagne Brass & Velvet Auburn
                activeKnobStyle       = KnobStyle::OrchestralRing;
                isLightChassis        = false;
                background            = juce::Colour(0xff0f0c0a);
                backgroundDeep        = juce::Colour(0xff070504);
                panel                 = juce::Colour(0xff171310);
                panelLight            = juce::Colour(0xff221c17);
                accent                = juce::Colour(0xffc85a46); // Conductor Velvet Coat Auburn
                accentBright          = juce::Colour(0xffe07a64);
                accentDeep            = juce::Colour(0xff852e20);
                cyanAccent            = juce::Colour(0xffd4a84f); // Warm Champagne Brass
                cyanBright            = juce::Colour(0xfff5deb3); // Music-stand tungsten cream
                cyanDeep              = juce::Colour(0xff9c7326);
                mintAccent            = juce::Colour(0xffe6be6a);
                glow                  = juce::Colour(0xfff5deb3);
                textPrimary           = juce::Colour(0xfff5ebd6);
                textSecondary         = juce::Colour(0xff9e8f78);
                knobOutline           = juce::Colour(0xff3d3328);
                shadowColour          = juce::Colour(0x88000000);
                graphBackground       = juce::Colour(0xff0a0806);
                graphOutline          = juce::Colour(0xff524230);
                graphGridLine         = juce::Colour(0xff1c1712);
                graphGridLineBright   = juce::Colour(0xff382e24);
                vuArc                 = juce::Colour(0xffd4a84f);
                pitchTraceBackground  = juce::Colour(0xff0a0806);
                pitchTraceUncorrected = juce::Colour(0xff786b59);
                pitchTraceHeroCore    = juce::Colour(0xfffff9eb);
                pitchTraceHeroGold    = juce::Colour(0xffe6be6a);
                pitchTraceHeroHalo    = juce::Colour(0xffc49335);
                pianoWhiteKey         = juce::Colour(0xfff2e6cf);
                pianoWhiteKeyShadow   = juce::Colour(0xffd4c3a3);
                pianoBlackKey         = juce::Colour(0xff1a1511);
                pianoBlackKeyGleam    = juce::Colour(0xff332922);
                pianoKeyActiveGlow    = juce::Colour(0xfff5deb3);
                pianoKeyActiveGold    = juce::Colour(0xffd4a84f);
                break;

            case ThemeId::AmpPire: // "AmpPire" AI Valve Rig — Obsidian Tolex, Valve Amber & Ruby Jewel
                activeKnobStyle       = KnobStyle::BoutiqueValve;
                isLightChassis        = false;
                background            = juce::Colour(0xff0c0d10);
                backgroundDeep        = juce::Colour(0xff060708);
                panel                 = juce::Colour(0xff15161b);
                panelLight            = juce::Colour(0xff1f2129);
                accent                = juce::Colour(0xfff0b44c); // Valve Gold / Amber
                accentBright          = juce::Colour(0xffffd07b);
                accentDeep            = juce::Colour(0xffb57c1e);
                cyanAccent            = juce::Colour(0xffff5247); // Ruby Power Jewel
                cyanBright            = juce::Colour(0xffffb84d);
                cyanDeep              = juce::Colour(0xffc92a2a);
                mintAccent            = juce::Colour(0xff2ecc71); // AI Local Emerald
                glow                  = juce::Colour(0xfff0b44c);
                textPrimary           = juce::Colour(0xfff2f2f5);
                textSecondary         = juce::Colour(0xff8a8d98);
                knobOutline           = juce::Colour(0xff2c2f3a);
                shadowColour          = juce::Colour(0x88000000);
                graphBackground       = juce::Colour(0xff090a0c);
                graphOutline          = juce::Colour(0xff2e313d);
                graphGridLine         = juce::Colour(0xff171920);
                graphGridLineBright   = juce::Colour(0xff2c303d);
                vuArc                 = juce::Colour(0xfff0b44c);
                pitchTraceBackground  = juce::Colour(0xff090a0c);
                pitchTraceUncorrected = juce::Colour(0xff686c78);
                pitchTraceHeroCore    = juce::Colour(0xfffff8eb);
                pitchTraceHeroGold    = juce::Colour(0xfff0b44c);
                pitchTraceHeroHalo    = juce::Colour(0xffff6b35);
                pianoWhiteKey         = juce::Colour(0xffe6e8ee);
                pianoWhiteKeyShadow   = juce::Colour(0xffc5c9d4);
                pianoBlackKey         = juce::Colour(0xff17181d);
                pianoBlackKeyGleam    = juce::Colour(0xff2d303b);
                pianoKeyActiveGlow    = juce::Colour(0xff2ecc71);
                pianoKeyActiveGold    = juce::Colour(0xfff0b44c);
                break;

            case ThemeId::NovaStudio:
            default:
                activeKnobStyle       = KnobStyle::StudioModern;
                isLightChassis        = false;
                background            = juce::Colour(0xff0f1116);
                backgroundDeep        = juce::Colour(0xff0a0b0f);
                panel                 = juce::Colour(0xff161920);
                panelLight            = juce::Colour(0xff1f232c);
                accent                = juce::Colour(0xffff8a3d);
                accentBright          = juce::Colour(0xffffa766);
                accentDeep            = juce::Colour(0xffd9682a);
                cyanAccent            = juce::Colour(0xff38bdf8);
                cyanBright            = juce::Colour(0xff7dd3fc);
                cyanDeep              = juce::Colour(0xff0ea5e9);
                mintAccent            = juce::Colour(0xff2dd4bf);
                glow                  = juce::Colour(0xff38bdf8);
                textPrimary           = juce::Colour(0xffe8eaef);
                textSecondary         = juce::Colour(0xff8b92a1);
                knobOutline           = juce::Colour(0xff2a2f3a);
                shadowColour          = juce::Colour(0x66000000);
                graphBackground       = juce::Colour(0xff0b0d11);
                graphOutline          = juce::Colour(0xff22262f);
                graphGridLine         = juce::Colour(0xff171a21);
                graphGridLineBright   = juce::Colour(0xff252a34);
                vuArc                 = juce::Colour(0xff38bdf8);
                pitchTraceBackground  = juce::Colour(0xff0b0d11);
                pitchTraceUncorrected = juce::Colour(0xff6b7383);
                pitchTraceHeroCore    = juce::Colour(0xffffffff);
                pitchTraceHeroGold    = juce::Colour(0xff38bdf8);
                pitchTraceHeroHalo    = juce::Colour(0xff0ea5e9);
                pianoWhiteKey         = juce::Colour(0xffd6dae2);
                pianoWhiteKeyShadow   = juce::Colour(0xffb4bac6);
                pianoBlackKey         = juce::Colour(0xff1a1d24);
                pianoBlackKeyGleam    = juce::Colour(0xff2a2f3a);
                pianoKeyActiveGlow    = juce::Colour(0xff2dd4bf);
                pianoKeyActiveGold    = juce::Colour(0xffff8a3d);
                break;
        }
    }

    /** 1 px separator / card border that works on every theme. */
    inline juce::Colour hairline() noexcept
    {
        return textPrimary.withAlpha(isLightChassis ? 0.12f : 0.07f);
    }

    inline void applyTheme(int index) noexcept
    {
        applyTheme(static_cast<ThemeId>(juce::jlimit(0, 4, index)));
    }
}