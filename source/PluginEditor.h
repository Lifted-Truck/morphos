#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

// ─────────────────────────────────────────────────────────────────────────────
// MorphosEditor — UI thread component (VST3 plugin window)
//
// Phase 0: placeholder window. Shows plugin name, tick counter from physics,
// and a status label confirming the threading model is running.
//
// Phase 3+: replace with the full Manifold canvas + parameter panels.
//
// The editor polls getPhysicsStateForUI() at ~30 Hz via a juce::Timer.
// This is safe: the processor copies the snapshot in processBlock() and the
// editor reads it on the UI thread. Both sides treat the copy as approximate.
// ─────────────────────────────────────────────────────────────────────────────

class MorphosEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit MorphosEditor(MorphosProcessor&);
    ~MorphosEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    MorphosProcessor& processor_;

    juce::Label titleLabel_;
    juce::Label statusLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphosEditor)
};
