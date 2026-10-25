#pragma once
#include <juce_dsp/juce_dsp.h>

/**
    Click-free, latency-matched bypass for one module of the strip.

    - The dry signal always runs through a delay equal to the module's latency,
      so bypassed audio stays time-aligned with the host's latency compensation.
    - Switching crossfades between the module output and that delayed dry
      signal over 15 ms.
    - Once fully bypassed the module is reset and no longer processed (no CPU).
      When it comes back it is processed but kept silent for its latency, so its
      buffers refill with current audio before it fades in.

    Real-time safe: all memory is allocated in prepare().
*/
class ModuleBypass
{
public:
    /** startBypassed: the module's bypass state at load, so a module that starts
        switched off never runs (no fade-out at startup). */
    /** maxLatencySamples: the largest latency setLatency() may switch to later
        (e.g. Tune with SMOOTH on); defaults to the current one. */
    void prepare(const juce::dsp::ProcessSpec& spec, int moduleLatencySamples, bool startBypassed = false,
                 int maxLatencySamples = 0)
    {
        latency = juce::jmax(0, moduleLatencySamples);
        maxLatency = juce::jmax(latency, maxLatencySamples);
        fadeStep = static_cast<float> (1.0 / juce::jmax(1.0, spec.sampleRate * fadeSeconds));

        dry.setSize(static_cast<int> (spec.numChannels), static_cast<int> (spec.maximumBlockSize));
        delay.prepare(spec);
        delay.setMaximumDelayInSamples(juce::jmax(1, maxLatency));
        delay.setDelay(static_cast<float> (latency));

        delay.reset();
        wetGain = startBypassed ? 0.0f : 1.0f;
        warmupLeft = 0;
        sleeping = startBypassed;
    }

    /** processModule(context) runs the module; resetModule() clears its state. */
    template <typename ProcessFn, typename ResetFn>
    void process(const juce::dsp::ProcessContextReplacing<float>& context, bool bypassed,
                 ProcessFn&& processModule, ResetFn&& resetModule) noexcept
    {
        auto& block = context.getOutputBlock();
        const size_t numCh = juce::jmin(block.getNumChannels(), static_cast<size_t> (dry.getNumChannels()));
        const size_t n = block.getNumSamples();
        if (n == 0 || numCh == 0)
            return;

        // latency-matched dry copy
        for (size_t ch = 0; ch < numCh; ++ch)
            dry.copyFrom(static_cast<int> (ch), 0, block.getChannelPointer(ch), static_cast<int> (n));

        juce::dsp::AudioBlock<float> dryBlock(dry.getArrayOfWritePointers(), numCh, n);
        if (latency > 0)
        {
            juce::dsp::ProcessContextReplacing<float> dryContext(dryBlock);
            delay.process(dryContext);
        }

        if (sleeping)
        {
            if (bypassed)
            {
                block.copyFrom(dryBlock);
                return;
            }

            sleeping = false;
            warmupLeft = latency; // module was reset on the way to sleep
        }

        processModule(context);

        const float target = bypassed ? 0.0f : 1.0f;
        const size_t silentSamples = static_cast<size_t> (juce::jmin(warmupLeft, static_cast<int> (n)));
        warmupLeft -= static_cast<int> (silentSamples);

        if (wetGain == target && silentSamples == 0 && target == 1.0f)
            return; // steady state, fully on: module output as is

        for (size_t i = 0; i < n; ++i)
        {
            if (i >= silentSamples)
                wetGain = target > wetGain ? juce::jmin(target, wetGain + fadeStep)
                                           : juce::jmax(target, wetGain - fadeStep);

            const float g = i < silentSamples ? 0.0f : wetGain;
            for (size_t ch = 0; ch < numCh; ++ch)
            {
                auto* out = block.getChannelPointer(ch);
                const float d = dryBlock.getSample(static_cast<int> (ch), static_cast<int> (i));
                out[i] = d + g * (out[i] - d);
            }
        }

        if (bypassed && wetGain <= 0.0f)
        {
            resetModule();
            sleeping = true;
            warmupLeft = 0;
        }
    }

    /** Switches the dry-path delay to a new module latency (up to the
        maxLatencySamples given to prepare()). Allocation-free; call at a
        block boundary, from the audio thread. */
    void setLatency(int newLatencySamples) noexcept
    {
        latency = juce::jlimit(0, maxLatency, newLatencySamples);
        delay.setDelay(static_cast<float> (latency));
        delay.reset();
    }

private:
    static constexpr double fadeSeconds = 0.015;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> delay { 1 };
    juce::AudioBuffer<float> dry;

    int   latency = 0;
    int   maxLatency = 0;
    float fadeStep = 1.0f;
    float wetGain = 1.0f;
    int   warmupLeft = 0;
    bool  sleeping = false;
};

