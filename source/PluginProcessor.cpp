#include "PluginProcessor.h"
#include "PluginEditor.h"

// ─────────────────────────────────────────────────────────────────────────────
// Parameter layout — define all automatable parameters here.
// IDs must match ParamID:: constants exactly and must never change.
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorValueTreeState::ParameterLayout
MorphosProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ ParamID::MASTER_GAIN, 1 },
        "Master Gain",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        0.8f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ ParamID::GLOBAL_TIME_SCALE, 1 },
        "Global Time Scale",
        juce::NormalisableRange<float>(0.05f, 8.0f, 0.0f, 0.5f), // skewed: finer control near 1.0
        1.0f));

    // Phase 4+: per-Emitter params, field object params, etc.

    return layout;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

MorphosProcessor::MorphosProcessor()
    : AudioProcessor(BusesProperties()
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "MorphosState", createParameterLayout())
{
    // Cache raw atomic pointers — safe to dereference on the audio thread
    // without going through APVTS (which acquires a lock).
    pGain_            = apvts_.getRawParameterValue(ParamID::MASTER_GAIN);
    pGlobalTimeScale_ = apvts_.getRawParameterValue(ParamID::GLOBAL_TIME_SCALE);

    jassert(pGain_            != nullptr);
    jassert(pGlobalTimeScale_ != nullptr);

    physicsEngine_.startSimulation();
}

MorphosProcessor::~MorphosProcessor()
{
    physicsEngine_.stopSimulation();
}

// ─────────────────────────────────────────────────────────────────────────────
// Prepare / release
// ─────────────────────────────────────────────────────────────────────────────

void MorphosProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    sampleRate_ = sampleRate;
    // Phase 2+: initialise per-Morphon synthesis engines here.
}

void MorphosProcessor::releaseResources()
{
    // Phase 2+: release synthesis resources.
}

// ─────────────────────────────────────────────────────────────────────────────
// processBlock — audio thread, called at buffer rate (~100–350 Hz)
//
// Real-time safety rules enforced here:
//   NO heap allocation, NO mutex, NO system calls that can block.
// ─────────────────────────────────────────────────────────────────────────────

void MorphosProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer&         midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // ── 1. Silence output (Phase 0: no synthesis yet) ─────────────────────────
    buffer.clear();

    // ── 2. Push MIDI note events to physics thread ────────────────────────────
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();

        if (msg.isNoteOn())
        {
            physicsEngine_.pushNoteEvent({
                NoteEvent::Type::NoteOn,
                msg.getChannel(),
                msg.getNoteNumber(),
                msg.getVelocity()
            });
        }
        else if (msg.isNoteOff())
        {
            physicsEngine_.pushNoteEvent({
                NoteEvent::Type::NoteOff,
                msg.getChannel(),
                msg.getNoteNumber(),
                0
            });
        }
    }

    // ── 3. Forward global time scale to physics ───────────────────────────────
    physicsEngine_.setGlobalTimeScale(
        pGlobalTimeScale_->load(std::memory_order_relaxed));

    // ── 4. Grab latest physics snapshot ──────────────────────────────────────
    const auto& snapshot = physicsEngine_.getLatestState();

    // ── 5. Synthesis  →  Phase 2+ ─────────────────────────────────────────────
    // For each active Morphon: read snapshot.morphons[i], drive synthesis engine,
    // accumulate into buffer. Nothing here yet.
    (void)snapshot;

    // ── 6. Apply master gain ──────────────────────────────────────────────────
    const float gain = pGain_->load(std::memory_order_relaxed);
    buffer.applyGain(gain);

    // ── 7. Copy snapshot for UI thread (relaxed — UI only needs approximate data)
    latestSnapshotForUI_ = snapshot;
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* MorphosProcessor::createEditor()
{
    return new MorphosEditor(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// State serialisation
//
// Two layers:
//   A. APVTS parameters — handled automatically by ValueTree serialisation.
//   B. Manifold object data (Anchors, Emitters, field objects, Paths…)
//      — custom XML appended as a child of the state tree.
//      Currently empty; the structure is established so the format is stable.
// ─────────────────────────────────────────────────────────────────────────────

void MorphosProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts_.copyState();

    // Append Manifold object data as a child element.
    // Phase 3+: populate this with Anchor positions, Emitter configs, etc.
    auto manifoldData = juce::ValueTree("ManifoldObjects");
    // manifoldData.appendChild(..., nullptr);  // Phase 3+
    state.appendChild(manifoldData, nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    if (xml)
        copyXmlToBinary(*xml, destData);
}

void MorphosProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (!xmlState)
        return;

    auto loadedState = juce::ValueTree::fromXml(*xmlState);
    if (!loadedState.isValid())
        return;

    // Restore APVTS parameters
    if (loadedState.hasType(apvts_.state.getType()))
        apvts_.replaceState(loadedState);

    // Restore Manifold object data
    auto manifoldData = loadedState.getChildWithName("ManifoldObjects");
    if (manifoldData.isValid())
    {
        // Phase 3+: reconstruct Anchors, Emitters, field objects from manifoldData.
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Plugin factory — required by JUCE
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphosProcessor();
}
