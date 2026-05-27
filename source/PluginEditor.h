#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "physics/PhysicsState.h"

// ─────────────────────────────────────────────────────────────────────────────
// MorphosEditor — Manifold canvas + status bar
//
// Phase 1: draws field objects (circles), active Morphons (dots), and
// per-Morphon trails (fading line history). Polls physics state at 30 Hz.
//
// Phase 3+: canvas becomes OpenGL surface; parameter panels added on the right.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_TRAIL_LENGTH = 180;   // Points; 6 s at 30 Hz

class MorphosEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit MorphosEditor(MorphosProcessor&);
    ~MorphosEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // ── Timer ─────────────────────────────────────────────────────────────────
    void timerCallback() override;

    // ── Canvas helpers ────────────────────────────────────────────────────────

    // The rectangle used for the Manifold canvas (inset from window edges).
    juce::Rectangle<int> getCanvasBounds() const;

    // Convert Manifold [0,1]×[0,1] → canvas pixel position.
    juce::Point<float> manifoldToCanvas(float mx, float my,
                                        juce::Rectangle<int> canvas) const;

    // Draw calls (called from paint())
    void drawGrid      (juce::Graphics&, juce::Rectangle<int> canvas) const;
    void drawFieldObjects(juce::Graphics&, const PhysicsStateSnapshot&,
                          juce::Rectangle<int> canvas) const;
    void drawTrails    (juce::Graphics&, juce::Rectangle<int> canvas) const;
    void drawMorphons  (juce::Graphics&, const PhysicsStateSnapshot&,
                        juce::Rectangle<int> canvas) const;
    void drawStatusBar (juce::Graphics&, const PhysicsStateSnapshot&) const;

    // ── Members ───────────────────────────────────────────────────────────────
    MorphosProcessor& processor_;

    // Per-Morphon circular trail buffer storing Manifold coordinates [0,1]×[0,1].
    // Indexed by Morphon slot, not MIDI note, so it stays valid across retrigs.
    struct Trail
    {
        std::array<std::pair<float, float>, MAX_TRAIL_LENGTH> pts{};
        int  head  = 0;
        int  count = 0;

        void push(float x, float y)
        {
            const int idx = (head + count) % MAX_TRAIL_LENGTH;
            if (count < MAX_TRAIL_LENGTH)
            {
                pts[idx] = { x, y };
                ++count;
            }
            else
            {
                pts[head] = { x, y };
                head = (head + 1) % MAX_TRAIL_LENGTH;
            }
        }

        void clear() { head = 0; count = 0; }

        // Access in chronological order (oldest first)
        std::pair<float,float> operator[](int i) const
        {
            return pts[(head + i) % MAX_TRAIL_LENGTH];
        }
    };

    std::array<Trail, MAX_MORPHONS> trails_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphosEditor)
};
