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
        v.prevPartialAmps.fill(0.0f);
        v.prevAmplitude = 0.0f;
        v.wasActive     = false;
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

            // Detect new spawn (inactive → active): reset phasors and prev state
            if (!voice.wasActive)
            {
                voice.phases.fill(0.0f);
                voice.prevPartialAmps.fill(0.0f);
                voice.prevAmplitude = 0.0f;
                voice.wasActive = true;
            }

            // ── Amplitude ramp — eliminates clicks at buffer boundaries ──────────
            // The physics snapshot is a point-in-time read; amplitude can jump
            // discontinuously between processBlock calls. Lerping from the previous
            // buffer's end amplitude to the current snapshot value smooths the step
            // into a ramp across the buffer, which the ear cannot distinguish.
            const float targetAmp = m.amplitude * VOICE_SCALE;
            const float startAmp  = voice.prevAmplitude;
            voice.prevAmplitude   = targetAmp;  // Store for next buffer

            if (targetAmp < 1e-5f && startAmp < 1e-5f) continue;  // Fully silent

            const float ampStep = (targetAmp - startAmp)
                                  / static_cast<float>(std::max(numSamples, 1));

            // ── Timbral parameters (written by physics thread via anchor blending + zones) ─
            // timbreX = spectral rolloff [0..1]: 0 = dark, 1 = bright
            // timbreY = inharmonicity    [0..1]: 0 = harmonic, 1 = stretched partials
            const float rolloffExp   = 3.5f - m.timbreX * 3.2f;  // [0,1] → [3.5, 0.3]
            const float stretchCoeff = 0.012f * m.timbreY;        // partial k stretches by k*coeff

            // Pitch zone shift (semitones accumulated by applyEffectZones); applied
            // multiplicatively so it doesn't corrupt the glide-target fundamentalHz.
            const float pitchMult = (m.pitchZoneSemitones != 0.0f)
                ? std::pow(2.0f, m.pitchZoneSemitones / 12.0f)
                : 1.0f;

            // ── Precompute per-partial frequency and normalised shape (once per buffer)
            // std::pow is called 20× per voice here, not 20× per sample.
            float partialFreqs[NUM_PARTIALS];
            float partialAmps [NUM_PARTIALS];
            float ampSum = 0.0f;

            for (int k = 1; k <= NUM_PARTIALS; ++k)
            {
                // Inharmonic partial: f_k = f0 * pitchMult * k * (1 + k * stretch)
                partialFreqs[k - 1] = m.fundamentalHz
                                      * pitchMult
                                      * static_cast<float>(k)
                                      * (1.0f + static_cast<float>(k) * stretchCoeff);

                // Spectral rolloff: a_k = 1 / k^rolloffExp
                const float pa = 1.0f / std::pow(static_cast<float>(k), rolloffExp);
                partialAmps[k - 1] = pa;
                ampSum += pa;
            }

            // Normalise spectral shape to unit sum; amplitude applied per-sample below.
            const float normFactor = (ampSum > 0.0f) ? 1.0f / ampSum : 0.0f;
            for (int k = 0; k < NUM_PARTIALS; ++k)
                partialAmps[k] *= normFactor;

            // ── Sample loop: advance phasors, accumulate into all output channels ─
            // Cache write pointers and pan coefficients outside the sample loop.
            constexpr int MAX_CH = 8;
            float* channelPtrs[MAX_CH] = {};
            const int clampedCh = std::min(numChannels, MAX_CH);
            for (int ch = 0; ch < clampedCh; ++ch)
                channelPtrs[ch] = buffer.getWritePointer(ch);

            // Pan is set at spawn and never changes mid-note — hoist trig outside.
            const float panAngle = (m.pan + 1.0f) * (juce::MathConstants<float>::pi * 0.25f);
            const float panL     = std::cos(panAngle);
            const float panR     = std::sin(panAngle);

            // Reciprocal buffer length for spectral lerp fraction.
            const float invN = 1.0f / static_cast<float>(std::max(numSamples, 1));

            for (int s = 0; s < numSamples; ++s)
            {
                // Per-sample amplitude: linear ramp from startAmp to targetAmp
                const float amp = startAmp + ampStep * static_cast<float>(s);

                // Lerp spectral shape from previous buffer's shape to this buffer's
                // shape.  Prevents clicks when timbreX/Y jump discontinuously at a
                // Manifold boundary crossing (Wrap / Klein bottle).  At steady state
                // prevPartialAmps == partialAmps, so the lerp is a no-op.
                const float tFrac = static_cast<float>(s) * invN;

                float sample = 0.0f;

                for (int k = 0; k < NUM_PARTIALS; ++k)
                {
                    voice.phases[k] += partialFreqs[k] * invSampleRate;
                    if (voice.phases[k] >= 1.0f) voice.phases[k] -= 1.0f;
                    const float pa = voice.prevPartialAmps[k]
                                   + (partialAmps[k] - voice.prevPartialAmps[k]) * tFrac;
                    sample += pa * std::sin(voice.phases[k] * twoPi);
                }

                sample *= amp;

                // Equal-power pan: θ maps pan [-1,+1] → [0, π/2]; L=cos(θ), R=sin(θ)
                if (clampedCh >= 2)
                {
                    channelPtrs[0][s] += sample * panL;
                    channelPtrs[1][s] += sample * panR;
                }
                else
                {
                    for (int ch = 0; ch < clampedCh; ++ch)
                        channelPtrs[ch][s] += sample;
                }
            }

            // Store spectral shape for next buffer — enables smooth lerp on the
            // next call even if timbreX/Y change significantly (e.g. boundary wrap).
            for (int k = 0; k < NUM_PARTIALS; ++k)
                voice.prevPartialAmps[k] = partialAmps[k];
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
