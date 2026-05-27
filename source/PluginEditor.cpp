#include "PluginEditor.h"

// ─────────────────────────────────────────────────────────────────────────────
// Palette — colours matching the design spec object taxonomy
// ─────────────────────────────────────────────────────────────────────────────
namespace Colour
{
    static const juce::Colour Background  { 0xFF1A1A18 };
    static const juce::Colour GridLine    { 0xFF252522 };
    static const juce::Colour Attractor   { 0xFF378ADD };
    static const juce::Colour Repeller    { 0xFFE24B4A };
    static const juce::Colour Vortex      { 0xFF7F77DD };
    static const juce::Colour MorphonDot  { 0xFFE8E4DC };
    static const juce::Colour TrailBase   { 0xFFB0AB9E };
    static const juce::Colour StatusText  { 0xFF888880 };
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
    // Leave 28px at the bottom for the status bar; 8px margin all around.
    return getLocalBounds().reduced(8).withTrimmedBottom(28);
}

juce::Point<float> MorphosEditor::manifoldToCanvas(float mx, float my,
                                                    juce::Rectangle<int> canvas) const
{
    // Manifold (0,0) → top-left of canvas, (1,1) → bottom-right.
    return {
        canvas.getX() + mx * canvas.getWidth(),
        canvas.getY() + my * canvas.getHeight()
    };
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
// paint
// ─────────────────────────────────────────────────────────────────────────────

void MorphosEditor::paint(juce::Graphics& g)
{
    g.fillAll(Colour::Background);

    const auto canvas = getCanvasBounds();
    const auto& state = processor_.getPhysicsStateForUI();

    drawGrid        (g, canvas);
    drawFieldObjects(g, state, canvas);
    drawTrails      (g, canvas);
    drawMorphons    (g, state, canvas);
    drawStatusBar   (g, state);
}

void MorphosEditor::resized()
{
    // Phase 3+: lay out the canvas + parameter panel here.
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
        g.drawVerticalLine  (static_cast<int>(x), canvas.getY(), canvas.getBottom());
        g.drawHorizontalLine(static_cast<int>(y), canvas.getX(), canvas.getRight());
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
            default:                         c = Colour::MorphonDot; break;
        }

        const auto  centre      = manifoldToCanvas(obj.x, obj.y, canvas);
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

        // Vortex: draw a small chirality arrow
        if (obj.type == FieldObjectType::Vortex)
        {
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            // Simple "+" centre marker — arrow drawn in Phase 3+
            g.drawLine(centre.x - 3.5f, centre.y, centre.x + 3.5f, centre.y, 1.5f);
            g.drawLine(centre.x, centre.y - 3.5f, centre.x, centre.y + 3.5f, 1.5f);
        }
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

        // Solid core
        constexpr float CORE_R = 4.5f;
        g.setColour(Colour::MorphonDot.withAlpha(0.9f));
        g.fillEllipse(centre.x - CORE_R, centre.y - CORE_R, CORE_R * 2.0f, CORE_R * 2.0f);
    }
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
           << (state.activeFieldObjCount != 1 ? "s" : "");

    const auto statusRect = getLocalBounds()
                                .removeFromBottom(28)
                                .reduced(12, 0);
    g.drawText(status, statusRect, juce::Justification::centredLeft, true);
}
