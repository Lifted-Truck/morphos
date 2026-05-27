#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "physics/PhysicsState.h"

// ─────────────────────────────────────────────────────────────────────────────
// MorphosEditor — Manifold canvas + parameter panel
//
// Phase 1: draws field objects (circles), active Morphons (dots), and
//          per-Morphon trails (fading line history). Polls physics state at 30 Hz.
// Phase 3: adds TimbralAnchor rendering, drag-and-drop for all canvas objects,
//          and a right-side parameter panel with per-selection sliders.
//
// Phase 3+ (OpenGL): canvas becomes an OpenGL surface for GPU-accelerated rendering.
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

    // ── Layout ────────────────────────────────────────────────────────────────
    static constexpr int PANEL_WIDTH = 220;    // Right-side parameter panel (px)

    juce::Rectangle<int> getCanvasBounds() const;
    juce::Rectangle<int> getPanelBounds()  const;

    // Coordinate transforms: Manifold [0,1]×[0,1] ↔ canvas pixel space
    juce::Point<float> manifoldToCanvas(float mx, float my,
                                        juce::Rectangle<int> canvas) const;
    juce::Point<float> canvasToManifold(juce::Point<float> screenPt,
                                        juce::Rectangle<int> canvas) const;

    // ── Draw calls (called from paint()) ──────────────────────────────────────
    void drawGrid           (juce::Graphics&, juce::Rectangle<int> canvas) const;
    void drawFieldObjects   (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawEmitters       (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawTimbralAnchors (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawTrails         (juce::Graphics&, juce::Rectangle<int> canvas) const;
    void drawMorphons       (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawStatusBar      (juce::Graphics&, const PhysicsStateSnapshot&) const;
    void drawPanelBackground(juce::Graphics&) const;

    // ── Mouse interaction (drag-and-drop) ─────────────────────────────────────
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

    // ── Selection / drag state ─────────────────────────────────────────────────
    enum class ObjectKind { None, FieldObject, Emitter, TimbralAnchor };

    struct Selection
    {
        ObjectKind kind  = ObjectKind::None;
        int        index = -1;
        bool valid() const noexcept { return kind != ObjectKind::None && index >= 0; }
    };

    struct DragState
    {
        bool  active   = false;
        float pendingX = 0.5f;  // object's dragged position in manifold coords [0,1]
        float pendingY = 0.5f;
    };

    Selection selection_;
    DragState drag_;

    // Returns the top-priority object under canvasPt, or None if nothing hit.
    // Priority: TimbralAnchors > Emitters > FieldObjects.
    Selection hitTest(juce::Point<float> canvasPt,
                      const PhysicsStateSnapshot& state,
                      juce::Rectangle<int> canvas) const;

    // ── Parameter panel ────────────────────────────────────────────────────────
    void setupSliders();
    void layoutPanel  (juce::Rectangle<int> panelArea);  // Set component bounds
    void updatePanel  ();                                 // Refresh values + visibility

    // Helper: send a single edit command to the physics thread
    void sendEdit(ManifoldEdit::Type type, int index, float x, float y = 0.0f);

    // Guard: prevents slider callbacks from firing while we programmatically
    // set slider values during updatePanel().
    bool ignoreSliderCallbacks_ = false;

    // ── Panel components ───────────────────────────────────────────────────────
    juce::Label lblPanelHeader_;   // "No Selection" / "Anchor 0" / "Attractor 2" …

    // Anchor section (visible when a TimbralAnchor is selected)
    juce::Label  lblBrightness_,    lblInharmonicity_;
    juce::Slider sldBrightness_,    sldInharmonicity_;

    // Field object section (visible when a FieldObject is selected)
    juce::Label  lblFOStrength_,    lblFORadius_,    lblFOChirality_;
    juce::Slider sldFOStrength_,    sldFORadius_,    sldFOChirality_;

    // Emitter section (visible when an Emitter is selected)
    juce::Label  lblEmitAngle_,     lblEmitSpeed_;
    juce::Label  lblEmitAttack_,    lblEmitDecay_,   lblEmitSustain_,  lblEmitRelease_;
    juce::Slider sldEmitAngle_,     sldEmitSpeed_;
    juce::Slider sldEmitAttack_,    sldEmitDecay_,   sldEmitSustain_,  sldEmitRelease_;

    // ── Per-Morphon trail buffer ───────────────────────────────────────────────
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

        std::pair<float, float> operator[](int i) const
        {
            return pts[(head + i) % MAX_TRAIL_LENGTH];
        }
    };

    std::array<Trail, MAX_MORPHONS> trails_;

    // ── Members ───────────────────────────────────────────────────────────────
    MorphosProcessor& processor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphosEditor)
};
