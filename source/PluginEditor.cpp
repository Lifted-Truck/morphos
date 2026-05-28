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
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

MorphosEditor::MorphosEditor(MorphosProcessor& p)
    : AudioProcessorEditor(&p), processor_(p)
{
    setSize(900, 600);
    setResizable(true, true);
    setResizeLimits(600, 400, 2400, 1600);

    setupSliders();
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
    const int x = panel.getX();
    const int w = panel.getWidth();

    // ── Spawn row — 5 equal-width buttons ────────────────────────────────────
    {
        const int bw = w / 5;
        btnAddAtt_ .setBounds(x + bw * 0, y, bw, SPAWN_H);
        btnAddRep_ .setBounds(x + bw * 1, y, bw, SPAWN_H);
        btnAddVor_ .setBounds(x + bw * 2, y, bw, SPAWN_H);
        btnAddEmit_.setBounds(x + bw * 3, y, bw, SPAWN_H);
        btnAddAnch_.setBounds(x + bw * 4, y, w - bw * 4, SPAWN_H);
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

    // ── Polyphony row — global voice mode; always visible ─────────────────────
    lblPolyMode_.setBounds(x, y, w, LABEL_H);
    y += LABEL_H;
    {
        const int bw = w / 3;
        btnPoly_  .setBounds(x + bw * 0, y, bw,         BTN_ROW_H);
        btnMono_  .setBounds(x + bw * 1, y, bw,         BTN_ROW_H);
        btnLegato_.setBounds(x + bw * 2, y, w - bw * 2, BTN_ROW_H);
    }
    y += BTN_ROW_H + 4;

    // ── Header: name label + remove button ────────────────────────────────────
    constexpr int REMOVE_W = 20;
    lblPanelHeader_.setBounds(x, y, w - REMOVE_W - 2, HEADER_H);
    btnRemove_     .setBounds(x + w - REMOVE_W, y, REMOVE_W, HEADER_H);
    y += HEADER_H + SECTION_GAP;

    auto layoutRow = [&](juce::Label& lbl, juce::Slider& sld)
    {
        lbl.setBounds(x, y,          w, LABEL_H);
        sld.setBounds(x, y + LABEL_H, w, SLIDER_H);
        y += ROW_H;
    };

    // ── Anchor section ────────────────────────────────────────────────────────
    layoutRow(lblBrightness_,    sldBrightness_);
    layoutRow(lblInharmonicity_, sldInharmonicity_);
    y += SECTION_GAP;

    // ── Field object section ──────────────────────────────────────────────────
    layoutRow(lblFOStrength_,  sldFOStrength_);
    layoutRow(lblFORadius_,    sldFORadius_);
    layoutRow(lblFOChirality_, sldFOChirality_);
    y += SECTION_GAP;

    // ── Emitter section ───────────────────────────────────────────────────────
    layoutRow(lblKeyLow_,         sldKeyLow_);
    layoutRow(lblKeyHigh_,        sldKeyHigh_);
    layoutRow(lblTransposeOct_,   sldTransposeOct_);
    layoutRow(lblTransposeSemi_,  sldTransposeSemi_);
    layoutRow(lblTransposeCents_, sldTransposeCents_);
    layoutRow(lblEmitPan_,        sldEmitPan_);
    layoutRow(lblEmitAngle_,      sldEmitAngle_);
    layoutRow(lblEmitSpeed_,   sldEmitSpeed_);
    layoutRow(lblEmitAttack_,  sldEmitAttack_);
    layoutRow(lblEmitDecay_,   sldEmitDecay_);
    layoutRow(lblEmitSustain_, sldEmitSustain_);
    layoutRow(lblEmitRelease_, sldEmitRelease_);
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

    btnAddAtt_.onClick  = [this]{ sendEdit(ManifoldEdit::Type::AddAttractor,    0, 0.5f, 0.5f); };
    btnAddRep_.onClick  = [this]{ sendEdit(ManifoldEdit::Type::AddRepeller,     0, 0.5f, 0.5f); };
    btnAddVor_.onClick  = [this]{ sendEdit(ManifoldEdit::Type::AddVortex,       0, 0.5f, 0.5f); };
    btnAddEmit_.onClick = [this]{ sendEdit(ManifoldEdit::Type::AddEmitter,      0, 0.5f, 0.5f); };
    btnAddAnch_.onClick = [this]{ sendEdit(ManifoldEdit::Type::AddTimbralAnchor,0, 0.5f, 0.5f); };

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

    // ── Global polyphony row ────────────────────────────────────────────────────
    styleLabel(lblPolyMode_, "Voices");
    addAndMakeVisible(lblPolyMode_);

    styleBoundBtn(btnPoly_);    addAndMakeVisible(btnPoly_);
    styleBoundBtn(btnMono_);    addAndMakeVisible(btnMono_);
    styleBoundBtn(btnLegato_);  addAndMakeVisible(btnLegato_);

    btnPoly_.setToggleState(true, juce::dontSendNotification);  // Default: Polyphonic

    auto polyClick = [this](PolyMode p)
    {
        sendEdit(ManifoldEdit::Type::SetPolyMode, 0,
                 static_cast<float>(static_cast<uint8_t>(p)));
        btnPoly_  .setToggleState(p == PolyMode::Polyphonic, juce::dontSendNotification);
        btnMono_  .setToggleState(p == PolyMode::Mono,       juce::dontSendNotification);
        btnLegato_.setToggleState(p == PolyMode::Legato,      juce::dontSendNotification);
    };

    btnPoly_  .onClick = [polyClick]{ polyClick(PolyMode::Polyphonic); };
    btnMono_  .onClick = [polyClick]{ polyClick(PolyMode::Mono);       };
    btnLegato_.onClick = [polyClick]{ polyClick(PolyMode::Legato);     };
}

// ─────────────────────────────────────────────────────────────────────────────
// updatePanel — refresh slider visibility + values from current snapshot
// Call on selection change; NOT in the timer (sliders only reflect state at
// time of selection — they are not live-updated by physics output).
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::updatePanel()
{
    // ── Always-visible: global topology + polyphony buttons ──────────────────
    {
        const auto& state = processor_.getPhysicsStateForUI();

        const auto b = state.globalBoundary;
        btnBoundWrap_     .setToggleState(b == BoundaryBehavior::Wrap,        juce::dontSendNotification);
        btnBoundReflect_  .setToggleState(b == BoundaryBehavior::Reflect,     juce::dontSendNotification);
        btnBoundTerminate_.setToggleState(b == BoundaryBehavior::Terminate,   juce::dontSendNotification);
        btnBoundKlein_    .setToggleState(b == BoundaryBehavior::KleinBottle, juce::dontSendNotification);

        const auto p = state.globalPolyMode;
        btnPoly_  .setToggleState(p == PolyMode::Polyphonic, juce::dontSendNotification);
        btnMono_  .setToggleState(p == PolyMode::Mono,       juce::dontSendNotification);
        btnLegato_.setToggleState(p == PolyMode::Legato,      juce::dontSendNotification);
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
    lblEmitPan_.setVisible(false);         sldEmitPan_.setVisible(false);
    lblEmitAngle_.setVisible(false);       sldEmitAngle_.setVisible(false);
    lblEmitSpeed_.setVisible(false);     sldEmitSpeed_.setVisible(false);
    lblEmitAttack_.setVisible(false);    sldEmitAttack_.setVisible(false);
    lblEmitDecay_.setVisible(false);     sldEmitDecay_.setVisible(false);
    lblEmitSustain_.setVisible(false);   sldEmitSustain_.setVisible(false);
    lblEmitRelease_.setVisible(false);   sldEmitRelease_.setVisible(false);

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
        sldEmitPan_.setValue       ((double)e.pan,            juce::dontSendNotification);
        sldEmitAngle_.setValue     (e.launchAngle,            juce::dontSendNotification);
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
        lblEmitPan_.setVisible(true);          sldEmitPan_.setVisible(true);
        lblEmitAngle_.setVisible(true);        sldEmitAngle_.setVisible(true);
        lblEmitSpeed_.setVisible(true);     sldEmitSpeed_.setVisible(true);
        lblEmitAttack_.setVisible(true);    sldEmitAttack_.setVisible(true);
        lblEmitDecay_.setVisible(true);     sldEmitDecay_.setVisible(true);
        lblEmitSustain_.setVisible(true);   sldEmitSustain_.setVisible(true);
        lblEmitRelease_.setVisible(true);   sldEmitRelease_.setVisible(true);
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

    return { ObjectKind::None, -1 };
}

void MorphosEditor::mouseDown(const juce::MouseEvent& event)
{
    const auto canvas = getCanvasBounds();
    if (!canvas.toFloat().contains(event.getPosition().toFloat()))
        return;

    const auto& state = processor_.getPhysicsStateForUI();
    const auto hit    = hitTest(event.getPosition().toFloat(), state, canvas);

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
            default: break;
        }
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

    drag_.pendingX = juce::jlimit(0.0f, 1.0f, manifoldPt.x);
    drag_.pendingY = juce::jlimit(0.0f, 1.0f, manifoldPt.y);

    ManifoldEdit::Type moveType;
    switch (selection_.kind)
    {
        case ObjectKind::TimbralAnchor: moveType = ManifoldEdit::Type::MoveTimbralAnchor; break;
        case ObjectKind::Emitter:       moveType = ManifoldEdit::Type::MoveEmitter;        break;
        case ObjectKind::FieldObject:   moveType = ManifoldEdit::Type::MoveFieldObject;    break;
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

        const auto  centre      = manifoldToCanvas(px, py, canvas);
        const float screenRadius = obj.radius * canvas.getWidth();

        // Influence halo
        g.setColour(c.withAlpha(0.07f));
        g.fillEllipse(centre.x - screenRadius, centre.y - screenRadius,
                      screenRadius * 2.0f, screenRadius * 2.0f);

        // Rim
        g.setColour(c.withAlpha(0.25f));
        g.drawEllipse(centre.x - screenRadius, centre.y - screenRadius,
                      screenRadius * 2.0f, screenRadius * 2.0f, 1.0f);

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
           << (state.activeTimbralAnchorCount != 1 ? "s" : "");

    const auto statusRect = getLocalBounds()
                                .removeFromBottom(28)
                                .reduced(12, 0);
    g.drawText(status, statusRect, juce::Justification::centredLeft, true);
}
