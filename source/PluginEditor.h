#pragma once
#include <limits>
#include <vector>

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
    void drawTangentPaths   (juce::Graphics&, const PhysicsStateSnapshot&,
                             juce::Rectangle<int> canvas) const;

    // ── Mouse interaction (drag-and-drop + placement) ─────────────────────────
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp  (const juce::MouseEvent&) override;

    // ── Keyboard ─────────────────────────────────────────────────────────────
    bool keyPressed(const juce::KeyPress&) override;

    // ── Selection / drag state ─────────────────────────────────────────────────
    enum class ObjectKind { None, FieldObject, Emitter, TimbralAnchor, EffectZone, FluxGate, PathObject, TrajectoryPath, TangentPath };

    // ── Placement mode — arm a type, then click canvas to place ───────────────
    enum class SpawnKind { None, Attractor, Repeller, Vortex, Emitter, TimbralAnchor, EffectZone, FluxGate, PathObject, TrajectoryPath, TangentPath };

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

    // ── Multi-select & marquee ──────────────────────────────────────────────────
    // multiSelection_ is populated when the user marquee-drags to select more
    // than one object, OR is updated to mirror selection_ for single selection
    // (size 1 == single selection; size > 1 == multi). When size > 1 the
    // parameter panel collapses to "Multi (N)" and clicking-and-dragging any
    // selected object translates the entire group.
    std::vector<Selection> multiSelection_;

    struct GroupDragEntry
    {
        ObjectKind kind;
        int        index;
        float      startX, startY;   // Manifold position at mouseDown
        float      pendingX, pendingY;  // Pending position during drag
    };
    std::vector<GroupDragEntry> groupDrag_;
    juce::Point<float>          groupDragCursorStart_{ 0.5f, 0.5f };  // Manifold coords

    bool                marqueeActive_   = false;
    juce::Point<float>  marqueeStartPx_  { 0.0f, 0.0f };   // Canvas (pixel) coords
    juce::Point<float>  marqueeCurrentPx_{ 0.0f, 0.0f };

    // ── Multi-select helpers ────────────────────────────────────────────────────
    bool isObjectSelected(ObjectKind k, int i) const noexcept;
    bool isObjectDragging(ObjectKind k, int i, float& outX, float& outY) const noexcept;
    void clearMultiSelection() noexcept;
    void buildMultiSelectionFromMarquee(const PhysicsStateSnapshot& state,
                                         juce::Rectangle<int> canvas);
    void beginGroupDrag(const PhysicsStateSnapshot& state, juce::Point<float> clickMfd);

    // ── Static (kind, index) → snapshot / edit-type helpers ─────────────────────
    static bool readObjectPos(const PhysicsStateSnapshot& state,
                              ObjectKind kind, int index,
                              float& outX, float& outY) noexcept;
    static ManifoldEdit::Type moveEditTypeFor  (ObjectKind kind) noexcept;
    static ManifoldEdit::Type removeEditTypeFor(ObjectKind kind) noexcept;

    // Returns the top-priority object under canvasPt, or None if nothing hit.
    // Priority: TimbralAnchors > Emitters > FieldObjects.
    Selection hitTest(juce::Point<float> canvasPt,
                      const PhysicsStateSnapshot& state,
                      juce::Rectangle<int> canvas) const;

    // ── Parameter panel ────────────────────────────────────────────────────────
    void setupSliders();
    void setupModMatrix();                                // Build mod-tab widgets + dropdowns
    void installPanelViewport();                          // Re-parent per-section comps into the viewport
    void layoutPanel  (juce::Rectangle<int> panelArea);  // Set component bounds
    void layoutModTab (int contentW);                     // Layout for mod-matrix rows; returns end y
    int  layoutModTabContent(int contentW);               // Helper returning total height
    void updatePanel  ();                                 // Refresh values + visibility
    void updateModTab ();                                 // Refresh mod row values + visibility
    void populateModSourceCombo(juce::ComboBox& cb, const PhysicsStateSnapshot& state) const;
    void populateModDestCombo  (juce::ComboBox& cb, const PhysicsStateSnapshot& state) const;
    void refreshModDropdownsIfNeeded(const PhysicsStateSnapshot& state);

    // The physics engine bumps state.configVersion any time an object is
    // added or removed; the editor compares it against this saved value to
    // decide whether to rebuild the mod-matrix dropdowns. Sentinel of
    // UINT64_MAX forces a build on the first refresh after construction.
    uint64_t lastSeenConfigVersion_ = std::numeric_limits<uint64_t>::max();

    // ── Panel tab mode ──────────────────────────────────────────────────────────
    // Inspector = per-selection sliders (the default panel content).
    // Mod       = list of mod-matrix connections with source/dest dropdowns.
    enum class PanelMode { Inspector, Mod };
    PanelMode panelMode_ = PanelMode::Inspector;
    juce::TextButton btnTabInspector_ { "Inspector" };
    juce::TextButton btnTabMod_       { "Mod"       };

    // Mod-matrix UI — one row per connection slot, hidden when slot is inactive.
    // ComboBox itemIds encode (type * 256 + index) — see encodeSrcChoice /
    // decodeSrcChoice helpers in PluginEditor.cpp for the actual packing.
    std::array<juce::ComboBox,   MAX_MOD_CONNECTIONS> modSrcCombos_;
    std::array<juce::ComboBox,   MAX_MOD_CONNECTIONS> modDstCombos_;
    std::array<juce::Slider,     MAX_MOD_CONNECTIONS> modDepthSliders_;
    std::array<juce::Label,      MAX_MOD_CONNECTIONS> modDepthLabels_;
    std::array<juce::TextButton, MAX_MOD_CONNECTIONS> modRemoveBtns_;
    juce::Label      lblModHeader_;
    juce::TextButton btnModAdd_       { "+ Add Mod" };
    int              lastModActiveCount_ = -1;   // -1 forces a relayout on first tick

    // ── Scroll viewport for per-selection sections ────────────────────────────
    // The always-visible top (spawn buttons, topology, glide, header) remains
    // a direct child of `this`. Per-selection slider/button sections live
    // inside panelContent_, which is the viewed component of panelViewport_;
    // when a section (e.g. Emitter) is taller than the panel area, the
    // viewport scrolls vertically. Content height is sized to the currently
    // selected section so empty scroll space doesn't appear.
    juce::Viewport  panelViewport_;
    juce::Component panelContent_;       // Inspector tab — per-selection sliders.
    juce::Component panelContentMod_;    // Mod tab — connection rows.
    // Tracks the section kind the viewport content was last sized for, so
    // updatePanel() only re-runs layoutPanel when the selection's section
    // actually changes (avoids relayouting on every 30Hz timer tick).
    ObjectKind      lastPanelLayoutKind_ = ObjectKind::None;

    // Helper: send a single edit command to the physics thread
    void sendEdit(ManifoldEdit::Type type, int index, float x, float y = 0.0f);
    // If any active mod connection targets (destType, dstIndex), forward the
    // edit to that connection's `base` instead of writing the param directly —
    // so a slider for a modulated parameter still does something useful
    // (moves the modulation centre) rather than being overwritten next tick.
    void sendParamOrModBase(ManifoldEdit::Type plainSet, int dstIndex,
                             float value, ModDestType destType);

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
    juce::TextButton btnAddFlow_ { "+Flow" };

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
    juce::Label  lblFriction_;
    juce::Slider sldFriction_;

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
    juce::Label  lblGateLength_, lblGateAngle_, lblGateRadius_, lblGateShape_;
    juce::Slider sldGateLength_, sldGateAngle_, sldGateRadius_;
    juce::TextButton btnGateShapeLine_   { "Line"   };
    juce::TextButton btnGateShapeCircle_ { "Circle" };

    // Path object section (visible when a PathObject is selected)
    juce::Label  lblPathRadius_, lblPathSnap_, lblPathEscape_;
    juce::Slider sldPathRadius_, sldPathSnap_, sldPathEscape_;

    // Trajectory path section (visible when a TrajectoryPath is selected)
    juce::Label      lblTrajRadius_, lblTrajSpeed_, lblTrajPos_, lblTrajMode_;
    juce::Label      lblTrajShape_, lblTrajLength_, lblTrajAngle_, lblTrajCurve_;
    juce::Slider     sldTrajRadius_, sldTrajSpeed_, sldTrajPos_;
    juce::Slider     sldTrajLength_, sldTrajAngle_;
    juce::TextButton btnTrajModeAuto_   { "Auto"   };
    juce::TextButton btnTrajModeManual_ { "Manual" };
    juce::TextButton btnTrajShapeCircle_   { "Circle"     };
    juce::TextButton btnTrajShapeLine_     { "Line"       };
    juce::TextButton btnTrajCurveTriangle_ { "Triangular" };
    juce::TextButton btnTrajCurveSine_     { "Sinusoidal" };

    // Tangent-force ("Flow") path section (visible when a TangentPath is selected)
    juce::Label  lblFlowRadius_, lblFlowWidth_, lblFlowStrength_, lblFlowChirality_;
    juce::Slider sldFlowRadius_, sldFlowWidth_, sldFlowStrength_, sldFlowChirality_;

    // Emitter section addition: trajectory path attachment (in Emitter panel)
    juce::Label  lblTrajAttach_;
    juce::Slider sldTrajAttach_;

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
