#include "PluginEditor.h"

// ─────────────────────────────────────────────────────────────────────────────
// UI WISHLIST (logged here for Phase 10):
//
//  • Piano keyboard strip along the bottom of the window.
//    - Displays currently active MIDI notes (keys light up).
//    - Clickable: lets you trigger notes from inside the VST without a controller.
//    - Useful both for live testing and for preset auditioning in Ableton.
//    JUCE provides MidiKeyboardComponent for a quick baseline; may want a custom
//    skinned version to match the Morphos visual language.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Palette — colours matching the design spec object taxonomy
// ─────────────────────────────────────────────────────────────────────────────
namespace Colour
{
    static const juce::Colour Background  { 0xFF1A1A18 };
    static const juce::Colour PanelBg     { 0xFF1F1F1D };   // Slightly lighter — panel area
    static const juce::Colour GridLine    { 0xFF252522 };
    static const juce::Colour Divider     { 0xFF2E2E2B };   // Canvas / panel border
    static const juce::Colour Attractor   { 0xFF378ADD };
    static const juce::Colour Repeller    { 0xFFE24B4A };
    static const juce::Colour Vortex      { 0xFF7F77DD };
    static const juce::Colour Emitter     { 0xFFD9A63A };   // Amber — spawn / generation
    static const juce::Colour Anchor      { 0xFF3DC9B0 };   // Teal — Timbral Anchor
    static const juce::Colour MorphonDot  { 0xFFE8E4DC };
    static const juce::Colour TrailBase   { 0xFFB0AB9E };
    static const juce::Colour StatusText  { 0xFF888880 };
    static const juce::Colour SelectRing  { 0xFFFFFFFF };   // Selection highlight ring

    // Effect zone colours — keyed by ZoneTarget
    static const juce::Colour ZoneTimbreX  { 0xFF5B6ECA };  // Blue/indigo — spectral rolloff
    static const juce::Colour ZoneTimbreY  { 0xFF4CAF72 };  // Green       — inharmonicity
    static const juce::Colour ZoneAmp      { 0xFFD4A84B };  // Gold        — amplitude
    static const juce::Colour ZonePan      { 0xFFCA5B8A };  // Pink        — stereo pan
    static const juce::Colour ZonePitch    { 0xFF9B5BCA };  // Purple      — pitch shift

    // Flux Gate — crossing-triggered envelope re-trigger. Distinct hue from
    // Repeller red and Emitter amber; reads as a "tripwire" cue.
    static const juce::Colour FluxGate     { 0xFF38CFE6 };  // Cyan

    // Path Object — rail-constraint curve. Sage / olive green reads as
    // "track / rail" without colliding with Anchor teal or Zone greens.
    static const juce::Colour PathObject   { 0xFF8DBC72 };  // Sage green

    // Trajectory Path — position-driver curve. Peach / apricot reads as
    // "warm / motion" without colliding with Emitter amber or Repeller red.
    static const juce::Colour TrajPath     { 0xFFE89A5C };  // Peach / apricot

    // Tangent-force ("Flow") path — soft stream that carries Morphons along a
    // curve. Periwinkle blue reads as "current / flow" without colliding with
    // FluxGate cyan or Anchor teal.
    static const juce::Colour FlowPath     { 0xFF7C9CF0 };  // Periwinkle blue
}

static juce::Colour zoneColour(ZoneTarget t) noexcept
{
    switch (t)
    {
        case ZoneTarget::TimbreX:   return Colour::ZoneTimbreX;
        case ZoneTarget::TimbreY:   return Colour::ZoneTimbreY;
        case ZoneTarget::Amplitude: return Colour::ZoneAmp;
        case ZoneTarget::Pan:       return Colour::ZonePan;
        case ZoneTarget::Pitch:     return Colour::ZonePitch;
        default:                    return Colour::ZoneTimbreX;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

MorphosEditor::MorphosEditor(MorphosProcessor& p)
    : AudioProcessorEditor(&p), processor_(p)
{
    setResizable(true, true);
    setResizeLimits(640, 400, 2400, 1600);
    setSize(processor_.getStoredEditorWidth(), processor_.getStoredEditorHeight());
    setWantsKeyboardFocus(true);

    setupSliders();
    installPanelViewport();   // Re-parent per-section components into the scrollable viewport
    updatePanel();    // Initialise panel to "No Selection" state

    startTimerHz(30);
}

MorphosEditor::~MorphosEditor()
{
    stopTimer();
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ─────────────────────────────────────────────────────────────────────────────

juce::Rectangle<int> MorphosEditor::getCanvasBounds() const
{
    // Full inner area (8 px margin, 28 px status bar, panel on right)
    auto r = getLocalBounds().reduced(8).withTrimmedBottom(28);
    r.removeFromRight(PANEL_WIDTH + 8);   // +8 = gap between canvas and panel
    return r;
}

juce::Rectangle<int> MorphosEditor::getPanelBounds() const
{
    auto r = getLocalBounds().reduced(8).withTrimmedBottom(28);
    return r.removeFromRight(PANEL_WIDTH);
}

juce::Point<float> MorphosEditor::manifoldToCanvas(float mx, float my,
                                                    juce::Rectangle<int> canvas) const
{
    return {
        canvas.getX() + mx * canvas.getWidth(),
        canvas.getY() + my * canvas.getHeight()
    };
}

juce::Point<float> MorphosEditor::canvasToManifold(juce::Point<float> screenPt,
                                                    juce::Rectangle<int> canvas) const
{
    return {
        (screenPt.x - canvas.getX()) / canvas.getWidth(),
        (screenPt.y - canvas.getY()) / canvas.getHeight()
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// resized — lay out all child components
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::resized()
{
    processor_.setStoredEditorSize(getWidth(), getHeight());
    layoutPanel(getPanelBounds());
}

void MorphosEditor::layoutPanel(juce::Rectangle<int> panel)
{
    constexpr int SPAWN_H    = 24;   // height of spawn button row
    constexpr int HEADER_H   = 24;
    constexpr int LABEL_H    = 13;
    constexpr int SLIDER_H   = 20;
    constexpr int ROW_H      = LABEL_H + SLIDER_H + 2;
    constexpr int SECTION_GAP = 6;
    constexpr int BTN_ROW_H  = 22;

    int y = panel.getY() + 4;
    int x = panel.getX();
    int w = panel.getWidth();

    // ── Spawn rows — 3 × 3 grid (9 object types) ─────────────────────────────
    // Phase 10's right-click context menu will subsume these buttons; until
    // then, a 3×3 grid keeps labels readable as more object types come online.
    {
        const int bw3 = w / 3;
        btnAddAtt_ .setBounds(x + bw3 * 0, y, bw3,         SPAWN_H);
        btnAddRep_ .setBounds(x + bw3 * 1, y, bw3,         SPAWN_H);
        btnAddVor_ .setBounds(x + bw3 * 2, y, w - bw3 * 2, SPAWN_H);
    }
    y += SPAWN_H + 2;
    {
        const int bw3 = w / 3;
        btnAddEmit_.setBounds(x + bw3 * 0, y, bw3,         SPAWN_H);
        btnAddAnch_.setBounds(x + bw3 * 1, y, bw3,         SPAWN_H);
        btnAddZone_.setBounds(x + bw3 * 2, y, w - bw3 * 2, SPAWN_H);
    }
    y += SPAWN_H + 2;
    {
        const int bw3 = w / 3;
        btnAddFlux_.setBounds(x + bw3 * 0, y, bw3,         SPAWN_H);
        btnAddPath_.setBounds(x + bw3 * 1, y, bw3,         SPAWN_H);
        btnAddTraj_.setBounds(x + bw3 * 2, y, w - bw3 * 2, SPAWN_H);
    }
    y += SPAWN_H + 2;
    {
        const int bw3 = w / 3;
        btnAddFlow_.setBounds(x + bw3 * 0, y, bw3,         SPAWN_H);
    }
    y += SPAWN_H + 4;

    // ── Topology row — global Manifold boundary; always visible ───────────────
    lblBoundary_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 4;
        btnBoundWrap_     .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnBoundReflect_  .setBounds(x + bw * 1, y, bw,         BTN_ROW_H);
        btnBoundTerminate_.setBounds(x + bw * 2, y, bw,         BTN_ROW_H);
        btnBoundKlein_    .setBounds(x + bw * 3, y, w - bw * 3, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;

    // ── Glide time — portamento rate; always visible ───────────────────────────
    lblGlideTime_.setBounds(x, y,           w, LABEL_H);
    sldGlideTime_.setBounds(x, y + LABEL_H, w, SLIDER_H);
    y += ROW_H + 4;

    // ── Header: name label + remove button (always visible, outside viewport) ─
    constexpr int REMOVE_W = 20;
    lblPanelHeader_.setBounds(x, y, w - REMOVE_W - 2, HEADER_H);
    btnRemove_     .setBounds(x + w - REMOVE_W, y, REMOVE_W, HEADER_H);
    y += HEADER_H + SECTION_GAP;

    // ── Viewport — per-selection sections live inside this scrollable area ───
    constexpr int SCROLLBAR_W = 8;
    panelViewport_.setBounds(x, y, w, panel.getBottom() - y);

    // From here on, x/y/w are LOCAL to panelContent_ (scrollable). The layoutRow
    // lambda below captures these by reference so it uses the new local coords.
    x = 0;
    w = w - SCROLLBAR_W;
    y = 0;

    auto layoutRow = [&](juce::Label& lbl, juce::Slider& sld)
    {
        lbl.setBounds(x, y,          w, LABEL_H);
        sld.setBounds(x, y + LABEL_H, w, SLIDER_H);
        y += ROW_H;
    };

    // ── Per-selection sections share the same top y ──────────────────────────
    // Only one section is visible at a time (Anchor / FieldObject / Emitter /
    // Zone / Gate / Path / Trajectory / Flow), so they all start at sectionTopY
    // and overlap in panelContent_ space; the visible-state toggles in
    // updatePanel ensure exactly one section paints. We track each section's
    // end y so panelContent_ can be sized to just the currently selected
    // section's height (no empty scroll space).
    const int sectionTopY = 0;

    // ── Anchor section ────────────────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblBrightness_,    sldBrightness_);
    layoutRow(lblInharmonicity_, sldInharmonicity_);
    const int anchorEndY = y;

    // ── Field object section ──────────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblFOStrength_,  sldFOStrength_);
    layoutRow(lblFORadius_,    sldFORadius_);
    layoutRow(lblFOChirality_, sldFOChirality_);
    const int fieldEndY = y;

    // ── Emitter section ───────────────────────────────────────────────────────
    y = sectionTopY;
    // Per-Emitter voice mode row (replaces the old global Voices row)
    lblEmitPolyMode_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 4;
        btnEmitPoly_  .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnEmitMono_  .setBounds(x + bw * 1, y, bw,         BTN_ROW_H);
        btnEmitLegato_.setBounds(x + bw * 2, y, bw,         BTN_ROW_H);
        btnEmitSlur_  .setBounds(x + bw * 3, y, w - bw * 3, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;

    layoutRow(lblKeyLow_,         sldKeyLow_);
    layoutRow(lblKeyHigh_,        sldKeyHigh_);
    layoutRow(lblTransposeOct_,   sldTransposeOct_);
    layoutRow(lblTransposeSemi_,  sldTransposeSemi_);
    layoutRow(lblTransposeCents_, sldTransposeCents_);
    layoutRow(lblEmitPan_,        sldEmitPan_);
    layoutRow(lblEmitMass_,       sldEmitMass_);
    layoutRow(lblEmitAngle_,      sldEmitAngle_);
    layoutRow(lblEmitSpeed_,   sldEmitSpeed_);
    layoutRow(lblEmitAttack_,  sldEmitAttack_);
    layoutRow(lblEmitDecay_,   sldEmitDecay_);
    layoutRow(lblEmitSustain_, sldEmitSustain_);
    layoutRow(lblEmitRelease_, sldEmitRelease_);

    // Terminus toggle button (full width)
    btnTerminusEnabled_.setBounds(x, y, w, BTN_ROW_H);
    y += BTN_ROW_H + 2;

    // Strength + radius shown only when Terminus is enabled (hidden otherwise)
    layoutRow(lblTerminusStrength_, sldTerminusStrength_);
    layoutRow(lblTerminusRadius_,   sldTerminusRadius_);

    // Trajectory attachment selector (Emitter sub-section)
    layoutRow(lblEmitTraj_, sldEmitTraj_);
    const int emitterEndY = y;

    // ── Effect zone section ───────────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblZoneRadius_, sldZoneRadius_);
    layoutRow(lblZoneDepth_,  sldZoneDepth_);
    y += SECTION_GAP;

    lblZoneTarget_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 3;
        btnZoneTimbreX_.setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnZoneTimbreY_.setBounds(x + bw * 1, y, bw,         BTN_ROW_H);
        btnZoneAmp_    .setBounds(x + bw * 2, y, w - bw * 2, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;
    {
        const int bw = w / 2;
        btnZonePan_  .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnZonePitch_.setBounds(x + bw * 1, y, w - bw * 1, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;

    lblZoneFalloff_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 2;
        btnZoneFalloffLinear_  .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnZoneFalloffGaussian_.setBounds(x + bw * 1, y, w - bw * 1, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;
    const int zoneEndY = y;

    // ── Flux gate section ─────────────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblGateLength_, sldGateLength_);
    layoutRow(lblGateAngle_,  sldGateAngle_);
    const int gateEndY = y;

    // ── Path object section ───────────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblPathRadius_, sldPathRadius_);
    layoutRow(lblPathSnap_,   sldPathSnap_);
    layoutRow(lblPathEscape_, sldPathEscape_);
    const int pathEndY = y;

    // ── Trajectory path section ───────────────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblTrajRadius_, sldTrajRadius_);
    // Mode toggle: Auto | Manual
    lblTrajMode_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 2;
        btnTrajModeAuto_  .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnTrajModeManual_.setBounds(x + bw * 1, y, w - bw * 1, BTN_ROW_H);
    }
    y += BTN_ROW_H + 2;
    // Speed (AutoPlay) and Position (Manual) overlap — only the active one is visible.
    const int trajModeRowY = y;
    layoutRow(lblTrajSpeed_, sldTrajSpeed_);
    y = trajModeRowY;
    layoutRow(lblTrajPos_,   sldTrajPos_);
    const int trajEndY = y;

    // ── Tangent-force ("Flow") path section ────────────────────────────────────
    y = sectionTopY;
    layoutRow(lblFlowRadius_,    sldFlowRadius_);
    layoutRow(lblFlowWidth_,     sldFlowWidth_);
    layoutRow(lblFlowStrength_,  sldFlowStrength_);
    layoutRow(lblFlowChirality_, sldFlowChirality_);
    const int flowEndY = y;

    // ── Size panelContent_ to the currently selected section's height ────────
    // No selection → use the smallest (Anchor) so the viewport stays compact.
    int contentH = anchorEndY;
    switch (selection_.kind)
    {
        case ObjectKind::TimbralAnchor:  contentH = anchorEndY;  break;
        case ObjectKind::FieldObject:    contentH = fieldEndY;   break;
        case ObjectKind::Emitter:        contentH = emitterEndY; break;
        case ObjectKind::EffectZone:     contentH = zoneEndY;    break;
        case ObjectKind::FluxGate:       contentH = gateEndY;    break;
        case ObjectKind::PathObject:     contentH = pathEndY;    break;
        case ObjectKind::TrajectoryPath: contentH = trajEndY;    break;
        case ObjectKind::TangentPath:    contentH = flowEndY;    break;
        default:                          contentH = anchorEndY;  break;
    }
    panelContent_.setSize(w, contentH);
}

// ─────────────────────────────────────────────────────────────────────────────
// setupSliders — called once from constructor
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::setupSliders()
{
    // ── Style helpers ──────────────────────────────────────────────────────────
    auto styleSlider = [](juce::Slider& s, double lo, double hi)
    {
        s.setRange(lo, hi, 0.0);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        s.setColour(juce::Slider::backgroundColourId,        juce::Colour(0xFF252522));
        s.setColour(juce::Slider::trackColourId,             juce::Colour(0xFF3DC9B0));
        s.setColour(juce::Slider::thumbColourId,             juce::Colour(0xFFE8E4DC));
        s.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xFF888880));
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFF1A1A18));
        s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colour(0xFF252522));
        s.setVisible(false);
    };

    auto styleLabel = [](juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(11.0f));
        l.setColour(juce::Label::textColourId, juce::Colour(0xFF888880));
        l.setVisible(false);
    };

    auto styleSpawnBtn = [](juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF252522));
        b.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFF888880));
        b.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFE8E4DC));
    };

    auto styleBoundBtn = [](juce::TextButton& b)
    {
        b.setClickingTogglesState(false);
        b.setColour(juce::TextButton::buttonColourId,    juce::Colour(0xFF252522));
        b.setColour(juce::TextButton::buttonOnColourId,  juce::Colour(0xFF3DC9B0));
        b.setColour(juce::TextButton::textColourOffId,   juce::Colour(0xFF888880));
        b.setColour(juce::TextButton::textColourOnId,    juce::Colour(0xFF1A1A18));
    };

    // ── Spawn buttons (always visible) ─────────────────────────────────────────
    styleSpawnBtn(btnAddAtt_);   addAndMakeVisible(btnAddAtt_);
    styleSpawnBtn(btnAddRep_);   addAndMakeVisible(btnAddRep_);
    styleSpawnBtn(btnAddVor_);   addAndMakeVisible(btnAddVor_);
    styleSpawnBtn(btnAddEmit_);  addAndMakeVisible(btnAddEmit_);
    styleSpawnBtn(btnAddAnch_);  addAndMakeVisible(btnAddAnch_);
    styleSpawnBtn(btnAddZone_);  addAndMakeVisible(btnAddZone_);
    styleSpawnBtn(btnAddFlux_);  addAndMakeVisible(btnAddFlux_);
    styleSpawnBtn(btnAddPath_);  addAndMakeVisible(btnAddPath_);
    styleSpawnBtn(btnAddTraj_);  addAndMakeVisible(btnAddTraj_);
    styleSpawnBtn(btnAddFlow_);  addAndMakeVisible(btnAddFlow_);

    // Spawn buttons arm placement mode — click canvas to place at desired position.
    // Clicking the same button again disarms without placing anything.
    auto armSpawn = [this](SpawnKind k)
    {
        pendingSpawn_ = (pendingSpawn_ == k) ? SpawnKind::None : k;
        btnAddAtt_ .setToggleState(pendingSpawn_ == SpawnKind::Attractor,      juce::dontSendNotification);
        btnAddRep_ .setToggleState(pendingSpawn_ == SpawnKind::Repeller,       juce::dontSendNotification);
        btnAddVor_ .setToggleState(pendingSpawn_ == SpawnKind::Vortex,         juce::dontSendNotification);
        btnAddEmit_.setToggleState(pendingSpawn_ == SpawnKind::Emitter,        juce::dontSendNotification);
        btnAddAnch_.setToggleState(pendingSpawn_ == SpawnKind::TimbralAnchor,  juce::dontSendNotification);
        btnAddZone_.setToggleState(pendingSpawn_ == SpawnKind::EffectZone,     juce::dontSendNotification);
        btnAddFlux_.setToggleState(pendingSpawn_ == SpawnKind::FluxGate,       juce::dontSendNotification);
        btnAddPath_.setToggleState(pendingSpawn_ == SpawnKind::PathObject,     juce::dontSendNotification);
        btnAddTraj_.setToggleState(pendingSpawn_ == SpawnKind::TrajectoryPath, juce::dontSendNotification);
        btnAddFlow_.setToggleState(pendingSpawn_ == SpawnKind::TangentPath,    juce::dontSendNotification);
        repaint();
    };

    btnAddAtt_ .onClick = [armSpawn]{ armSpawn(SpawnKind::Attractor);      };
    btnAddRep_ .onClick = [armSpawn]{ armSpawn(SpawnKind::Repeller);       };
    btnAddVor_ .onClick = [armSpawn]{ armSpawn(SpawnKind::Vortex);         };
    btnAddEmit_.onClick = [armSpawn]{ armSpawn(SpawnKind::Emitter);        };
    btnAddAnch_.onClick = [armSpawn]{ armSpawn(SpawnKind::TimbralAnchor);  };
    btnAddZone_.onClick = [armSpawn]{ armSpawn(SpawnKind::EffectZone);     };
    btnAddFlux_.onClick = [armSpawn]{ armSpawn(SpawnKind::FluxGate);       };
    btnAddPath_.onClick = [armSpawn]{ armSpawn(SpawnKind::PathObject);     };
    btnAddTraj_.onClick = [armSpawn]{ armSpawn(SpawnKind::TrajectoryPath); };
    btnAddFlow_.onClick = [armSpawn]{ armSpawn(SpawnKind::TangentPath);    };

    // ── Panel header + remove button ──────────────────────────────────────────
    lblPanelHeader_.setText("No Selection", juce::dontSendNotification);
    lblPanelHeader_.setFont(juce::FontOptions(13.0f));
    lblPanelHeader_.setColour(juce::Label::textColourId, juce::Colour(0xFFD0CBC2));
    addAndMakeVisible(lblPanelHeader_);

    btnRemove_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF252522));
    btnRemove_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFE24B4A));
    btnRemove_.setVisible(false);
    addAndMakeVisible(btnRemove_);

    btnRemove_.onClick = [this]
    {
        if (!selection_.valid()) return;
        ManifoldEdit::Type t;
        switch (selection_.kind)
        {
            case ObjectKind::FieldObject:   t = ManifoldEdit::Type::RemoveFieldObject;   break;
            case ObjectKind::Emitter:       t = ManifoldEdit::Type::RemoveEmitter;       break;
            case ObjectKind::TimbralAnchor: t = ManifoldEdit::Type::RemoveTimbralAnchor; break;
            case ObjectKind::EffectZone:    t = ManifoldEdit::Type::RemoveEffectZone;    break;
            case ObjectKind::FluxGate:      t = ManifoldEdit::Type::RemoveFluxGate;      break;
            case ObjectKind::PathObject:    t = ManifoldEdit::Type::RemovePathObject;    break;
            case ObjectKind::TrajectoryPath:t = ManifoldEdit::Type::RemoveTrajectoryPath;break;
            case ObjectKind::TangentPath:   t = ManifoldEdit::Type::RemoveTangentPath;   break;
            default: return;
        }
        sendEdit(t, selection_.index, 0.0f, 0.0f);
        selection_ = {};
        drag_.active = false;
        updatePanel();
        repaint();
    };

    // ── Anchor sliders ─────────────────────────────────────────────────────────
    styleLabel (lblBrightness_,    "Brightness");
    styleLabel (lblInharmonicity_, "Inharmonicity");
    styleSlider(sldBrightness_,    0.0, 1.0);
    styleSlider(sldInharmonicity_, 0.0, 1.0);
    addAndMakeVisible(lblBrightness_);    addAndMakeVisible(sldBrightness_);
    addAndMakeVisible(lblInharmonicity_); addAndMakeVisible(sldInharmonicity_);

    sldBrightness_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTimbralAnchorTimbreX,
                     selection_.index, (float)sldBrightness_.getValue());
    };
    sldInharmonicity_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTimbralAnchorTimbreY,
                     selection_.index, (float)sldInharmonicity_.getValue());
    };

    // ── Field object sliders ───────────────────────────────────────────────────
    styleLabel (lblFOStrength_,  "Strength");
    styleLabel (lblFORadius_,    "Radius");
    styleLabel (lblFOChirality_, "Chirality");
    styleSlider(sldFOStrength_,  0.0,  1.0);
    styleSlider(sldFORadius_,    0.01, 1.0);
    styleSlider(sldFOChirality_,-1.0,  1.0);
    addAndMakeVisible(lblFOStrength_);  addAndMakeVisible(sldFOStrength_);
    addAndMakeVisible(lblFORadius_);    addAndMakeVisible(sldFORadius_);
    addAndMakeVisible(lblFOChirality_); addAndMakeVisible(sldFOChirality_);

    sldFOStrength_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetFieldObjectStrength,
                     selection_.index, (float)sldFOStrength_.getValue());
    };
    sldFORadius_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetFieldObjectRadius,
                     selection_.index, (float)sldFORadius_.getValue());
    };
    sldFOChirality_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetFieldObjectChirality,
                     selection_.index, (float)sldFOChirality_.getValue());
    };

    // ── Emitter sliders ────────────────────────────────────────────────────────
    styleLabel (lblKeyLow_,      "Key Low");
    styleLabel (lblKeyHigh_,     "Key High");
    styleLabel (lblEmitAngle_,   "Launch Angle");
    styleLabel (lblEmitSpeed_,   "Launch Speed");
    styleLabel (lblEmitAttack_,  "Attack (s)");
    styleLabel (lblEmitDecay_,   "Decay (s)");
    styleLabel (lblEmitSustain_, "Sustain");
    styleLabel (lblEmitRelease_, "Release (s)");

    // Key range sliders: integer MIDI notes 0–127, displayed as note names (C4 etc.)
    auto midiNoteName = [](double v) -> juce::String
    {
        const int   n     = juce::jlimit(0, 127, (int)v);
        const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String(names[n % 12]) + juce::String(n / 12 - 1);
    };

    styleSlider(sldKeyLow_,  0.0, 127.0);
    sldKeyLow_.setNumDecimalPlacesToDisplay(0);
    sldKeyLow_.textFromValueFunction  = midiNoteName;
    styleSlider(sldKeyHigh_, 0.0, 127.0);
    sldKeyHigh_.setNumDecimalPlacesToDisplay(0);
    sldKeyHigh_.textFromValueFunction = midiNoteName;

    addAndMakeVisible(lblKeyLow_);  addAndMakeVisible(sldKeyLow_);
    addAndMakeVisible(lblKeyHigh_); addAndMakeVisible(sldKeyHigh_);

    sldKeyLow_.onValueChange = [this]
    {
        if (ignoreSliderCallbacks_) return;
        // Enforce low ≤ high
        if (sldKeyLow_.getValue() > sldKeyHigh_.getValue())
            sldKeyHigh_.setValue(sldKeyLow_.getValue(), juce::dontSendNotification);
        sendEdit(ManifoldEdit::Type::SetEmitterKeyLow,
                 selection_.index, (float)sldKeyLow_.getValue());
    };
    sldKeyHigh_.onValueChange = [this]
    {
        if (ignoreSliderCallbacks_) return;
        if (sldKeyHigh_.getValue() < sldKeyLow_.getValue())
            sldKeyLow_.setValue(sldKeyHigh_.getValue(), juce::dontSendNotification);
        sendEdit(ManifoldEdit::Type::SetEmitterKeyHigh,
                 selection_.index, (float)sldKeyHigh_.getValue());
    };

    // Transpose sliders — integer oct/semi, float cents
    styleLabel(lblTransposeOct_,   "Oct");
    styleLabel(lblTransposeSemi_,  "Semi");
    styleLabel(lblTransposeCents_, "Cents");

    styleSlider(sldTransposeOct_,   -4.0,  4.0);   sldTransposeOct_.setNumDecimalPlacesToDisplay(0);
    styleSlider(sldTransposeSemi_, -12.0, 12.0);   sldTransposeSemi_.setNumDecimalPlacesToDisplay(0);
    styleSlider(sldTransposeCents_,-100.0,100.0);   sldTransposeCents_.setNumDecimalPlacesToDisplay(1);

    addAndMakeVisible(lblTransposeOct_);   addAndMakeVisible(sldTransposeOct_);
    addAndMakeVisible(lblTransposeSemi_);  addAndMakeVisible(sldTransposeSemi_);
    addAndMakeVisible(lblTransposeCents_); addAndMakeVisible(sldTransposeCents_);

    sldTransposeOct_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTransposeOct,
                     selection_.index, (float)sldTransposeOct_.getValue());
    };
    sldTransposeSemi_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTransposeSemi,
                     selection_.index, (float)sldTransposeSemi_.getValue());
    };
    sldTransposeCents_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTransposeCents,
                     selection_.index, (float)sldTransposeCents_.getValue());
    };

    // Pan slider
    styleLabel(lblEmitPan_, "Pan");
    styleSlider(sldEmitPan_, -1.0, 1.0);
    sldEmitPan_.setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblEmitPan_); addAndMakeVisible(sldEmitPan_);
    sldEmitPan_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterPan,
                     selection_.index, (float)sldEmitPan_.getValue());
    };

    // ── Terminus ───────────────────────────────────────────────────────────────
    styleBoundBtn(btnTerminusEnabled_);
    addAndMakeVisible(btnTerminusEnabled_);
    btnTerminusEnabled_.onClick = [this]
    {
        const bool now = !btnTerminusEnabled_.getToggleState();
        btnTerminusEnabled_.setToggleState(now, juce::dontSendNotification);
        sendEdit(ManifoldEdit::Type::SetEmitterTerminusEnabled,
                 selection_.index, now ? 1.0f : 0.0f);
        sldTerminusStrength_.setVisible(now);
        lblTerminusStrength_.setVisible(now);
        sldTerminusRadius_.setVisible(now);
        lblTerminusRadius_.setVisible(now);
    };

    styleLabel(lblTerminusStrength_, "Pull Strength");
    styleLabel(lblTerminusRadius_,   "Arrival Radius");
    styleSlider(sldTerminusStrength_, 0.0, 2.0);
    styleSlider(sldTerminusRadius_,   0.005, 0.25);
    sldTerminusStrength_.setNumDecimalPlacesToDisplay(2);
    sldTerminusRadius_.setNumDecimalPlacesToDisplay(3);

    addAndMakeVisible(lblTerminusStrength_); addAndMakeVisible(sldTerminusStrength_);
    addAndMakeVisible(lblTerminusRadius_);   addAndMakeVisible(sldTerminusRadius_);

    sldTerminusStrength_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTerminusStrength,
                     selection_.index, (float)sldTerminusStrength_.getValue());
    };
    sldTerminusRadius_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTerminusRadius,
                     selection_.index, (float)sldTerminusRadius_.getValue());
    };

    // Launch angle: simple [-π, π] range displayed in degrees.
    // Continuous-rotation mode will be added when automation is implemented.
    {
        const double pi = juce::MathConstants<double>::pi;
        styleSlider(sldEmitAngle_, -pi, pi);
        sldEmitAngle_.textFromValueFunction = [](double v) -> juce::String
        {
            return juce::String(v * 180.0 / juce::MathConstants<double>::pi, 1)
                 + juce::String::fromUTF8("\xc2\xb0");
        };
    }

    styleSlider(sldEmitSpeed_,    0.0,   0.8);
    styleSlider(sldEmitAttack_,   0.001, 5.0);
    styleSlider(sldEmitDecay_,    0.001, 5.0);
    styleSlider(sldEmitSustain_,  0.0,   1.0);
    styleSlider(sldEmitRelease_,  0.001, 10.0);

    addAndMakeVisible(lblEmitAngle_);   addAndMakeVisible(sldEmitAngle_);
    addAndMakeVisible(lblEmitSpeed_);   addAndMakeVisible(sldEmitSpeed_);
    addAndMakeVisible(lblEmitAttack_);  addAndMakeVisible(sldEmitAttack_);
    addAndMakeVisible(lblEmitDecay_);   addAndMakeVisible(sldEmitDecay_);
    addAndMakeVisible(lblEmitSustain_); addAndMakeVisible(sldEmitSustain_);
    addAndMakeVisible(lblEmitRelease_); addAndMakeVisible(sldEmitRelease_);

    sldEmitAngle_.onValueChange = [this]
    {
        if (ignoreSliderCallbacks_) return;
        sendEdit(ManifoldEdit::Type::SetEmitterLaunchAngle,
                 selection_.index, (float)sldEmitAngle_.getValue());
    };
    sldEmitSpeed_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterLaunchSpeed,
                     selection_.index, (float)sldEmitSpeed_.getValue());
    };
    sldEmitAttack_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterAttack,
                     selection_.index, (float)sldEmitAttack_.getValue());
    };
    sldEmitDecay_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterDecay,
                     selection_.index, (float)sldEmitDecay_.getValue());
    };
    sldEmitSustain_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterSustain,
                     selection_.index, (float)sldEmitSustain_.getValue());
    };
    sldEmitRelease_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterRelease,
                     selection_.index, (float)sldEmitRelease_.getValue());
    };

    // ── Effect zone controls ───────────────────────────────────────────────────
    styleLabel (lblZoneRadius_, "Radius");
    styleLabel (lblZoneDepth_,  "Depth");
    styleSlider(sldZoneRadius_, 0.02, 0.75);
    styleSlider(sldZoneDepth_,  -1.0, 1.0);
    sldZoneRadius_.setNumDecimalPlacesToDisplay(2);
    sldZoneDepth_ .setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblZoneRadius_); addAndMakeVisible(sldZoneRadius_);
    addAndMakeVisible(lblZoneDepth_);  addAndMakeVisible(sldZoneDepth_);

    sldZoneRadius_.onValueChange = [this]
    {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEffectZoneRadius,
                     selection_.index, (float)sldZoneRadius_.getValue());
    };
    sldZoneDepth_.onValueChange = [this]
    {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEffectZoneDepth,
                     selection_.index, (float)sldZoneDepth_.getValue());
    };

    styleLabel(lblZoneTarget_,  "Target");
    styleLabel(lblZoneFalloff_, "Falloff");
    addAndMakeVisible(lblZoneTarget_);  addAndMakeVisible(lblZoneFalloff_);

    // Target buttons
    for (auto* b : { &btnZoneTimbreX_, &btnZoneTimbreY_, &btnZoneAmp_,
                     &btnZonePan_, &btnZonePitch_ })
    {
        styleBoundBtn(*b);
        addAndMakeVisible(*b);
    }

    auto zoneTargetClick = [this](ZoneTarget t)
    {
        const bool isPitch = (t == ZoneTarget::Pitch);
        if (isPitch)
        {
            sldZoneDepth_.setRange(-24.0, 24.0, 0.0);
            lblZoneDepth_.setText("Depth (semi)", juce::dontSendNotification);
        }
        else
        {
            sldZoneDepth_.setRange(-1.0, 1.0, 0.0);
            lblZoneDepth_.setText("Depth", juce::dontSendNotification);
        }
        sendEdit(ManifoldEdit::Type::SetEffectZoneTarget, selection_.index,
                 static_cast<float>(static_cast<uint8_t>(t)));
        btnZoneTimbreX_.setToggleState(t == ZoneTarget::TimbreX,   juce::dontSendNotification);
        btnZoneTimbreY_.setToggleState(t == ZoneTarget::TimbreY,   juce::dontSendNotification);
        btnZoneAmp_    .setToggleState(t == ZoneTarget::Amplitude, juce::dontSendNotification);
        btnZonePan_    .setToggleState(t == ZoneTarget::Pan,       juce::dontSendNotification);
        btnZonePitch_  .setToggleState(t == ZoneTarget::Pitch,     juce::dontSendNotification);
    };

    btnZoneTimbreX_.onClick = [zoneTargetClick]{ zoneTargetClick(ZoneTarget::TimbreX);   };
    btnZoneTimbreY_.onClick = [zoneTargetClick]{ zoneTargetClick(ZoneTarget::TimbreY);   };
    btnZoneAmp_    .onClick = [zoneTargetClick]{ zoneTargetClick(ZoneTarget::Amplitude); };
    btnZonePan_    .onClick = [zoneTargetClick]{ zoneTargetClick(ZoneTarget::Pan);       };
    btnZonePitch_  .onClick = [zoneTargetClick]{ zoneTargetClick(ZoneTarget::Pitch);     };

    // Falloff buttons
    styleBoundBtn(btnZoneFalloffLinear_);   addAndMakeVisible(btnZoneFalloffLinear_);
    styleBoundBtn(btnZoneFalloffGaussian_); addAndMakeVisible(btnZoneFalloffGaussian_);

    auto zoneFalloffClick = [this](ZoneFalloff f)
    {
        sendEdit(ManifoldEdit::Type::SetEffectZoneFalloff, selection_.index,
                 static_cast<float>(static_cast<uint8_t>(f)));
        btnZoneFalloffLinear_  .setToggleState(f == ZoneFalloff::Linear,   juce::dontSendNotification);
        btnZoneFalloffGaussian_.setToggleState(f == ZoneFalloff::Gaussian, juce::dontSendNotification);
    };

    btnZoneFalloffLinear_  .onClick = [zoneFalloffClick]{ zoneFalloffClick(ZoneFalloff::Linear);   };
    btnZoneFalloffGaussian_.onClick = [zoneFalloffClick]{ zoneFalloffClick(ZoneFalloff::Gaussian); };

    // ── Flux gate sliders ──────────────────────────────────────────────────────
    styleLabel(lblGateLength_, "Length");
    styleLabel(lblGateAngle_,  "Angle");
    styleSlider(sldGateLength_, 0.02, 0.9);
    sldGateLength_.setNumDecimalPlacesToDisplay(2);
    {
        const double pi = juce::MathConstants<double>::pi;
        styleSlider(sldGateAngle_, -pi, pi);
        sldGateAngle_.textFromValueFunction = [](double v) -> juce::String
        {
            return juce::String(v * 180.0 / juce::MathConstants<double>::pi, 1)
                 + juce::String::fromUTF8("\xc2\xb0");
        };
    }
    addAndMakeVisible(lblGateLength_); addAndMakeVisible(sldGateLength_);
    addAndMakeVisible(lblGateAngle_);  addAndMakeVisible(sldGateAngle_);

    sldGateLength_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetFluxGateLength,
                     selection_.index, (float)sldGateLength_.getValue());
    };
    sldGateAngle_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetFluxGateAngle,
                     selection_.index, (float)sldGateAngle_.getValue());
    };

    // ── Path object sliders ────────────────────────────────────────────────────
    styleLabel(lblPathRadius_, "Radius");
    styleLabel(lblPathSnap_,   "Snap Radius");
    styleLabel(lblPathEscape_, "Escape Force");
    styleSlider(sldPathRadius_, 0.02, 0.45);
    styleSlider(sldPathSnap_,   0.005, 0.15);
    styleSlider(sldPathEscape_, 0.0,   5.0);
    sldPathRadius_.setNumDecimalPlacesToDisplay(2);
    sldPathSnap_  .setNumDecimalPlacesToDisplay(3);
    sldPathEscape_.setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblPathRadius_); addAndMakeVisible(sldPathRadius_);
    addAndMakeVisible(lblPathSnap_);   addAndMakeVisible(sldPathSnap_);
    addAndMakeVisible(lblPathEscape_); addAndMakeVisible(sldPathEscape_);

    sldPathRadius_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetPathObjectRadius,
                     selection_.index, (float)sldPathRadius_.getValue());
    };
    sldPathSnap_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetPathObjectSnapRadius,
                     selection_.index, (float)sldPathSnap_.getValue());
    };
    sldPathEscape_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetPathObjectEscapeForce,
                     selection_.index, (float)sldPathEscape_.getValue());
    };

    // ── Trajectory path sliders ────────────────────────────────────────────────
    styleLabel(lblTrajRadius_, "Radius");
    styleLabel(lblTrajSpeed_,  "Speed (Hz)");
    styleLabel(lblTrajPos_,    "Position (t)");
    styleLabel(lblTrajMode_,   "Mode");
    styleSlider(sldTrajRadius_, 0.02, 0.45);
    styleSlider(sldTrajSpeed_,  -4.0,  4.0);
    styleSlider(sldTrajPos_,     0.0,  1.0);
    sldTrajRadius_.setNumDecimalPlacesToDisplay(2);
    sldTrajSpeed_ .setNumDecimalPlacesToDisplay(2);
    sldTrajPos_   .setNumDecimalPlacesToDisplay(3);
    addAndMakeVisible(lblTrajRadius_); addAndMakeVisible(sldTrajRadius_);
    addAndMakeVisible(lblTrajSpeed_);  addAndMakeVisible(sldTrajSpeed_);
    addAndMakeVisible(lblTrajPos_);    addAndMakeVisible(sldTrajPos_);
    addAndMakeVisible(lblTrajMode_);
    styleBoundBtn(btnTrajModeAuto_);   addAndMakeVisible(btnTrajModeAuto_);
    styleBoundBtn(btnTrajModeManual_); addAndMakeVisible(btnTrajModeManual_);

    sldTrajRadius_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTrajectoryPathRadius,
                     selection_.index, (float)sldTrajRadius_.getValue());
    };
    sldTrajSpeed_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTrajectoryPathSpeed,
                     selection_.index, (float)sldTrajSpeed_.getValue());
    };
    sldTrajPos_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTrajectoryPathCurrentT,
                     selection_.index, (float)sldTrajPos_.getValue());
    };

    auto trajModeClick = [this](TrajectoryMode m) {
        sendEdit(ManifoldEdit::Type::SetTrajectoryPathMode, selection_.index,
                 static_cast<float>(static_cast<uint8_t>(m)));
        const bool autoMode   = (m == TrajectoryMode::AutoPlay);
        const bool manualMode = (m == TrajectoryMode::Manual);
        btnTrajModeAuto_  .setToggleState(autoMode,   juce::dontSendNotification);
        btnTrajModeManual_.setToggleState(manualMode, juce::dontSendNotification);
        // Toggle row visibility immediately — updatePanel runs only on
        // selection change, not on the timer, so without this the swap
        // wouldn't appear until the user re-selects the object.
        lblTrajSpeed_.setVisible(autoMode);   sldTrajSpeed_.setVisible(autoMode);
        lblTrajPos_  .setVisible(manualMode); sldTrajPos_  .setVisible(manualMode);
    };
    btnTrajModeAuto_  .onClick = [trajModeClick]{ trajModeClick(TrajectoryMode::AutoPlay); };
    btnTrajModeManual_.onClick = [trajModeClick]{ trajModeClick(TrajectoryMode::Manual);   };

    // ── Tangent-force ("Flow") path sliders ────────────────────────────────────
    styleLabel(lblFlowRadius_,    "Radius");
    styleLabel(lblFlowWidth_,     "Width");
    styleLabel(lblFlowStrength_,  "Strength");
    styleLabel(lblFlowChirality_, "Chirality");
    styleSlider(sldFlowRadius_,    0.02, 0.45);
    styleSlider(sldFlowWidth_,     0.01, 0.30);
    styleSlider(sldFlowStrength_,  0.0,  2.0);
    styleSlider(sldFlowChirality_,-1.0,  1.0);
    sldFlowRadius_  .setNumDecimalPlacesToDisplay(2);
    sldFlowWidth_   .setNumDecimalPlacesToDisplay(2);
    sldFlowStrength_.setNumDecimalPlacesToDisplay(2);
    sldFlowChirality_.setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblFlowRadius_);    addAndMakeVisible(sldFlowRadius_);
    addAndMakeVisible(lblFlowWidth_);     addAndMakeVisible(sldFlowWidth_);
    addAndMakeVisible(lblFlowStrength_);  addAndMakeVisible(sldFlowStrength_);
    addAndMakeVisible(lblFlowChirality_); addAndMakeVisible(sldFlowChirality_);

    sldFlowRadius_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTangentPathRadius,
                     selection_.index, (float)sldFlowRadius_.getValue());
    };
    sldFlowWidth_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTangentPathWidth,
                     selection_.index, (float)sldFlowWidth_.getValue());
    };
    sldFlowStrength_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTangentPathStrength,
                     selection_.index, (float)sldFlowStrength_.getValue());
    };
    sldFlowChirality_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetTangentPathChirality,
                     selection_.index, (float)sldFlowChirality_.getValue());
    };

    // ── Emitter ↔ Trajectory attachment slider ─────────────────────────────────
    // Integer slider: -1 = stationary (no attachment), 0..MAX_TRAJECTORY_PATHS-1
    // = follow that trajectory path. Text shows "—" for -1 and "Traj N" for 0+.
    styleLabel(lblEmitTraj_, "Trajectory");
    styleSlider(sldEmitTraj_, -1.0, (double)(MAX_TRAJECTORY_PATHS - 1));
    sldEmitTraj_.setNumDecimalPlacesToDisplay(0);
    sldEmitTraj_.textFromValueFunction = [](double v) -> juce::String
    {
        const int n = (int)v;
        if (n < 0) return juce::String::fromUTF8("\xe2\x80\x94");  // em-dash
        return "Traj " + juce::String(n);
    };
    addAndMakeVisible(lblEmitTraj_); addAndMakeVisible(sldEmitTraj_);
    sldEmitTraj_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterTrajectoryPath,
                     selection_.index, (float)sldEmitTraj_.getValue());
    };

    // ── Global topology row — always visible ──────────────────────────────────
    styleLabel(lblBoundary_, "Topology");
    addAndMakeVisible(lblBoundary_);

    styleBoundBtn(btnBoundWrap_);      addAndMakeVisible(btnBoundWrap_);
    styleBoundBtn(btnBoundReflect_);   addAndMakeVisible(btnBoundReflect_);
    styleBoundBtn(btnBoundTerminate_); addAndMakeVisible(btnBoundTerminate_);
    styleBoundBtn(btnBoundKlein_);     addAndMakeVisible(btnBoundKlein_);

    // Default: Wrap is active at startup
    btnBoundWrap_.setToggleState(true, juce::dontSendNotification);

    auto boundaryClick = [this](BoundaryBehavior b)
    {
        sendEdit(ManifoldEdit::Type::SetGlobalBoundary, 0,
                 static_cast<float>(static_cast<uint8_t>(b)));
        // Update toggle state immediately without waiting for next snapshot poll
        btnBoundWrap_     .setToggleState(b == BoundaryBehavior::Wrap,        juce::dontSendNotification);
        btnBoundReflect_  .setToggleState(b == BoundaryBehavior::Reflect,     juce::dontSendNotification);
        btnBoundTerminate_.setToggleState(b == BoundaryBehavior::Terminate,   juce::dontSendNotification);
        btnBoundKlein_    .setToggleState(b == BoundaryBehavior::KleinBottle, juce::dontSendNotification);
    };

    btnBoundWrap_     .onClick = [boundaryClick]{ boundaryClick(BoundaryBehavior::Wrap);        };
    btnBoundReflect_  .onClick = [boundaryClick]{ boundaryClick(BoundaryBehavior::Reflect);     };
    btnBoundTerminate_.onClick = [boundaryClick]{ boundaryClick(BoundaryBehavior::Terminate);   };
    btnBoundKlein_    .onClick = [boundaryClick]{ boundaryClick(BoundaryBehavior::KleinBottle); };

    // ── Per-Emitter polyphony row (visible only when an Emitter is selected) ───
    styleLabel(lblEmitPolyMode_, "Voices");
    addAndMakeVisible(lblEmitPolyMode_);

    styleBoundBtn(btnEmitPoly_);    addAndMakeVisible(btnEmitPoly_);
    styleBoundBtn(btnEmitMono_);    addAndMakeVisible(btnEmitMono_);
    styleBoundBtn(btnEmitLegato_);  addAndMakeVisible(btnEmitLegato_);
    styleBoundBtn(btnEmitSlur_);    addAndMakeVisible(btnEmitSlur_);
    btnEmitPoly_.setVisible(false);
    btnEmitMono_.setVisible(false);
    btnEmitLegato_.setVisible(false);
    btnEmitSlur_.setVisible(false);

    auto emitterPolyClick = [this](PolyMode p)
    {
        if (selection_.kind != ObjectKind::Emitter || selection_.index < 0) return;
        sendEdit(ManifoldEdit::Type::SetEmitterPolyMode, selection_.index,
                 static_cast<float>(static_cast<uint8_t>(p)));
        btnEmitPoly_  .setToggleState(p == PolyMode::Polyphonic, juce::dontSendNotification);
        btnEmitMono_  .setToggleState(p == PolyMode::Mono,       juce::dontSendNotification);
        btnEmitLegato_.setToggleState(p == PolyMode::Legato,     juce::dontSendNotification);
        btnEmitSlur_  .setToggleState(p == PolyMode::LegatoSlur, juce::dontSendNotification);
    };

    btnEmitPoly_  .onClick = [emitterPolyClick]{ emitterPolyClick(PolyMode::Polyphonic); };
    btnEmitMono_  .onClick = [emitterPolyClick]{ emitterPolyClick(PolyMode::Mono);       };
    btnEmitLegato_.onClick = [emitterPolyClick]{ emitterPolyClick(PolyMode::Legato);     };
    btnEmitSlur_  .onClick = [emitterPolyClick]{ emitterPolyClick(PolyMode::LegatoSlur); };

    // ── Mass slider (Emitter section) ──────────────────────────────────────────
    // Sets the spawn-time mass of new Morphons from this Emitter. Mass scales
    // the inverse-mass response to all field forces (a = F/mass), so heavier
    // Morphons resist Attractors/Repellers/Vortices more strongly.
    styleLabel(lblEmitMass_, "Mass");
    styleSlider(sldEmitMass_, 0.1, 4.0);
    sldEmitMass_.setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblEmitMass_); addAndMakeVisible(sldEmitMass_);
    sldEmitMass_.onValueChange = [this] {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetEmitterSpawnMass,
                     selection_.index, (float)sldEmitMass_.getValue());
    };

    // ── Glide time — portamento rate; always visible ───────────────────────────
    // Applies during Legato / Slur retarget: fundamentalHz slides exponentially
    // (in log-frequency space) from the old pitch toward the new one.
    // 0 = instant (no glide).  glideTime is the RC time-constant in seconds
    // (63 % of the way there after glideTime seconds, ~99% after 5×glideTime).
    styleLabel(lblGlideTime_, "Glide (s)");
    styleSlider(sldGlideTime_, 0.0, 5.0);
    sldGlideTime_.setNumDecimalPlacesToDisplay(2);
    addAndMakeVisible(lblGlideTime_);   // Override styleLabel's setVisible(false)
    addAndMakeVisible(sldGlideTime_);   // Override styleSlider's setVisible(false)
    sldGlideTime_.onValueChange = [this]
    {
        if (!ignoreSliderCallbacks_)
            sendEdit(ManifoldEdit::Type::SetGlideTime, 0, (float)sldGlideTime_.getValue());
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// installPanelViewport — wrap per-selection sections in a scrollable Viewport
//
// Called once from the constructor, after setupSliders() has parented every
// component to `this`. We re-parent the per-section components (Anchor /
// FieldObject / Emitter / Zone / Gate / Path / Trajectory / Flow sliders +
// buttons) into panelContent_, which becomes the viewed component of
// panelViewport_. Always-visible widgets (spawn buttons, topology row, glide
// slider, panel header, remove button) remain direct children of `this`.
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::installPanelViewport()
{
    panelViewport_.setScrollBarsShown(true, false);
    panelViewport_.setScrollBarThickness(8);
    panelViewport_.setViewedComponent(&panelContent_, false);
    addAndMakeVisible(panelViewport_);

    juce::Component* perSection[] = {
        // Anchor
        &lblBrightness_,       &sldBrightness_,
        &lblInharmonicity_,    &sldInharmonicity_,
        // Field object
        &lblFOStrength_,       &sldFOStrength_,
        &lblFORadius_,         &sldFORadius_,
        &lblFOChirality_,      &sldFOChirality_,
        // Emitter — voice mode buttons
        &lblEmitPolyMode_,
        &btnEmitPoly_, &btnEmitMono_, &btnEmitLegato_, &btnEmitSlur_,
        // Emitter — sliders / toggles
        &lblKeyLow_,           &sldKeyLow_,
        &lblKeyHigh_,          &sldKeyHigh_,
        &lblTransposeOct_,     &sldTransposeOct_,
        &lblTransposeSemi_,    &sldTransposeSemi_,
        &lblTransposeCents_,   &sldTransposeCents_,
        &lblEmitPan_,          &sldEmitPan_,
        &lblEmitMass_,         &sldEmitMass_,
        &lblEmitAngle_,        &sldEmitAngle_,
        &lblEmitSpeed_,        &sldEmitSpeed_,
        &lblEmitAttack_,       &sldEmitAttack_,
        &lblEmitDecay_,        &sldEmitDecay_,
        &lblEmitSustain_,      &sldEmitSustain_,
        &lblEmitRelease_,      &sldEmitRelease_,
        &btnTerminusEnabled_,
        &lblTerminusStrength_, &sldTerminusStrength_,
        &lblTerminusRadius_,   &sldTerminusRadius_,
        &lblEmitTraj_,         &sldEmitTraj_,
        // Zone
        &lblZoneRadius_,       &sldZoneRadius_,
        &lblZoneDepth_,        &sldZoneDepth_,
        &lblZoneTarget_,       &lblZoneFalloff_,
        &btnZoneTimbreX_, &btnZoneTimbreY_, &btnZoneAmp_,
        &btnZonePan_,     &btnZonePitch_,
        &btnZoneFalloffLinear_, &btnZoneFalloffGaussian_,
        // Gate
        &lblGateLength_,       &sldGateLength_,
        &lblGateAngle_,        &sldGateAngle_,
        // Path
        &lblPathRadius_,       &sldPathRadius_,
        &lblPathSnap_,         &sldPathSnap_,
        &lblPathEscape_,       &sldPathEscape_,
        // Trajectory
        &lblTrajRadius_,       &sldTrajRadius_,
        &lblTrajMode_,
        &btnTrajModeAuto_,     &btnTrajModeManual_,
        &lblTrajSpeed_,        &sldTrajSpeed_,
        &lblTrajPos_,          &sldTrajPos_,
        // Flow
        &lblFlowRadius_,       &sldFlowRadius_,
        &lblFlowWidth_,        &sldFlowWidth_,
        &lblFlowStrength_,     &sldFlowStrength_,
        &lblFlowChirality_,    &sldFlowChirality_,
    };
    for (auto* c : perSection)
        panelContent_.addAndMakeVisible(*c);   // Re-parents from `this` to panelContent_
}

// ─────────────────────────────────────────────────────────────────────────────
// updatePanel — refresh slider visibility + values from current snapshot
// Call on selection change; NOT in the timer (sliders only reflect state at
// time of selection — they are not live-updated by physics output).
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::updatePanel()
{
    // Re-layout the panel viewport's content when the selected section changes,
    // so panelContent_ is sized to fit the current section (and scrollbar
    // appears only when that specific section overflows the panel area).
    if (selection_.kind != lastPanelLayoutKind_)
    {
        lastPanelLayoutKind_ = selection_.kind;
        layoutPanel(getPanelBounds());
    }

    // ── Always-visible: global topology + glide ──────────────────────────────
    {
        const auto& state = processor_.getPhysicsStateForUI();

        const auto b = state.globalBoundary;
        btnBoundWrap_     .setToggleState(b == BoundaryBehavior::Wrap,        juce::dontSendNotification);
        btnBoundReflect_  .setToggleState(b == BoundaryBehavior::Reflect,     juce::dontSendNotification);
        btnBoundTerminate_.setToggleState(b == BoundaryBehavior::Terminate,   juce::dontSendNotification);
        btnBoundKlein_    .setToggleState(b == BoundaryBehavior::KleinBottle, juce::dontSendNotification);

        ignoreSliderCallbacks_ = true;
        sldGlideTime_.setValue(state.globalGlideTime, juce::dontSendNotification);
        ignoreSliderCallbacks_ = false;
    }

    // Hide every per-selection slider group first
    lblBrightness_.setVisible(false);    sldBrightness_.setVisible(false);
    lblInharmonicity_.setVisible(false); sldInharmonicity_.setVisible(false);

    lblFOStrength_.setVisible(false);    sldFOStrength_.setVisible(false);
    lblFORadius_.setVisible(false);      sldFORadius_.setVisible(false);
    lblFOChirality_.setVisible(false);   sldFOChirality_.setVisible(false);

    lblKeyLow_.setVisible(false);          sldKeyLow_.setVisible(false);
    lblKeyHigh_.setVisible(false);         sldKeyHigh_.setVisible(false);
    lblTransposeOct_.setVisible(false);    sldTransposeOct_.setVisible(false);
    lblTransposeSemi_.setVisible(false);   sldTransposeSemi_.setVisible(false);
    lblTransposeCents_.setVisible(false);  sldTransposeCents_.setVisible(false);
    lblEmitPan_.setVisible(false);             sldEmitPan_.setVisible(false);
    lblEmitMass_.setVisible(false);            sldEmitMass_.setVisible(false);
    lblEmitPolyMode_.setVisible(false);
    btnEmitPoly_.setVisible(false);            btnEmitMono_.setVisible(false);
    btnEmitLegato_.setVisible(false);          btnEmitSlur_.setVisible(false);
    btnTerminusEnabled_.setVisible(false);
    lblTerminusStrength_.setVisible(false);    sldTerminusStrength_.setVisible(false);
    lblTerminusRadius_.setVisible(false);      sldTerminusRadius_.setVisible(false);
    lblEmitAngle_.setVisible(false);           sldEmitAngle_.setVisible(false);
    lblEmitSpeed_.setVisible(false);     sldEmitSpeed_.setVisible(false);
    lblEmitAttack_.setVisible(false);    sldEmitAttack_.setVisible(false);
    lblEmitDecay_.setVisible(false);     sldEmitDecay_.setVisible(false);
    lblEmitSustain_.setVisible(false);   sldEmitSustain_.setVisible(false);
    lblEmitRelease_.setVisible(false);   sldEmitRelease_.setVisible(false);

    lblZoneRadius_.setVisible(false);       sldZoneRadius_.setVisible(false);
    lblZoneDepth_.setVisible(false);        sldZoneDepth_.setVisible(false);
    lblZoneTarget_.setVisible(false);       lblZoneFalloff_.setVisible(false);
    btnZoneTimbreX_.setVisible(false);      btnZoneTimbreY_.setVisible(false);
    btnZoneAmp_.setVisible(false);          btnZonePan_.setVisible(false);
    btnZonePitch_.setVisible(false);
    btnZoneFalloffLinear_.setVisible(false); btnZoneFalloffGaussian_.setVisible(false);

    lblGateLength_.setVisible(false);       sldGateLength_.setVisible(false);
    lblGateAngle_.setVisible(false);        sldGateAngle_.setVisible(false);

    lblPathRadius_.setVisible(false);       sldPathRadius_.setVisible(false);
    lblPathSnap_.setVisible(false);         sldPathSnap_.setVisible(false);
    lblPathEscape_.setVisible(false);       sldPathEscape_.setVisible(false);

    lblTrajRadius_.setVisible(false);       sldTrajRadius_.setVisible(false);
    lblTrajSpeed_.setVisible(false);        sldTrajSpeed_.setVisible(false);
    lblTrajPos_.setVisible(false);          sldTrajPos_.setVisible(false);
    lblTrajMode_.setVisible(false);
    btnTrajModeAuto_.setVisible(false);     btnTrajModeManual_.setVisible(false);

    lblFlowRadius_.setVisible(false);       sldFlowRadius_.setVisible(false);
    lblFlowWidth_.setVisible(false);        sldFlowWidth_.setVisible(false);
    lblFlowStrength_.setVisible(false);     sldFlowStrength_.setVisible(false);
    lblFlowChirality_.setVisible(false);    sldFlowChirality_.setVisible(false);

    lblEmitTraj_.setVisible(false);         sldEmitTraj_.setVisible(false);

    const bool hasSelection = selection_.valid();
    btnRemove_.setVisible(hasSelection);

    if (!hasSelection)
    {
        lblPanelHeader_.setText("No Selection", juce::dontSendNotification);
        return;
    }

    const auto& state = processor_.getPhysicsStateForUI();
    ignoreSliderCallbacks_ = true;

    if (selection_.kind == ObjectKind::TimbralAnchor)
    {
        const int i = selection_.index;
        if (i >= state.activeTimbralAnchorCount) { selection_ = {}; ignoreSliderCallbacks_ = false; return; }

        lblPanelHeader_.setText("Anchor " + juce::String(i), juce::dontSendNotification);

        sldBrightness_.setValue(state.timbralAnchors[i].timbreX, juce::dontSendNotification);
        sldInharmonicity_.setValue(state.timbralAnchors[i].timbreY, juce::dontSendNotification);

        lblBrightness_.setVisible(true);    sldBrightness_.setVisible(true);
        lblInharmonicity_.setVisible(true); sldInharmonicity_.setVisible(true);
    }
    else if (selection_.kind == ObjectKind::FieldObject)
    {
        const int i = selection_.index;
        if (i >= MAX_FIELD_OBJECTS || !state.fieldObjects[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& obj = state.fieldObjects[i];

        juce::String typeName;
        switch (obj.type)
        {
            case FieldObjectType::Attractor: typeName = "Attractor"; break;
            case FieldObjectType::Repeller:  typeName = "Repeller";  break;
            case FieldObjectType::Vortex:    typeName = "Vortex";    break;
        }
        lblPanelHeader_.setText(typeName + " " + juce::String(i), juce::dontSendNotification);

        sldFOStrength_.setValue(obj.strength, juce::dontSendNotification);
        sldFORadius_.setValue  (obj.radius,   juce::dontSendNotification);

        lblFOStrength_.setVisible(true); sldFOStrength_.setVisible(true);
        lblFORadius_.setVisible(true);   sldFORadius_.setVisible(true);

        if (obj.type == FieldObjectType::Vortex)
        {
            sldFOChirality_.setValue(obj.chirality, juce::dontSendNotification);
            lblFOChirality_.setVisible(true);
            sldFOChirality_.setVisible(true);
        }
    }
    else if (selection_.kind == ObjectKind::Emitter)
    {
        const int i = selection_.index;
        if (i >= MAX_EMITTERS || !state.emitters[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& e = state.emitters[i];

        lblPanelHeader_.setText("Emitter " + juce::String(i), juce::dontSendNotification);

        sldKeyLow_.setValue        ((double)e.keyLow,         juce::dontSendNotification);
        sldKeyHigh_.setValue       ((double)e.keyHigh,        juce::dontSendNotification);
        sldTransposeOct_.setValue  ((double)e.transposeOct,   juce::dontSendNotification);
        sldTransposeSemi_.setValue ((double)e.transposeSemi,  juce::dontSendNotification);
        sldTransposeCents_.setValue((double)e.transposeCents, juce::dontSendNotification);
        sldEmitPan_.setValue           ((double)e.pan,                    juce::dontSendNotification);
        sldEmitMass_.setValue          ((double)e.spawnMass,               juce::dontSendNotification);
        sldEmitTraj_.setValue          ((double)e.trajectoryPathIndex,     juce::dontSendNotification);
        btnEmitPoly_  .setToggleState(e.polyMode == PolyMode::Polyphonic, juce::dontSendNotification);
        btnEmitMono_  .setToggleState(e.polyMode == PolyMode::Mono,       juce::dontSendNotification);
        btnEmitLegato_.setToggleState(e.polyMode == PolyMode::Legato,     juce::dontSendNotification);
        btnEmitSlur_  .setToggleState(e.polyMode == PolyMode::LegatoSlur, juce::dontSendNotification);
        btnTerminusEnabled_.setToggleState(e.terminusEnabled,              juce::dontSendNotification);
        sldTerminusStrength_.setValue  ((double)e.terminusStrength,        juce::dontSendNotification);
        sldTerminusRadius_.setValue    ((double)e.terminusArrivalRadius,   juce::dontSendNotification);
        sldEmitAngle_.setValue         (e.launchAngle,                     juce::dontSendNotification);
        sldEmitSpeed_.setValue  (e.launchSpeed,     juce::dontSendNotification);
        sldEmitAttack_.setValue (e.attackTime,   juce::dontSendNotification);
        sldEmitDecay_.setValue  (e.decayTime,    juce::dontSendNotification);
        sldEmitSustain_.setValue(e.sustainLevel, juce::dontSendNotification);
        sldEmitRelease_.setValue(e.releaseTime,  juce::dontSendNotification);

        lblKeyLow_.setVisible(true);           sldKeyLow_.setVisible(true);
        lblKeyHigh_.setVisible(true);          sldKeyHigh_.setVisible(true);
        lblTransposeOct_.setVisible(true);     sldTransposeOct_.setVisible(true);
        lblTransposeSemi_.setVisible(true);    sldTransposeSemi_.setVisible(true);
        lblTransposeCents_.setVisible(true);   sldTransposeCents_.setVisible(true);
        lblEmitPan_.setVisible(true);              sldEmitPan_.setVisible(true);
        lblEmitMass_.setVisible(true);             sldEmitMass_.setVisible(true);
        lblEmitTraj_.setVisible(true);             sldEmitTraj_.setVisible(true);
        lblEmitPolyMode_.setVisible(true);
        btnEmitPoly_.setVisible(true);             btnEmitMono_.setVisible(true);
        btnEmitLegato_.setVisible(true);           btnEmitSlur_.setVisible(true);
        btnTerminusEnabled_.setVisible(true);
        // Strength + radius only visible when Terminus is enabled
        const bool termOn = e.terminusEnabled;
        lblTerminusStrength_.setVisible(termOn); sldTerminusStrength_.setVisible(termOn);
        lblTerminusRadius_.setVisible(termOn);   sldTerminusRadius_.setVisible(termOn);
        lblEmitAngle_.setVisible(true);            sldEmitAngle_.setVisible(true);
        lblEmitSpeed_.setVisible(true);     sldEmitSpeed_.setVisible(true);
        lblEmitAttack_.setVisible(true);    sldEmitAttack_.setVisible(true);
        lblEmitDecay_.setVisible(true);     sldEmitDecay_.setVisible(true);
        lblEmitSustain_.setVisible(true);   sldEmitSustain_.setVisible(true);
        lblEmitRelease_.setVisible(true);   sldEmitRelease_.setVisible(true);
    }
    else if (selection_.kind == ObjectKind::EffectZone)
    {
        const int i = selection_.index;
        if (i >= MAX_EFFECT_ZONES || !state.effectZones[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& z = state.effectZones[i];

        lblPanelHeader_.setText("Zone " + juce::String(i), juce::dontSendNotification);

        sldZoneRadius_.setValue(z.radius, juce::dontSendNotification);

        // Depth slider range depends on target
        if (z.target == ZoneTarget::Pitch)
        {
            sldZoneDepth_.setRange(-24.0, 24.0, 0.0);
            lblZoneDepth_.setText("Depth (semi)", juce::dontSendNotification);
        }
        else
        {
            sldZoneDepth_.setRange(-1.0, 1.0, 0.0);
            lblZoneDepth_.setText("Depth", juce::dontSendNotification);
        }
        sldZoneDepth_.setValue(z.depth, juce::dontSendNotification);

        btnZoneTimbreX_.setToggleState(z.target == ZoneTarget::TimbreX,   juce::dontSendNotification);
        btnZoneTimbreY_.setToggleState(z.target == ZoneTarget::TimbreY,   juce::dontSendNotification);
        btnZoneAmp_    .setToggleState(z.target == ZoneTarget::Amplitude, juce::dontSendNotification);
        btnZonePan_    .setToggleState(z.target == ZoneTarget::Pan,       juce::dontSendNotification);
        btnZonePitch_  .setToggleState(z.target == ZoneTarget::Pitch,     juce::dontSendNotification);

        btnZoneFalloffLinear_  .setToggleState(z.falloff == ZoneFalloff::Linear,   juce::dontSendNotification);
        btnZoneFalloffGaussian_.setToggleState(z.falloff == ZoneFalloff::Gaussian, juce::dontSendNotification);

        lblZoneRadius_.setVisible(true);        sldZoneRadius_.setVisible(true);
        lblZoneDepth_.setVisible(true);         sldZoneDepth_.setVisible(true);
        lblZoneTarget_.setVisible(true);        lblZoneFalloff_.setVisible(true);
        btnZoneTimbreX_.setVisible(true);       btnZoneTimbreY_.setVisible(true);
        btnZoneAmp_.setVisible(true);           btnZonePan_.setVisible(true);
        btnZonePitch_.setVisible(true);
        btnZoneFalloffLinear_.setVisible(true); btnZoneFalloffGaussian_.setVisible(true);
    }
    else if (selection_.kind == ObjectKind::FluxGate)
    {
        const int i = selection_.index;
        if (i >= MAX_FLUX_GATES || !state.fluxGates[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& gate = state.fluxGates[i];

        lblPanelHeader_.setText("Gate " + juce::String(i), juce::dontSendNotification);

        sldGateLength_.setValue(gate.length,   juce::dontSendNotification);
        sldGateAngle_ .setValue(gate.angleRad, juce::dontSendNotification);

        lblGateLength_.setVisible(true); sldGateLength_.setVisible(true);
        lblGateAngle_ .setVisible(true); sldGateAngle_ .setVisible(true);
    }
    else if (selection_.kind == ObjectKind::PathObject)
    {
        const int i = selection_.index;
        if (i >= MAX_PATH_OBJECTS || !state.pathObjects[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& p = state.pathObjects[i];

        lblPanelHeader_.setText("Rail " + juce::String(i), juce::dontSendNotification);

        sldPathRadius_.setValue(p.radius,      juce::dontSendNotification);
        sldPathSnap_  .setValue(p.snapRadius,  juce::dontSendNotification);
        sldPathEscape_.setValue(p.escapeForce, juce::dontSendNotification);

        lblPathRadius_.setVisible(true); sldPathRadius_.setVisible(true);
        lblPathSnap_  .setVisible(true); sldPathSnap_  .setVisible(true);
        lblPathEscape_.setVisible(true); sldPathEscape_.setVisible(true);
    }
    else if (selection_.kind == ObjectKind::TrajectoryPath)
    {
        const int i = selection_.index;
        if (i >= MAX_TRAJECTORY_PATHS || !state.trajectoryPaths[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& tp = state.trajectoryPaths[i];

        lblPanelHeader_.setText("Traj " + juce::String(i), juce::dontSendNotification);

        sldTrajRadius_.setValue(tp.radius,   juce::dontSendNotification);
        sldTrajSpeed_ .setValue(tp.speed,    juce::dontSendNotification);
        sldTrajPos_   .setValue(tp.currentT, juce::dontSendNotification);

        const bool autoMode   = (tp.mode == TrajectoryMode::AutoPlay);
        const bool manualMode = (tp.mode == TrajectoryMode::Manual);
        btnTrajModeAuto_  .setToggleState(autoMode,   juce::dontSendNotification);
        btnTrajModeManual_.setToggleState(manualMode, juce::dontSendNotification);

        lblTrajRadius_.setVisible(true); sldTrajRadius_.setVisible(true);
        lblTrajMode_  .setVisible(true);
        btnTrajModeAuto_  .setVisible(true);
        btnTrajModeManual_.setVisible(true);
        // Speed visible only in AutoPlay; Position visible only in Manual.
        lblTrajSpeed_.setVisible(autoMode);   sldTrajSpeed_.setVisible(autoMode);
        lblTrajPos_  .setVisible(manualMode); sldTrajPos_  .setVisible(manualMode);
    }
    else if (selection_.kind == ObjectKind::TangentPath)
    {
        const int i = selection_.index;
        if (i >= MAX_TANGENT_PATHS || !state.tangentPaths[i].active)
        {
            selection_ = {}; ignoreSliderCallbacks_ = false; return;
        }
        const auto& tp = state.tangentPaths[i];

        lblPanelHeader_.setText("Flow " + juce::String(i), juce::dontSendNotification);

        sldFlowRadius_   .setValue(tp.radius,    juce::dontSendNotification);
        sldFlowWidth_    .setValue(tp.width,     juce::dontSendNotification);
        sldFlowStrength_ .setValue(tp.strength,  juce::dontSendNotification);
        sldFlowChirality_.setValue(tp.chirality, juce::dontSendNotification);

        lblFlowRadius_.setVisible(true);    sldFlowRadius_.setVisible(true);
        lblFlowWidth_.setVisible(true);     sldFlowWidth_.setVisible(true);
        lblFlowStrength_.setVisible(true);  sldFlowStrength_.setVisible(true);
        lblFlowChirality_.setVisible(true); sldFlowChirality_.setVisible(true);
    }

    ignoreSliderCallbacks_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendEdit — convenience wrapper; dispatches a ManifoldEdit to physics thread
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::sendEdit(ManifoldEdit::Type type, int index, float x, float y)
{
    if (index < 0) return;   // Guard against stale selection_
    ManifoldEdit e;
    e.type  = type;
    e.index = index;
    e.x     = x;
    e.y     = y;
    processor_.pushManifoldEdit(e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Placement mode helpers
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::clearPlacementMode()
{
    pendingSpawn_ = SpawnKind::None;
    btnAddAtt_ .setToggleState(false, juce::dontSendNotification);
    btnAddRep_ .setToggleState(false, juce::dontSendNotification);
    btnAddVor_ .setToggleState(false, juce::dontSendNotification);
    btnAddEmit_.setToggleState(false, juce::dontSendNotification);
    btnAddAnch_.setToggleState(false, juce::dontSendNotification);
    btnAddZone_.setToggleState(false, juce::dontSendNotification);
    btnAddFlux_.setToggleState(false, juce::dontSendNotification);
    btnAddPath_.setToggleState(false, juce::dontSendNotification);
    btnAddTraj_.setToggleState(false, juce::dontSendNotification);
    btnAddFlow_.setToggleState(false, juce::dontSendNotification);
}

juce::Colour MorphosEditor::pendingSpawnColour() const noexcept
{
    switch (pendingSpawn_)
    {
        case SpawnKind::Attractor:      return Colour::Attractor;
        case SpawnKind::Repeller:       return Colour::Repeller;
        case SpawnKind::Vortex:         return Colour::Vortex;
        case SpawnKind::Emitter:        return Colour::Emitter;
        case SpawnKind::TimbralAnchor:  return Colour::Anchor;
        case SpawnKind::EffectZone:     return Colour::ZoneTimbreX;
        case SpawnKind::FluxGate:       return Colour::FluxGate;
        case SpawnKind::PathObject:     return Colour::PathObject;
        case SpawnKind::TrajectoryPath: return Colour::TrajPath;
        case SpawnKind::TangentPath:    return Colour::FlowPath;
        default:                        return juce::Colours::white;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard — Delete/Backspace removes the selected object
// ─────────────────────────────────────────────────────────────────────────────

bool MorphosEditor::keyPressed(const juce::KeyPress& key)
{
    if (pendingSpawn_ != SpawnKind::None)
    {
        if (key == juce::KeyPress::escapeKey)
        {
            clearPlacementMode();
            repaint();
            return true;
        }
    }

    if (selection_.valid()
        && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
    {
        ManifoldEdit::Type t;
        switch (selection_.kind)
        {
            case ObjectKind::FieldObject:   t = ManifoldEdit::Type::RemoveFieldObject;   break;
            case ObjectKind::Emitter:       t = ManifoldEdit::Type::RemoveEmitter;       break;
            case ObjectKind::TimbralAnchor: t = ManifoldEdit::Type::RemoveTimbralAnchor; break;
            case ObjectKind::EffectZone:    t = ManifoldEdit::Type::RemoveEffectZone;    break;
            case ObjectKind::FluxGate:      t = ManifoldEdit::Type::RemoveFluxGate;      break;
            case ObjectKind::PathObject:    t = ManifoldEdit::Type::RemovePathObject;    break;
            case ObjectKind::TrajectoryPath:t = ManifoldEdit::Type::RemoveTrajectoryPath;break;
            case ObjectKind::TangentPath:   t = ManifoldEdit::Type::RemoveTangentPath;   break;
            default: return false;
        }
        sendEdit(t, selection_.index, 0.0f, 0.0f);
        selection_ = {};
        drag_.active = false;
        updatePanel();
        repaint();
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer — update trails and trigger repaint
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::timerCallback()
{
    const auto& state = processor_.getPhysicsStateForUI();

    for (int i = 0; i < MAX_MORPHONS; ++i)
    {
        const auto& m = state.morphons[i];
        if (m.active)
            trails_[i].push(m.x, m.y);
        else
            trails_[i].clear();
    }

    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// Mouse — drag-and-drop Manifold objects
// ─────────────────────────────────────────────────────────────────────────────

MorphosEditor::Selection MorphosEditor::hitTest(juce::Point<float> canvasPt,
                                                 const PhysicsStateSnapshot& state,
                                                 juce::Rectangle<int> canvas) const
{
    constexpr float HIT_PX = 14.0f;   // Pixel hit radius for all object types

    // Priority 1: TimbralAnchors (smallest glyph — need first crack to be selectable)
    for (int i = 0; i < state.activeTimbralAnchorCount; ++i)
    {
        const auto& a = state.timbralAnchors[i];
        if (!a.active) continue;
        if (canvasPt.getDistanceFrom(manifoldToCanvas(a.x, a.y, canvas)) <= HIT_PX)
            return { ObjectKind::TimbralAnchor, i };
    }

    // Priority 2: Emitters
    for (int i = 0; i < MAX_EMITTERS; ++i)
    {
        const auto& e = state.emitters[i];
        if (!e.active) continue;
        if (canvasPt.getDistanceFrom(manifoldToCanvas(e.x, e.y, canvas)) <= HIT_PX)
            return { ObjectKind::Emitter, i };
    }

    // Priority 3: Field objects
    for (int i = 0; i < MAX_FIELD_OBJECTS; ++i)
    {
        const auto& obj = state.fieldObjects[i];
        if (!obj.active) continue;
        if (canvasPt.getDistanceFrom(manifoldToCanvas(obj.x, obj.y, canvas)) <= HIT_PX)
            return { ObjectKind::FieldObject, i };
    }

    // Priority 4: Effect zones (hit on centre glyph only, not the influence circle)
    for (int i = 0; i < MAX_EFFECT_ZONES; ++i)
    {
        const auto& z = state.effectZones[i];
        if (!z.active) continue;
        if (canvasPt.getDistanceFrom(manifoldToCanvas(z.x, z.y, canvas)) <= HIT_PX)
            return { ObjectKind::EffectZone, i };
    }

    // Priority 5 & 6: Path / Trajectory rings — hit-test in manifold space so
    // the grabbable curve coincides with the rendered ellipse on non-square
    // canvases. HIT_PX is converted to a manifold-space tolerance using the
    // *less compressed* axis; that gives the user at least HIT_PX pixels of
    // hit zone in either dimension regardless of canvas aspect.
    const auto  cursorMfd      = canvasToManifold(canvasPt, canvas);
    const float minPxPerMfd    = (float)juce::jmin(canvas.getWidth(), canvas.getHeight());
    const float hitMfd         = (minPxPerMfd > 0.0f) ? (HIT_PX / minPxPerMfd) : 0.0f;

    for (int i = 0; i < MAX_PATH_OBJECTS; ++i)
    {
        const auto& p = state.pathObjects[i];
        if (!p.active) continue;

        const float dx   = cursorMfd.x - p.x;
        const float dy   = cursorMfd.y - p.y;
        const float dMfd = std::sqrt(dx * dx + dy * dy);
        if (std::abs(dMfd - p.radius) <= hitMfd)
            return { ObjectKind::PathObject, i };
    }

    for (int i = 0; i < MAX_TRAJECTORY_PATHS; ++i)
    {
        const auto& tp = state.trajectoryPaths[i];
        if (!tp.active) continue;

        const float dx   = cursorMfd.x - tp.x;
        const float dy   = cursorMfd.y - tp.y;
        const float dMfd = std::sqrt(dx * dx + dy * dy);
        if (std::abs(dMfd - tp.radius) <= hitMfd)
            return { ObjectKind::TrajectoryPath, i };
    }

    for (int i = 0; i < MAX_TANGENT_PATHS; ++i)
    {
        const auto& tp = state.tangentPaths[i];
        if (!tp.active) continue;

        const float dx   = cursorMfd.x - tp.x;
        const float dy   = cursorMfd.y - tp.y;
        const float dMfd = std::sqrt(dx * dx + dy * dy);
        if (std::abs(dMfd - tp.radius) <= hitMfd)
            return { ObjectKind::TangentPath, i };
    }

    // Priority 6: Flux gates — hit on the line segment itself, using point-to-
    // segment distance so the whole tripwire is grabbable (not just the centre).
    for (int i = 0; i < MAX_FLUX_GATES; ++i)
    {
        const auto& g = state.fluxGates[i];
        if (!g.active) continue;

        const float half = g.length * 0.5f;
        const auto a = manifoldToCanvas(g.x - std::cos(g.angleRad) * half,
                                        g.y - std::sin(g.angleRad) * half, canvas);
        const auto b = manifoldToCanvas(g.x + std::cos(g.angleRad) * half,
                                        g.y + std::sin(g.angleRad) * half, canvas);
        // Standard point-to-segment distance: project onto AB, clamp to [0,1].
        const float dx  = b.x - a.x;
        const float dy  = b.y - a.y;
        const float len2 = dx * dx + dy * dy;
        const float t   = (len2 > 1e-6f)
            ? juce::jlimit(0.0f, 1.0f,
                ((canvasPt.x - a.x) * dx + (canvasPt.y - a.y) * dy) / len2)
            : 0.0f;
        const float cx = a.x + dx * t;
        const float cy = a.y + dy * t;
        const float d  = std::sqrt((canvasPt.x - cx) * (canvasPt.x - cx)
                                 + (canvasPt.y - cy) * (canvasPt.y - cy));
        if (d <= HIT_PX)
            return { ObjectKind::FluxGate, i };
    }

    return { ObjectKind::None, -1 };
}

void MorphosEditor::mouseDown(const juce::MouseEvent& event)
{
    const auto canvas = getCanvasBounds();
    if (!canvas.toFloat().contains(event.getPosition().toFloat()))
        return;

    const auto& state = processor_.getPhysicsStateForUI();
    const auto hit    = hitTest(event.getPosition().toFloat(), state, canvas);

    // ── Placement mode — click on empty canvas to spawn the armed object type ─
    if (pendingSpawn_ != SpawnKind::None && !hit.valid())
    {
        const auto mpt = canvasToManifold(event.getPosition().toFloat(), canvas);
        const float mx = juce::jlimit(0.0f, 1.0f, mpt.x);
        const float my = juce::jlimit(0.0f, 1.0f, mpt.y);

        ManifoldEdit::Type addType;
        switch (pendingSpawn_)
        {
            case SpawnKind::Attractor:     addType = ManifoldEdit::Type::AddAttractor;     break;
            case SpawnKind::Repeller:      addType = ManifoldEdit::Type::AddRepeller;      break;
            case SpawnKind::Vortex:        addType = ManifoldEdit::Type::AddVortex;        break;
            case SpawnKind::Emitter:       addType = ManifoldEdit::Type::AddEmitter;       break;
            case SpawnKind::TimbralAnchor: addType = ManifoldEdit::Type::AddTimbralAnchor; break;
            case SpawnKind::EffectZone:    addType = ManifoldEdit::Type::AddEffectZone;    break;
            case SpawnKind::FluxGate:      addType = ManifoldEdit::Type::AddFluxGate;      break;
            case SpawnKind::PathObject:    addType = ManifoldEdit::Type::AddPathObject;    break;
            case SpawnKind::TrajectoryPath:addType = ManifoldEdit::Type::AddTrajectoryPath;break;
            case SpawnKind::TangentPath:   addType = ManifoldEdit::Type::AddTangentPath;   break;
            default: clearPlacementMode(); return;
        }

        sendEdit(addType, 0, mx, my);
        clearPlacementMode();
        repaint();
        return;
    }

    // If armed but user clicked an existing object: cancel placement mode and select it
    if (pendingSpawn_ != SpawnKind::None && hit.valid())
        clearPlacementMode();

    const bool selectionChanged = (hit.kind != selection_.kind || hit.index != selection_.index);
    selection_ = hit;

    if (hit.valid())
    {
        drag_.active = true;
        // Seed drag position from snapshot so first drag renders correctly
        switch (hit.kind)
        {
            case ObjectKind::TimbralAnchor:
                drag_.pendingX = state.timbralAnchors[hit.index].x;
                drag_.pendingY = state.timbralAnchors[hit.index].y;
                break;
            case ObjectKind::Emitter:
                drag_.pendingX = state.emitters[hit.index].x;
                drag_.pendingY = state.emitters[hit.index].y;
                break;
            case ObjectKind::FieldObject:
                drag_.pendingX = state.fieldObjects[hit.index].x;
                drag_.pendingY = state.fieldObjects[hit.index].y;
                break;
            case ObjectKind::EffectZone:
                drag_.pendingX = state.effectZones[hit.index].x;
                drag_.pendingY = state.effectZones[hit.index].y;
                break;
            case ObjectKind::FluxGate:
                drag_.pendingX = state.fluxGates[hit.index].x;
                drag_.pendingY = state.fluxGates[hit.index].y;
                break;
            case ObjectKind::PathObject:
                drag_.pendingX = state.pathObjects[hit.index].x;
                drag_.pendingY = state.pathObjects[hit.index].y;
                break;
            case ObjectKind::TrajectoryPath:
                drag_.pendingX = state.trajectoryPaths[hit.index].x;
                drag_.pendingY = state.trajectoryPaths[hit.index].y;
                break;
            case ObjectKind::TangentPath:
                drag_.pendingX = state.tangentPaths[hit.index].x;
                drag_.pendingY = state.tangentPaths[hit.index].y;
                break;
            default: break;
        }

        // Record cursor→centre offset so mouseDrag preserves the relative grip
        // point. Without this, the object's centre snapped to the cursor on the
        // first drag event — fine for small dots, but disorienting for gates
        // grabbed near an endpoint or zones grabbed off-centre.
        const auto clickMfd = canvasToManifold(event.getPosition().toFloat(), canvas);
        drag_.offsetX = drag_.pendingX - clickMfd.x;
        drag_.offsetY = drag_.pendingY - clickMfd.y;
    }
    else
    {
        drag_.active = false;
    }

    if (selectionChanged)
        updatePanel();

    repaint();
}

void MorphosEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (!drag_.active || !selection_.valid()) return;

    const auto canvas     = getCanvasBounds();
    const auto manifoldPt = canvasToManifold(event.getPosition().toFloat(), canvas);

    // Apply the cursor→centre offset captured at mouseDown so the object
    // follows the cursor while preserving where the user grabbed it.
    drag_.pendingX = juce::jlimit(0.0f, 1.0f, manifoldPt.x + drag_.offsetX);
    drag_.pendingY = juce::jlimit(0.0f, 1.0f, manifoldPt.y + drag_.offsetY);

    ManifoldEdit::Type moveType;
    switch (selection_.kind)
    {
        case ObjectKind::TimbralAnchor: moveType = ManifoldEdit::Type::MoveTimbralAnchor; break;
        case ObjectKind::Emitter:       moveType = ManifoldEdit::Type::MoveEmitter;        break;
        case ObjectKind::FieldObject:   moveType = ManifoldEdit::Type::MoveFieldObject;    break;
        case ObjectKind::EffectZone:    moveType = ManifoldEdit::Type::MoveEffectZone;     break;
        case ObjectKind::FluxGate:      moveType = ManifoldEdit::Type::MoveFluxGate;       break;
        case ObjectKind::PathObject:    moveType = ManifoldEdit::Type::MovePathObject;     break;
        case ObjectKind::TrajectoryPath:moveType = ManifoldEdit::Type::MoveTrajectoryPath; break;
        case ObjectKind::TangentPath:   moveType = ManifoldEdit::Type::MoveTangentPath;    break;
        default: return;
    }

    sendEdit(moveType, selection_.index, drag_.pendingX, drag_.pendingY);
    repaint();
}

void MorphosEditor::mouseUp(const juce::MouseEvent&)
{
    drag_.active = false;
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// paint
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::paint(juce::Graphics& g)
{
    g.fillAll(Colour::Background);

    const auto canvas = getCanvasBounds();
    const auto& state = processor_.getPhysicsStateForUI();

    drawGrid            (g, canvas);

    // ── Placement mode indicator ───────────────────────────────────────────────
    if (pendingSpawn_ != SpawnKind::None)
    {
        const juce::Colour c = pendingSpawnColour();

        // Coloured canvas border (2 px)
        g.setColour(c.withAlpha(0.55f));
        g.drawRect(canvas, 2);

        // "Click to place: Type" hint in top-left corner of canvas
        static const char* spawnNames[] = {
            "", "Attractor", "Repeller", "Vortex", "Emitter", "Anchor", "Zone", "Gate", "Rail", "Trajectory", "Flow"
        };
        g.setFont(juce::FontOptions(11.0f));
        g.setColour(c.withAlpha(0.80f));
        g.drawText(juce::String("Click to place: ") + spawnNames[static_cast<int>(pendingSpawn_)],
                   canvas.reduced(6).removeFromTop(16),
                   juce::Justification::topLeft, false);
    }

    drawEffectZones     (g, state, canvas);    // Behind all other objects
    drawPathObjects     (g, state, canvas);    // Above zones
    drawTangentPaths    (g, state, canvas);    // Above rail paths
    drawTrajectoryPaths (g, state, canvas);    // Above flow paths
    drawFluxGates       (g, state, canvas);    // Above paths, beneath fields
    drawFieldObjects    (g, state, canvas);
    drawEmitters        (g, state, canvas);
    drawTimbralAnchors  (g, state, canvas);
    drawTrails          (g, canvas);
    drawMorphons        (g, state, canvas);
    drawPanelBackground (g);
    drawStatusBar       (g, state);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — grid
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawGrid(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    g.setColour(Colour::GridLine);

    const auto  cf        = canvas.toFloat();
    const int   DIVISIONS = 8;
    for (int i = 0; i <= DIVISIONS; ++i)
    {
        const float t = static_cast<float>(i) / DIVISIONS;
        const float x = cf.getX() + t * cf.getWidth();
        const float y = cf.getY() + t * cf.getHeight();
        g.drawVerticalLine  (static_cast<int>(x), cf.getY(), cf.getBottom());
        g.drawHorizontalLine(static_cast<int>(y), cf.getX(), cf.getRight());
    }

    // Canvas border
    g.setColour(Colour::GridLine.brighter(0.3f));
    g.drawRect(canvas, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — Effect zones
//
// Drawn behind all other canvas objects so they don't obscure glyphs.
// Each zone is a dashed circle (colour-coded by target) with a small centre
// glyph carrying a single letter identifying the target type.
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawEffectZones(juce::Graphics& g,
                                    const PhysicsStateSnapshot& state,
                                    juce::Rectangle<int> canvas) const
{
    static const char* targetLabels[] = { "X", "Y", "A", "P", "F" };

    for (int i = 0; i < MAX_EFFECT_ZONES; ++i)
    {
        const auto& z = state.effectZones[i];
        if (!z.active) continue;

        float px = z.x, py = z.y;
        if (drag_.active && selection_.kind == ObjectKind::EffectZone && selection_.index == i)
        {
            px = drag_.pendingX;
            py = drag_.pendingY;
        }

        const auto         centre = manifoldToCanvas(px, py, canvas);
        // Manifold-space circle → canvas ellipse; see drawFieldObjects note.
        const float        rxPx   = z.radius * canvas.getWidth();
        const float        ryPx   = z.radius * canvas.getHeight();
        const juce::Colour c      = zoneColour(z.target);

        // Influence fill — very transparent to stay out of the way
        g.setColour(c.withAlpha(0.07f));
        g.fillEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f);

        // Dashed border
        {
            juce::Path circlePath;
            circlePath.addEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f);
            float dashes[] = { 6.0f, 4.0f };
            juce::Path dashedPath;
            juce::PathStrokeType(1.4f).createDashedStroke(dashedPath, circlePath, dashes, 2);
            g.setColour(c.withAlpha(0.45f));
            g.fillPath(dashedPath);
        }

        // Centre glyph — filled circle
        constexpr float GLYPH_R = 6.0f;
        g.setColour(c.withAlpha(0.85f));
        g.fillEllipse(centre.x - GLYPH_R, centre.y - GLYPH_R,
                      GLYPH_R * 2.0f, GLYPH_R * 2.0f);

        // Target letter (X/Y/A/P/F)
        g.setColour(juce::Colours::black.withAlpha(0.80f));
        g.setFont(juce::FontOptions(8.0f));
        g.drawText(juce::String(targetLabels[static_cast<int>(z.target)]),
                   juce::Rectangle<float>(centre.x - 5.0f, centre.y - 4.5f, 10.0f, 9.0f),
                   juce::Justification::centred, false);

        // Selection ring
        if (selection_.kind == ObjectKind::EffectZone && selection_.index == i)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.8f));
            g.drawEllipse(centre.x - GLYPH_R - 3.0f, centre.y - GLYPH_R - 3.0f,
                          (GLYPH_R + 3.0f) * 2.0f, (GLYPH_R + 3.0f) * 2.0f, 1.5f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — path objects (rail curve + snap-zone fill + direction arrowhead)
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawPathObjects(juce::Graphics& g,
                                    const PhysicsStateSnapshot& state,
                                    juce::Rectangle<int> canvas) const
{
    const juce::Colour c = Colour::PathObject;

    for (int i = 0; i < MAX_PATH_OBJECTS; ++i)
    {
        const auto& p = state.pathObjects[i];
        if (!p.active) continue;

        float cx = p.x, cy = p.y;
        if (drag_.active && selection_.kind == ObjectKind::PathObject && selection_.index == i)
        {
            cx = drag_.pendingX;
            cy = drag_.pendingY;
        }

        const auto  centre = manifoldToCanvas(cx, cy, canvas);
        // Manifold-space circle → canvas ellipse so the rendering matches the
        // physics on non-square canvases (a Morphon pinned to a manifold-radius
        // r curve traces this ellipse in canvas pixels).
        const float rxPx = p.radius     * canvas.getWidth();
        const float ryPx = p.radius     * canvas.getHeight();
        const float sxPx = p.snapRadius * canvas.getWidth();
        const float syPx = p.snapRadius * canvas.getHeight();

        // Snap zone — translucent annulus between the outer and inner ellipses
        // (each radius offset by ±snap on its own axis).
        {
            const float outerX = rxPx + sxPx;
            const float outerY = ryPx + syPx;
            const float innerX = juce::jmax(0.0f, rxPx - sxPx);
            const float innerY = juce::jmax(0.0f, ryPx - syPx);
            juce::Path annulus;
            annulus.addEllipse(centre.x - outerX, centre.y - outerY,
                               outerX * 2.0f, outerY * 2.0f);
            juce::Path hole;
            hole.addEllipse(centre.x - innerX, centre.y - innerY,
                            innerX * 2.0f, innerY * 2.0f);
            annulus.setUsingNonZeroWinding(false);
            annulus.addPath(hole);
            g.setColour(c.withAlpha(0.10f));
            g.fillPath(annulus);
        }

        // Rail line itself (ellipse)
        g.setColour(c.withAlpha(0.85f));
        g.drawEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f, 2.0f);

        // Direction arrowhead at the right-most point (manifold angle 0), pointing
        // CCW (downward). x offsets by rxPx so the head sits on the rendered ellipse;
        // y stays on centre because the manifold-tangent at angle 0 is (0, +1)
        // which maps to canvas-tangent (0, +height/||·||) — still straight down.
        {
            const float ax = centre.x + rxPx;
            const float ay = centre.y;
            constexpr float AR = 5.0f;   // arrowhead size
            juce::Path arrow;
            arrow.startNewSubPath(ax,        ay + AR);          // tip (CCW direction = +Y)
            arrow.lineTo        (ax - AR,    ay);               // back-left
            arrow.lineTo        (ax + AR,    ay);               // back-right
            arrow.closeSubPath();
            g.setColour(c);
            g.fillPath(arrow);
        }

        // Selection ring — thinner re-stroke of the rail
        if (selection_.kind == ObjectKind::PathObject && selection_.index == i)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.70f));
            g.drawEllipse(centre.x - rxPx, centre.y - ryPx,
                          rxPx * 2.0f, ryPx * 2.0f, 1.0f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — trajectory paths (dashed curve + moving t-dot indicating current pos)
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawTrajectoryPaths(juce::Graphics& g,
                                        const PhysicsStateSnapshot& state,
                                        juce::Rectangle<int> canvas) const
{
    const juce::Colour c = Colour::TrajPath;

    for (int i = 0; i < MAX_TRAJECTORY_PATHS; ++i)
    {
        const auto& tp = state.trajectoryPaths[i];
        if (!tp.active) continue;

        float cx = tp.x, cy = tp.y;
        if (drag_.active && selection_.kind == ObjectKind::TrajectoryPath
            && selection_.index == i)
        {
            cx = drag_.pendingX;
            cy = drag_.pendingY;
        }

        const auto  centre = manifoldToCanvas(cx, cy, canvas);
        // Manifold-space circle → canvas ellipse (see drawFieldObjects note).
        // The moving t-dot below samples in manifold space and is mapped via
        // manifoldToCanvas, so it already lies exactly on this ellipse.
        const float rxPx = tp.radius * canvas.getWidth();
        const float ryPx = tp.radius * canvas.getHeight();

        // Dashed ring — distinguishes Trajectory (moves objects) from Rail
        // (constrains Morphons; solid in drawPathObjects).
        {
            juce::Path ring;
            ring.addEllipse(centre.x - rxPx, centre.y - ryPx,
                            rxPx * 2.0f, ryPx * 2.0f);
            float dashes[] = { 7.0f, 5.0f };
            juce::Path dashed;
            juce::PathStrokeType(1.6f).createDashedStroke(dashed, ring, dashes, 2);
            g.setColour(c.withAlpha(0.85f));
            g.fillPath(dashed);
        }

        // Current-T dot — shows where attached objects are currently positioned.
        // Renders the moving head on AutoPlay so the user can read the speed at
        // a glance.
        {
            constexpr float TWO_PI = 6.28318530717958647692f;
            const float angle = tp.currentT * TWO_PI;
            const float hx = cx + std::cos(angle) * tp.radius;
            const float hy = cy + std::sin(angle) * tp.radius;
            const auto headPx = manifoldToCanvas(hx, hy, canvas);
            constexpr float HR = 4.5f;
            g.setColour(c);
            g.fillEllipse(headPx.x - HR, headPx.y - HR, HR * 2.0f, HR * 2.0f);
        }

        // Selection ring
        if (selection_.kind == ObjectKind::TrajectoryPath && selection_.index == i)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.70f));
            g.drawEllipse(centre.x - rxPx, centre.y - ryPx,
                          rxPx * 2.0f, ryPx * 2.0f, 1.0f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — tangent-force ("Flow") paths
//
// A solid ring (the curve) wrapped in a translucent influence band (annulus of
// half-width `width` per axis). Flow arrowheads at four points around the ring
// show traversal direction (chirality). Distinct from the rail (solid ring, no
// band) and the trajectory path (dashed ring + moving t-dot).
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawTangentPaths(juce::Graphics& g,
                                     const PhysicsStateSnapshot& state,
                                     juce::Rectangle<int> canvas) const
{
    const juce::Colour c = Colour::FlowPath;

    for (int i = 0; i < MAX_TANGENT_PATHS; ++i)
    {
        const auto& tp = state.tangentPaths[i];
        if (!tp.active) continue;

        float cx = tp.x, cy = tp.y;
        if (drag_.active && selection_.kind == ObjectKind::TangentPath
            && selection_.index == i)
        {
            cx = drag_.pendingX;
            cy = drag_.pendingY;
        }

        const auto  centre = manifoldToCanvas(cx, cy, canvas);
        // Manifold-space circle → canvas ellipse (see drawFieldObjects note).
        const float rxPx = tp.radius * canvas.getWidth();
        const float ryPx = tp.radius * canvas.getHeight();
        const float wxPx = tp.width  * canvas.getWidth();
        const float wyPx = tp.width  * canvas.getHeight();

        // Influence band — translucent annulus where the flow force acts, with
        // intensity scaled by strength so a stronger stream reads as more solid.
        {
            const float outerX = rxPx + wxPx;
            const float outerY = ryPx + wyPx;
            const float innerX = juce::jmax(0.0f, rxPx - wxPx);
            const float innerY = juce::jmax(0.0f, ryPx - wyPx);
            juce::Path band;
            band.addEllipse(centre.x - outerX, centre.y - outerY,
                            outerX * 2.0f, outerY * 2.0f);
            juce::Path hole;
            hole.addEllipse(centre.x - innerX, centre.y - innerY,
                            innerX * 2.0f, innerY * 2.0f);
            band.setUsingNonZeroWinding(false);
            band.addPath(hole);
            const float a = 0.06f + 0.10f * juce::jlimit(0.0f, 1.0f, tp.strength * 0.5f);
            g.setColour(c.withAlpha(a));
            g.fillPath(band);
        }

        // Curve ring itself
        g.setColour(c.withAlpha(0.85f));
        g.drawEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f, 1.6f);

        // Flow arrowheads — four around the ring, oriented along the local
        // traversal direction (CCW for chirality > 0, CW for < 0). Direction is
        // derived by sampling a neighbouring parameter so it follows the canvas
        // ellipse correctly on non-square aspect ratios.
        {
            constexpr float TWO_PI = 6.28318530717958647692f;
            const float dir = (tp.chirality >= 0.0f) ? 1.0f : -1.0f;
            constexpr float AR = 5.0f;
            g.setColour(c);
            for (int k = 0; k < 4; ++k)
            {
                const float ang = (float)k * (TWO_PI * 0.25f);
                const float pmx = cx + std::cos(ang) * tp.radius;
                const float pmy = cy + std::sin(ang) * tp.radius;
                const float qmx = cx + std::cos(ang + dir * 0.08f) * tp.radius;
                const float qmy = cy + std::sin(ang + dir * 0.08f) * tp.radius;
                const auto  p   = manifoldToCanvas(pmx, pmy, canvas);
                const auto  q   = manifoldToCanvas(qmx, qmy, canvas);

                float dx = q.x - p.x, dy = q.y - p.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f) continue;
                dx /= len; dy /= len;
                const float nx = -dy, ny = dx;   // perpendicular for the base

                juce::Path arrow;
                arrow.startNewSubPath(p.x + dx * AR,            p.y + dy * AR);            // tip
                arrow.lineTo        (p.x - dx * AR + nx * AR,   p.y - dy * AR + ny * AR);  // base 1
                arrow.lineTo        (p.x - dx * AR - nx * AR,   p.y - dy * AR - ny * AR);  // base 2
                arrow.closeSubPath();
                g.fillPath(arrow);
            }
        }

        // Selection ring
        if (selection_.kind == ObjectKind::TangentPath && selection_.index == i)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.70f));
            g.drawEllipse(centre.x - rxPx, centre.y - ryPx,
                          rxPx * 2.0f, ryPx * 2.0f, 1.0f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — flux gates (line segment + endpoint dots; selection halo on whole line)
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawFluxGates(juce::Graphics& g,
                                  const PhysicsStateSnapshot& state,
                                  juce::Rectangle<int> canvas) const
{
    const juce::Colour c = Colour::FluxGate;

    for (int i = 0; i < MAX_FLUX_GATES; ++i)
    {
        const auto& gate = state.fluxGates[i];
        if (!gate.active) continue;

        float cx = gate.x, cy = gate.y;
        if (drag_.active && selection_.kind == ObjectKind::FluxGate && selection_.index == i)
        {
            cx = drag_.pendingX;
            cy = drag_.pendingY;
        }

        const float half = gate.length * 0.5f;
        const auto a = manifoldToCanvas(cx - std::cos(gate.angleRad) * half,
                                        cy - std::sin(gate.angleRad) * half, canvas);
        const auto b = manifoldToCanvas(cx + std::cos(gate.angleRad) * half,
                                        cy + std::sin(gate.angleRad) * half, canvas);

        // Selection halo — wide, low-alpha line under the main stroke
        if (selection_.kind == ObjectKind::FluxGate && selection_.index == i)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.55f));
            g.drawLine(a.x, a.y, b.x, b.y, 5.0f);
        }

        // Main line
        g.setColour(c.withAlpha(0.85f));
        g.drawLine(a.x, a.y, b.x, b.y, 2.0f);

        // Endpoint dots so the gate's extent reads at a glance
        constexpr float ER = 3.0f;
        g.setColour(c);
        g.fillEllipse(a.x - ER, a.y - ER, ER * 2.0f, ER * 2.0f);
        g.fillEllipse(b.x - ER, b.y - ER, ER * 2.0f, ER * 2.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — field objects
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawFieldObjects(juce::Graphics& g,
                                     const PhysicsStateSnapshot& state,
                                     juce::Rectangle<int> canvas) const
{
    for (int i = 0; i < MAX_FIELD_OBJECTS; ++i)
    {
        const auto& obj = state.fieldObjects[i];
        if (!obj.active) continue;

        juce::Colour c;
        switch (obj.type)
        {
            case FieldObjectType::Attractor: c = Colour::Attractor; break;
            case FieldObjectType::Repeller:  c = Colour::Repeller;  break;
            case FieldObjectType::Vortex:    c = Colour::Vortex;    break;
        }

        // If this object is being dragged, use the pending position
        float px = obj.x, py = obj.y;
        if (drag_.active && selection_.kind == ObjectKind::FieldObject && selection_.index == i)
        {
            px = drag_.pendingX;
            py = drag_.pendingY;
        }

        const auto  centre = manifoldToCanvas(px, py, canvas);
        // Manifold-space circle of radius r maps to a canvas-space ellipse with
        // semi-axes (r * canvasWidth, r * canvasHeight). Using width for both
        // axes would draw a pixel-circle that diverges from the physics whenever
        // the canvas isn't square.
        const float rxPx = obj.radius * canvas.getWidth();
        const float ryPx = obj.radius * canvas.getHeight();

        // Influence halo
        g.setColour(c.withAlpha(0.07f));
        g.fillEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f);

        // Rim
        g.setColour(c.withAlpha(0.25f));
        g.drawEllipse(centre.x - rxPx, centre.y - ryPx, rxPx * 2.0f, ryPx * 2.0f, 1.0f);

        // Centre glyph
        constexpr float GLYPH_R = 7.0f;
        g.setColour(c.withAlpha(0.85f));
        g.fillEllipse(centre.x - GLYPH_R, centre.y - GLYPH_R,
                      GLYPH_R * 2.0f, GLYPH_R * 2.0f);

        // Selection ring
        const bool selected = (selection_.kind == ObjectKind::FieldObject && selection_.index == i);
        if (selected)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.8f));
            g.drawEllipse(centre.x - GLYPH_R - 3.0f, centre.y - GLYPH_R - 3.0f,
                          (GLYPH_R + 3.0f) * 2.0f, (GLYPH_R + 3.0f) * 2.0f, 1.5f);
        }

        // Vortex: curved spin arrow indicating rotation direction
        if (obj.type == FieldObjectType::Vortex)
        {
            constexpr float ARROW_R    = 14.0f;
            constexpr float HEAD_LEN   =  5.0f;
            constexpr float HEAD_ANGLE =  2.44f;  // ~140° — wide open arrowhead

            // chirality > 0 → CW in screen coords (Y-down); < 0 → CCW
            const float cw = (obj.chirality >= 0.0f) ? 1.0f : -1.0f;

            // ── JUCE addArc coordinate convention ────────────────────────────
            // angle 0 = 12 o'clock (top), increases clockwise.
            // Position on arc: x = cx + R·sinθ,  y = cy − R·cosθ
            //   (NOT standard trig: x = cx + R·cosθ, y = cy + R·sinθ)
            //
            // 270° arc from top (0):
            //   CW  → toAngle =  3π/2  → left  (9 o'clock); from < to → CW in JUCE
            //   CCW → toAngle = −3π/2  → right (3 o'clock); from > to → CCW in JUCE
            const float startAngle = 0.0f;
            const float endAngle   = cw * 1.5f * juce::MathConstants<float>::pi;

            juce::Path arc;
            arc.addArc(centre.x - ARROW_R, centre.y - ARROW_R,
                       ARROW_R * 2.0f, ARROW_R * 2.0f,
                       startAngle, endAngle, true);

            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.strokePath(arc, juce::PathStrokeType(1.5f));

            // Arrowhead — use JUCE position formula, not standard trig
            const float tipX = centre.x + ARROW_R * std::sin(endAngle);
            const float tipY = centre.y - ARROW_R * std::cos(endAngle);

            // CW tangent at θ: d/dθ(sinθ, −cosθ) = (cosθ, sinθ) in screen coords.
            // CCW: negate. Combined: (cw·cosθ, cw·sinθ).
            const float motionAngle = std::atan2(cw * std::sin(endAngle),
                                                 cw * std::cos(endAngle));
            g.drawLine(tipX, tipY,
                       tipX + std::cos(motionAngle + HEAD_ANGLE) * HEAD_LEN,
                       tipY + std::sin(motionAngle + HEAD_ANGLE) * HEAD_LEN, 1.5f);
            g.drawLine(tipX, tipY,
                       tipX + std::cos(motionAngle - HEAD_ANGLE) * HEAD_LEN,
                       tipY + std::sin(motionAngle - HEAD_ANGLE) * HEAD_LEN, 1.5f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — Emitters
//
// Amber circle with an outward arrow showing launch angle/speed.
// Drawn above field objects so it's never obscured.
//
// BACKBURNER — field-line visualisation:
//   Draw streamlines by tracing the precomputed FieldGrid from a grid of seed
//   points (e.g. 24×24), advancing each N steps via FieldGrid::sample().
//   FieldGrid::sample() is O(1) bilinear — 576 seeds × 60 steps = 34,560
//   lookups per frame, well within budget on the UI thread.
//   Threading concern: FieldGrid is rebuilt on the physics thread when dirty.
//   For Phase 3+ (user edits field objects), either double-buffer the grid
//   or render into an Image on the physics thread after rebuild and hand the
//   Image to the UI via the snapshot.
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawEmitters(juce::Graphics& g,
                                 const PhysicsStateSnapshot& state,
                                 juce::Rectangle<int> canvas) const
{
    for (int i = 0; i < MAX_EMITTERS; ++i)
    {
        const auto& e = state.emitters[i];
        if (!e.active) continue;

        // Use pending drag position if this emitter is being dragged
        float px = e.x, py = e.y;
        if (drag_.active && selection_.kind == ObjectKind::Emitter && selection_.index == i)
        {
            px = drag_.pendingX;
            py = drag_.pendingY;
        }

        const auto centre = manifoldToCanvas(px, py, canvas);

        // Outer ring — distinguishes emitter from field-object glyphs
        constexpr float RING_R = 11.0f;
        g.setColour(Colour::Emitter.withAlpha(0.30f));
        g.drawEllipse(centre.x - RING_R, centre.y - RING_R,
                      RING_R * 2.0f, RING_R * 2.0f, 1.2f);

        // Filled centre dot
        constexpr float GLYPH_R = 5.0f;
        g.setColour(Colour::Emitter.withAlpha(0.88f));
        g.fillEllipse(centre.x - GLYPH_R, centre.y - GLYPH_R,
                      GLYPH_R * 2.0f, GLYPH_R * 2.0f);

        // Selection ring
        const bool selected = (selection_.kind == ObjectKind::Emitter && selection_.index == i);
        if (selected)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.8f));
            g.drawEllipse(centre.x - RING_R - 3.0f, centre.y - RING_R - 3.0f,
                          (RING_R + 3.0f) * 2.0f, (RING_R + 3.0f) * 2.0f, 1.5f);
        }

        // Terminus arrival radius — faint ring shows the zone where Terminus activates.
        // Drawn below the selection ring so it doesn't obscure the glyph itself.
        if (e.terminusEnabled)
        {
            const float arrivalR = e.terminusArrivalRadius
                                   * static_cast<float>(canvas.getWidth());
            g.setColour(Colour::Emitter.withAlpha(0.35f));
            g.drawEllipse(centre.x - arrivalR, centre.y - arrivalR,
                          arrivalR * 2.0f, arrivalR * 2.0f, 1.0f);
        }

        // Launch-direction arrow (only when launchSpeed > 0)
        if (e.launchSpeed > 0.001f)
        {
            // Arrow length scales with speed; minimum 12 px, max ~26 px
            const float arrowLen = 12.0f + std::min(e.launchSpeed, 0.5f) * 28.0f;
            const float tipX = centre.x + std::cos(e.launchAngle) * arrowLen;
            const float tipY = centre.y + std::sin(e.launchAngle) * arrowLen;

            g.setColour(Colour::Emitter.withAlpha(0.70f));
            g.drawLine(centre.x, centre.y, tipX, tipY, 1.5f);

            // Small arrowhead: two short lines at ±140° from the tip
            constexpr float HEAD_LEN   = 5.0f;
            constexpr float HEAD_ANGLE = 2.44f;  // ~140°
            const float a = e.launchAngle + HEAD_ANGLE;
            const float b = e.launchAngle - HEAD_ANGLE;
            g.drawLine(tipX, tipY, tipX + std::cos(a) * HEAD_LEN,
                                   tipY + std::sin(a) * HEAD_LEN, 1.5f);
            g.drawLine(tipX, tipY, tipX + std::cos(b) * HEAD_LEN,
                                   tipY + std::sin(b) * HEAD_LEN, 1.5f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — Timbral Anchors
//
// Diamond (rotated square) glyph in teal. Colour tinted by the anchor's
// own timbre values so anchors are visually distinct from each other:
//   timbreX (brightness) → shifts hue from teal toward amber
//   timbreY (inharmonicity) → dims/saturates the base colour
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawTimbralAnchors(juce::Graphics& g,
                                        const PhysicsStateSnapshot& state,
                                        juce::Rectangle<int> canvas) const
{
    for (int i = 0; i < state.activeTimbralAnchorCount; ++i)
    {
        const auto& a = state.timbralAnchors[i];
        if (!a.active) continue;

        // Use pending drag position if this anchor is being dragged
        float px = a.x, py = a.y;
        if (drag_.active && selection_.kind == ObjectKind::TimbralAnchor && selection_.index == i)
        {
            px = drag_.pendingX;
            py = drag_.pendingY;
        }

        const auto centre = manifoldToCanvas(px, py, canvas);
        const bool selected = (selection_.kind == ObjectKind::TimbralAnchor && selection_.index == i);

        // Tint: blend from teal (dark/harmonic) toward amber (bright/inharmonic)
        const float tintT = a.timbreX * 0.6f + a.timbreY * 0.4f;
        const juce::Colour fillC = Colour::Anchor.interpolatedWith(Colour::Emitter, tintT);

        // Diamond (rotated square)
        constexpr float D = 9.0f;    // half-diagonal in pixels
        juce::Path diamond;
        diamond.startNewSubPath(centre.x,       centre.y - D);
        diamond.lineTo         (centre.x + D,   centre.y);
        diamond.lineTo         (centre.x,       centre.y + D);
        diamond.lineTo         (centre.x - D,   centre.y);
        diamond.closeSubPath();

        // Subtle halo (influence cue)
        constexpr float HALO_D = 18.0f;
        g.setColour(fillC.withAlpha(0.07f));
        g.fillEllipse(centre.x - HALO_D, centre.y - HALO_D, HALO_D * 2.0f, HALO_D * 2.0f);

        // Fill
        g.setColour(fillC.withAlpha(0.88f));
        g.fillPath(diamond);

        // Rim / selection
        if (selected)
        {
            g.setColour(Colour::SelectRing.withAlpha(0.85f));
            g.strokePath(diamond, juce::PathStrokeType(1.8f));

            // Outer selection ring (larger, fainter)
            constexpr float SEL_D = D + 4.0f;
            juce::Path selRing;
            selRing.startNewSubPath(centre.x,        centre.y - SEL_D);
            selRing.lineTo         (centre.x + SEL_D, centre.y);
            selRing.lineTo         (centre.x,         centre.y + SEL_D);
            selRing.lineTo         (centre.x - SEL_D, centre.y);
            selRing.closeSubPath();
            g.setColour(Colour::SelectRing.withAlpha(0.3f));
            g.strokePath(selRing, juce::PathStrokeType(1.0f));
        }
        else
        {
            g.setColour(fillC.brighter(0.4f).withAlpha(0.55f));
            g.strokePath(diamond, juce::PathStrokeType(1.0f));
        }

        // Index digit (centred in diamond)
        g.setColour(juce::Colours::black.withAlpha(0.65f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(juce::String(i),
                   juce::Rectangle<float>(centre.x - 6.0f, centre.y - 5.5f, 12.0f, 11.0f),
                   juce::Justification::centred, false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — trails
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawTrails(juce::Graphics& g, juce::Rectangle<int> canvas) const
{
    for (int i = 0; i < MAX_MORPHONS; ++i)
    {
        const auto& trail = trails_[i];
        if (trail.count < 2) continue;

        for (int j = 1; j < trail.count; ++j)
        {
            const float alpha = static_cast<float>(j) / trail.count;
            g.setColour(Colour::TrailBase.withAlpha(alpha * 0.55f));

            const auto [x0, y0] = trail[j - 1];
            const auto [x1, y1] = trail[j];

            const auto p0 = manifoldToCanvas(x0, y0, canvas);
            const auto p1 = manifoldToCanvas(x1, y1, canvas);

            g.drawLine(p0.x, p0.y, p1.x, p1.y, 1.2f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — Morphon dots
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawMorphons(juce::Graphics& g,
                                 const PhysicsStateSnapshot& state,
                                 juce::Rectangle<int> canvas) const
{
    for (int i = 0; i < MAX_MORPHONS; ++i)
    {
        const auto& m = state.morphons[i];
        if (!m.active) continue;

        const auto centre = manifoldToCanvas(m.x, m.y, canvas);

        // Outer glow, scaled by amplitude
        const float glowR = 10.0f + m.amplitude * 4.0f;
        g.setColour(Colour::MorphonDot.withAlpha(m.amplitude * 0.15f));
        g.fillEllipse(centre.x - glowR, centre.y - glowR, glowR * 2.0f, glowR * 2.0f);

        // Solid core — opacity tracks amplitude so envelope stages are visible:
        // dim during attack/release (floor 0.2), bright at peak/sustain (→ 0.9).
        constexpr float CORE_R = 4.5f;
        const float coreAlpha = 0.2f + m.amplitude * 0.7f;
        g.setColour(Colour::MorphonDot.withAlpha(coreAlpha));
        g.fillEllipse(centre.x - CORE_R, centre.y - CORE_R, CORE_R * 2.0f, CORE_R * 2.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — panel background + divider
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawPanelBackground(juce::Graphics& g) const
{
    const auto panel  = getPanelBounds();
    const auto canvas = getCanvasBounds();

    // Panel fill (slightly lighter than canvas)
    g.setColour(Colour::PanelBg);
    g.fillRect(panel);

    // Vertical divider between canvas and panel
    g.setColour(Colour::Divider);
    g.drawVerticalLine(canvas.getRight() + 4,
                       static_cast<float>(panel.getY()),
                       static_cast<float>(panel.getBottom()));

    // Horizontal rule below the spawn row
    const int spawnBottom  = panel.getY() + 4 + 24 + 4;     // top pad + SPAWN_H + gap
    g.setColour(Colour::Divider);
    g.drawHorizontalLine(spawnBottom, static_cast<float>(panel.getX()),
                         static_cast<float>(panel.getRight()));

    // Horizontal rule under the selection header
    const int headerBottom = spawnBottom + 4 + 24 + 6;       // gap + HEADER_H + section gap
    g.drawHorizontalLine(headerBottom, static_cast<float>(panel.getX()),
                         static_cast<float>(panel.getRight()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — status bar
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::drawStatusBar(juce::Graphics& g,
                                  const PhysicsStateSnapshot& state) const
{
    g.setColour(Colour::StatusText);
    g.setFont(juce::FontOptions(12.0f));

    juce::String status;
    status << "Morphos  |  "
           << "tick " << static_cast<juce::int64>(state.tickIndex) << "  |  "
           << "sim " << juce::String(state.simulationTimeMs / 1000.0, 1) << "s  |  "
           << state.activeMorphonCount << " morphon"
           << (state.activeMorphonCount != 1 ? "s" : "") << " active  |  "
           << state.activeFieldObjCount << " field object"
           << (state.activeFieldObjCount != 1 ? "s" : "") << "  |  "
           << state.activeTimbralAnchorCount << " anchor"
           << (state.activeTimbralAnchorCount != 1 ? "s" : "") << "  |  "
           << state.activeEffectZoneCount << " zone"
           << (state.activeEffectZoneCount != 1 ? "s" : "") << "  |  "
           << state.activeFluxGateCount << " gate"
           << (state.activeFluxGateCount != 1 ? "s" : "") << "  |  "
           << state.activePathObjectCount << " rail"
           << (state.activePathObjectCount != 1 ? "s" : "") << "  |  "
           << state.activeTrajectoryPathCount << " traj"
           << (state.activeTrajectoryPathCount != 1 ? "s" : "") << "  |  "
           << state.activeTangentPathCount << " flow"
           << (state.activeTangentPathCount != 1 ? "s" : "");

    const auto statusRect = getLocalBounds()
                                .removeFromBottom(28)
                                .reduced(12, 0);
    g.drawText(status, statusRect, juce::Justification::centredLeft, true);
}
