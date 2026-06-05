#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

// ─────────────────────────────────────────────────────────────────────────────
// SampleSource — one loaded audio file, summed to mono, used as a granular
// source. The buffer is immutable once loaded, so it can be shared read-only
// with the audio thread with no locking.
//
// Lifetime is owned by the message thread (the processor keeps the unique_ptr);
// the audio thread sees it via an atomic raw pointer published in the registry.
// Slice 1: mono only (Morphons carry a single pan). Stereo grains come later.
// ─────────────────────────────────────────────────────────────────────────────
struct SampleSource
{
    int                      id = -1;
    juce::String             name;
    juce::AudioBuffer<float> mono;             // single-channel grain source
    double                   sampleRate = 44100.0;

    int          numSamples() const noexcept { return mono.getNumSamples(); }
    const float* data()       const noexcept { return mono.getReadPointer(0); }
    bool         valid()      const noexcept { return mono.getNumSamples() > 0; }
};
