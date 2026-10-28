#pragma once

#include "PolyAnalyzer.h"
#include <juce_audio_basics/juce_audio_basics.h>

/**
    Offline polyphonic note renderer (Melodyne-style clip editing).

    For every edited note:
      1. track its partials (frequency + amplitude) frame by frame along the analysed pitch curve
      2. where a partial is shared with a concurrent note, split it in proportion to each note's
         smooth spectral envelope
      3. remove the note's share from the mix with a spectral mask (STFT, perfect-reconstruction
         sqrt-Hann, 75% overlap)
      4. resynthesise its partials at the new pitch with an oscillator bank; each partial's
         amplitude is read from the note's own spectral envelope at the *new* frequency, which
         keeps the formants of a voice (or the body of a guitar) where they were
      5. blend back into the untouched original with short crossfades - regions without edits
         are bit-identical to the input.
*/
namespace Poly
{
    struct NoteEdit
    {
        int   noteId           = 0;
        float semitones        = 0.0f; ///< pitch change
        float gainDb           = 0.0f;
        float formantSemitones = 0.0f; ///< 0 = keep the original formants
    };

    void render (const juce::AudioBuffer<float>& clip, double sampleRate, const Analysis& analysis,
                 const std::vector<NoteEdit>& edits, juce::AudioBuffer<float>& out,
                 const std::function<bool()>& shouldCancel = {},
                 const std::function<void (float)>& progress = {});
}
