#pragma once
#include "../Harmony/HarmonyEngine.h"

/** Default singer register for a harmony mode: 0 low male, 1 mid male, 2 mid female, 3 high female. */
inline int choirRegisterFor (int mode)
{
    switch (mode)
    {
        case HarmonyEngine::octaveDown: case HarmonyEngine::chordRoot:  return 0;
        case HarmonyEngine::chordBelow: case HarmonyEngine::chordBelow2: return 1;
        case HarmonyEngine::chordAbove2: case HarmonyEngine::octaveUp: case HarmonyEngine::maj9Colour: return 3;
        default: return 2;
    }
}
