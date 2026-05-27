#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

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

    // Reset all voice state (phasors and wasActive flags) so stale phases
    // from a previous session don't cause clicks on first note.
    for (auto& v : voices_)
    {
        v.phases.fill(0.0f);
        v.wasActive = false;
    }
}

void MorphosProcessor::releaseResources()
{
    // Nothing to release — additive voice state is POD, lives inline.
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

    // ── 5. Additive synthesis ─────────────────────────────────────────────────
    // Per-voice: read timbre params (pre-blended by physics thread), accumulate
    // additive partials into the output buffer.
    //
    // Real-time safety: no heap allocation. Stack arrays (NUM_PARTIALS floats) are
    // fixed-size. std::pow is called once per voice per buffer (not per sample).
    {
        const int   numSamples    = buffer.getNumSamples();
        const int   numChannels   = buffer.getNumChannels();
        const float invSampleRate = 1.0f / static_cast<float>(sampleRate_);
        const float twoPi         = juce::MathConstants<float>::twoPi;

        // Per-voice gain: scale so a single full-amplitude voice peaks around -12 dBFS.
        // Master gain (APVTS) applied to the accumulated mix after this loop.
        constexpr float VOICE_SCALE = 0.12f;

        for (int i = 0; i < MAX_MORPHONS; ++i)
        {
            const auto& m     = snapshot.morphons[i];
            auto&       voice = voices_[i];

            if (!m.active)
            {
                voice.wasActive = false;
                continue;
            }

            // Detect new spawn (inactive → active): reset phase accumulators
            if (!voice.wasActive)
            {
                voice.phases.fill(0.0f);
                voice.wasActive = true;
            }

            const float amplitude = m.amplitude * VOICE_SCALE;
            if (amplitude < 1e-5f) continue;  // Skip inaudible voices

            // ── Timbral parameters (written by physics thread via anchor blending) ─
            // timbreX = spectral rolloff [0..1]: 0 = dark, 1 = bright
            // timbreY = inharmonicity    [0..1]: 0 = harmonic, 1 = stretched partials
            const float rolloffExp   = 3.5f - m.timbreX * 3.2f;  // [0,1] → [3.5, 0.3]
            const float stretchCoeff = 0.012f * m.timbreY;        // partial k stretches by k*coeff

            // ── Precompute per-partial frequency and amplitude (once per buffer) ──
            // Pull std::pow out of the per-sample loop entirely.
            float partialFreqs[NUM_PARTIALS];
            float partialAmps [NUM_PARTIALS];
            float ampSum = 0.0f;

            for (int k = 1; k <= NUM_PARTIALS; ++k)
            {
                // Inharmonic partial: f_k = f0 * k * (1 + k * stretch)
                partialFreqs[k - 1] = m.fundamentalHz
                                      * static_cast<float>(k)
                                      * (1.0f + static_cast<float>(k) * stretchCoeff);

                // Spectral rolloff: a_k = 1 / k^rolloffExp
                const float pa = 1.0f / std::pow(static_cast<float>(k), rolloffExp);
                partialAmps[k - 1] = pa;
                ampSum += pa;
            }

            // Normalise so total spectral power is constant across rolloff settings,
            // then bake in voice amplitude so the sample loop is multiply-only.
            const float normScale = (ampSum > 0.0f) ? amplitude / ampSum : 0.0f;
            for (int k = 0; k < NUM_PARTIALS; ++k)
                partialAmps[k] *= normScale;

            // ── Sample loop: advance phasors, accumulate into all output channels ─
            // Cache write pointers outside the sample loop.
            constexpr int MAX_CH = 8;
            float* channelPtrs[MAX_CH] = {};
            const int clampedCh = std::min(numChannels, MAX_CH);
            for (int ch = 0; ch < clampedCh; ++ch)
                channelPtrs[ch] = buffer.getWritePointer(ch);

            for (int s = 0; s < numSamples; ++s)
            {
                float sample = 0.0f;

                for (int k = 0; k < NUM_PARTIALS; ++k)
                {
                    voice.phases[k] += partialFreqs[k] * invSampleRate;
                    if (voice.phases[k] >= 1.0f) voice.phases[k] -= 1.0f;
                    sample += partialAmps[k] * std::sin(voice.phases[k] * twoPi);
                }

                for (int ch = 0; ch < clampedCh; ++ch)
                    channelPtrs[ch][s] += sample;
            }
        }
    }

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
