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
    static constexpr int PANEL_WIDTH = 260;    // Right-side parameter panel (px)

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

    // ── Draw calls (additional) ───────────────────────────────────────────────
    void drawEffectZones    (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawFluxGates      (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawPathObjects    (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;
    void drawTrajectoryPaths(juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;

    // ── Mouse interaction (drag-and-drop + placement) ─────────────────────────
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

    // ── Keyboard ─────────────────────────────────────────────────────────────
    bool keyPressed(const juce::KeyPress&) override;

    // ── Selection / drag state ─────────────────────────────────────────────────
    enum class ObjectKind { None, FieldObject, Emitter, TimbralAnchor, EffectZone, FluxGate, PathObject, TrajectoryPath };

    // ── Placement mode — arm a type, then click canvas to place ───────────────
    enum class SpawnKind { None, Attractor, Repeller, Vortex, Emitter, TimbralAnchor, EffectZone, FluxGate, PathObject, TrajectoryPath };

    SpawnKind pendingSpawn_ = SpawnKind::None;

    // Returns the colour associated with the currently armed spawn type (for canvas indicator).
    juce::Colour pendingSpawnColour() const noexcept;

    // Clears placement mode and resets all spawn button toggle states.
    void clearPlacementMode();

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
        // Manifold-space offset from cursor to object centre at mouseDown.
        // Preserved through the drag so the object tracks the cursor relative
        // to where it was first grabbed, instead of snapping its centre under
        // the cursor on the first drag event.
        float offsetX  = 0.0f;
        float offsetY  = 0.0f;
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

    // Spawn row — always visible; add new objects at canvas centre
    juce::TextButton btnAddAtt_  { "+Att"  };
    juce::TextButton btnAddRep_  { "+Rep"  };
    juce::TextButton btnAddVor_  { "+Vor"  };
    juce::TextButton btnAddEmit_ { "+Emit" };
    juce::TextButton btnAddAnch_ { "+Anch" };
    juce::TextButton btnAddZone_ { "+Zon"  };
    juce::TextButton btnAddFlux_ { "+Gate" };
    juce::TextButton btnAddPath_ { "+Rail" };
    juce::TextButton btnAddTraj_ { "+Traj" };

    // Panel header: object type+index label, plus Remove button on the right
    juce::Label      lblPanelHeader_;
    juce::TextButton btnRemove_  { "×" };

    // Anchor section (visible when a TimbralAnchor is selected)
    juce::Label  lblBrightness_,    lblInharmonicity_;
    juce::Slider sldBrightness_,    sldInharmonicity_;

    // Field object section (visible when a FieldObject is selected)
    juce::Label  lblFOStrength_,    lblFORadius_,    lblFOChirality_;
    juce::Slider sldFOStrength_,    sldFORadius_,    sldFOChirality_;

    // Emitter section (visible when an Emitter is selected)
    juce::Label  lblKeyLow_,        lblKeyHigh_;
    juce::Slider sldKeyLow_,        sldKeyHigh_;
    juce::Label  lblTransposeOct_,  lblTransposeSemi_, lblTransposeCents_;
    juce::Slider sldTransposeOct_,  sldTransposeSemi_, sldTransposeCents_;
    juce::Label  lblEmitPan_,       lblEmitMass_;
    juce::Slider sldEmitPan_,       sldEmitMass_;

    // Per-Emitter polyphony row (replaces the old global Voices row)
    juce::Label      lblEmitPolyMode_;
    juce::TextButton btnEmitPoly_   { "Poly"   };
    juce::TextButton btnEmitMono_   { "Mono"   };
    juce::TextButton btnEmitLegato_ { "Legato" };
    juce::TextButton btnEmitSlur_   { "Slur"   };

    // Terminus sub-section (emitter panel)
    juce::TextButton btnTerminusEnabled_ { "Terminus" };
    juce::Label      lblTerminusStrength_, lblTerminusRadius_;
    juce::Slider     sldTerminusStrength_, sldTerminusRadius_;
    juce::Label  lblEmitAngle_,     lblEmitSpeed_;
    juce::Label  lblEmitAttack_,    lblEmitDecay_,   lblEmitSustain_,  lblEmitRelease_;
    juce::Slider sldEmitAngle_,     sldEmitSpeed_;
    juce::Slider sldEmitAttack_,    sldEmitDecay_,   sldEmitSustain_,  sldEmitRelease_;

    // Global topology row — always visible; sets Manifold boundary for all Morphons
    juce::Label      lblBoundary_;
    juce::TextButton btnBoundWrap_      { "Wrap"      };
    juce::TextButton btnBoundReflect_   { "Reflect"   };
    juce::TextButton btnBoundTerminate_ { "Terminate" };
    juce::TextButton btnBoundKlein_     { "Klein"     };

    // Glide (portamento) — always visible; applies in Legato + Slur modes
    juce::Label  lblGlideTime_;
    juce::Slider sldGlideTime_;

    // Effect zone section (visible when an EffectZone is selected)
    juce::Label  lblZoneRadius_,     lblZoneDepth_;
    juce::Slider sldZoneRadius_,     sldZoneDepth_;
    juce::Label  lblZoneTarget_,     lblZoneFalloff_;
    juce::TextButton btnZoneTimbreX_ { "TmbrX" };
    juce::TextButton btnZoneTimbreY_ { "TmbrY" };
    juce::TextButton btnZoneAmp_     { "Amp"   };
    juce::TextButton btnZonePan_     { "Pan"   };
    juce::TextButton btnZonePitch_   { "Pitch" };
    juce::TextButton btnZoneFalloffLinear_   { "Linear"   };
    juce::TextButton btnZoneFalloffGaussian_ { "Gauss"    };

    // Flux gate section (visible when a FluxGate is selected)
    juce::Label  lblGateLength_, lblGateAngle_;
    juce::Slider sldGateLength_, sldGateAngle_;

    // Path object section (visible when a PathObject is selected)
    juce::Label  lblPathRadius_, lblPathSnap_;
    juce::Slider sldPathRadius_, sldPathSnap_;

    // Trajectory path section (visible when a TrajectoryPath is selected)
    juce::Label  lblTrajRadius_, lblTrajSpeed_;
    juce::Slider sldTrajRadius_, sldTrajSpeed_;

    // Emitter section addition: trajectory path attachment (in Emitter panel)
    juce::Label  lblEmitTraj_;
    juce::Slider sldEmitTraj_;

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
