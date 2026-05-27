#include "PluginEditor.h"

MorphosEditor::MorphosEditor(MorphosProcessor& p)
    : AudioProcessorEditor(&p), processor_(p)
{
    setSize(800, 520);
    setResizable(true, true);
    setResizeLimits(600, 400, 2400, 1600);

    titleLabel_.setText("Morphos", juce::dontSendNotification);
    titleLabel_.setFont(juce::FontOptions(28.0f, juce::Font::plain));
    titleLabel_.setJustificationType(juce::Justification::centred);
    titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFE8E4DC));
    addAndMakeVisible(titleLabel_);

    statusLabel_.setText("Physics thread starting...", juce::dontSendNotification);
    statusLabel_.setFont(juce::FontOptions(13.0f));
    statusLabel_.setJustificationType(juce::Justification::centred);
    statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFF888880));
    addAndMakeVisible(statusLabel_);

    startTimerHz(30); // 30 Hz UI refresh
}

MorphosEditor::~MorphosEditor()
{
    stopTimer();
}

void MorphosEditor::paint(juce::Graphics& g)
{
    // Phase 0 background — a placeholder dark surface.
    // Phase 3+: replaced by the Manifold canvas renderer.
    g.fillAll(juce::Colour(0xFF1A1A18));

    // Faint grid as a visual hint of the future Manifold
    g.setColour(juce::Colour(0xFF252522));
    const int step = 40;
    for (int x = 0; x < getWidth(); x += step)
        g.drawVerticalLine(x, 0.0f, static_cast<float>(getHeight()));
    for (int y = 0; y < getHeight(); y += step)
        g.drawHorizontalLine(y, 0.0f, static_cast<float>(getWidth()));
}

void MorphosEditor::resized()
{
    auto area = getLocalBounds();
    titleLabel_.setBounds(area.removeFromTop(80).reduced(20, 16));
    statusLabel_.setBounds(area.removeFromTop(30).reduced(20, 0));
}

void MorphosEditor::timerCallback()
{
    // Poll the physics snapshot at 30 Hz.
    // In Phase 0 this just reads the tick counter to confirm the thread is running.
    const auto& state = processor_.getPhysicsStateForUI();

    juce::String status;
    status << "Physics tick: " << static_cast<juce::int64>(state.tickIndex)
           << "  |  Sim time: "
           << juce::String(state.simulationTimeMs / 1000.0, 2) << "s"
           << "  |  Active Morphons: " << state.activeMorphonCount;

    statusLabel_.setText(status, juce::dontSendNotification);
}
