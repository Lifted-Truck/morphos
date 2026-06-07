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

    // ── Phase-6 macro knobs ──────────────────────────────────────────────────
    // Eight normalised [0, 1] params exposed to the host as standard
    // automation. Display names ("Macro 1" … "Macro 8") are what the DAW shows
    // in its parameter list / macro mapper.
    for (int i = 0; i < 8; ++i)
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{ ParamID::MACRO[i], 1 },
            "Macro " + juce::String(i + 1),
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.0f));

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
    for (int i = 0; i < 8; ++i)
    {
        pMacros_[i] = apvts_.getRawParameterValue(ParamID::MACRO[i]);
        jassert(pMacros_[i] != nullptr);
    }

    jassert(pGain_            != nullptr);
    jassert(pGlobalTimeScale_ != nullptr);

    formatManager_.registerBasicFormats();   // WAV / AIFF (+ FLAC/Ogg if enabled)

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

    for (auto& g : grainVoices_)
        g.reset();
}

void MorphosProcessor::releaseResources()
{
    // Nothing to release — additive voice state is POD, lives inline.
}

// ─────────────────────────────────────────────────────────────────────────────
// Granular source registry (message/UI thread)
// ─────────────────────────────────────────────────────────────────────────────

int MorphosProcessor::loadSampleSource(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels < 1)
        return -1;

    // Find a free source slot.
    int slot = -1;
    for (int i = 0; i < MAX_SOURCES; ++i)
        if (sources_[i].load(std::memory_order_relaxed) == nullptr) { slot = i; break; }
    if (slot < 0) return -1;

    const double srcSR      = reader->sampleRate > 0.0 ? reader->sampleRate : sampleRate_;
    const juce::int64 capSamples = (juce::int64) (60.0 * srcSR);   // slice 1: cap at 60 s
    const int    numSamples = (int) juce::jmin(reader->lengthInSamples, capSamples);
    const int    numCh      = (int) reader->numChannels;
    if (numSamples <= 0) return -1;

    auto src        = std::make_unique<SampleSource>();
    src->id         = slot;
    src->name       = file.getFileNameWithoutExtension();
    src->sampleRate = srcSR;

    // Read the file's channels, then sum to mono.
    juce::AudioBuffer<float> tmp(numCh, numSamples);
    reader->read(tmp.getArrayOfWritePointers(), numCh, 0, numSamples);

    src->mono.setSize(1, numSamples);
    src->mono.clear();
    auto* dst = src->mono.getWritePointer(0);
    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto* s = tmp.getReadPointer(ch);
        for (int n = 0; n < numSamples; ++n) dst[n] += s[n];
    }
    if (numCh > 1)
        src->mono.applyGain(1.0f / static_cast<float>(numCh));

    // Normalise to a target peak so grain level doesn't ride on the file's
    // recording level — a quiet file would otherwise produce quiet grains.
    {
        const float* d = src->mono.getReadPointer(0);
        float peak = 0.0f;
        for (int n = 0; n < numSamples; ++n) peak = juce::jmax(peak, std::abs(d[n]));
        if (peak > 1.0e-6f)
            src->mono.applyGain(0.89f / peak);   // ≈ −1 dBFS
    }

    // Publish: keep ownership here, hand the audio thread an atomic raw pointer.
    SampleSource* raw = src.get();
    sourceStorage_.push_back(std::move(src));
    sources_[slot].store(raw, std::memory_order_release);
    return slot;
}

juce::String MorphosProcessor::getSourceName(int sourceId) const
{
    if (sourceId < 0 || sourceId >= MAX_SOURCES) return {};
    if (auto* src = sources_[sourceId].load(std::memory_order_acquire))
        return src->name;
    return {};
}

std::vector<std::pair<int, juce::String>> MorphosProcessor::getLoadedSources() const
{
    std::vector<std::pair<int, juce::String>> out;
    for (int i = 0; i < MAX_SOURCES; ++i)
        if (auto* src = sources_[i].load(std::memory_order_acquire))
            out.emplace_back(i, src->name);
    return out;
}

const float* MorphosProcessor::getSourceAudio(int sourceId, int& outNumSamples) const
{
    outNumSamples = 0;
    if (sourceId < 0 || sourceId >= MAX_SOURCES) return nullptr;
    if (auto* src = sources_[sourceId].load(std::memory_order_acquire))
        if (src->valid()) { outNumSamples = src->numSamples(); return src->data(); }
    return nullptr;
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
        else if (msg.isController())
        {
            // CC values feed the mod-matrix MidiCC source. The NoteEvent
            // struct reuses `note` as CC number and `velocity` as CC value.
            physicsEngine_.pushNoteEvent({
                NoteEvent::Type::ControlChange,
                msg.getChannel(),
                msg.getControllerNumber(),
                msg.getControllerValue()
            });
        }
    }

    // ── 3. Forward global time scale to physics ───────────────────────────────
    physicsEngine_.setGlobalTimeScale(
        pGlobalTimeScale_->load(std::memory_order_relaxed));

    // Forward host-automated macro knobs so the mod matrix can read them.
    for (int i = 0; i < 8; ++i)
        physicsEngine_.setMacroValue(i,
            pMacros_[i]->load(std::memory_order_relaxed));

    // ── 3a. Offline rendering: drive physics synchronously ───────────────────
    // During DAW bounce/freeze, processBlock runs faster than wall clock. The
    // physics thread is parked and we advance the simulation here by the
    // buffer's virtual duration so bounced audio reflects the same Manifold
    // evolution as real-time playback.
    {
        const bool offline = isNonRealtime();
        physicsEngine_.setOfflineMode(offline);
        if (offline && sampleRate_ > 0.0)
        {
            const double bufferSeconds = (double) buffer.getNumSamples() / sampleRate_;
            physicsEngine_.advance(bufferSeconds);
        }
    }

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

        // Global granular output trim (constant per buffer).
        const float globalGrainLevel = juce::jlimit(0.0f, 2.0f, snapshot.globalGrainLevel);

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
                grainVoices_[i].reset();   // fresh grain scheduler for this voice
            }

            // ── Amplitude ramp — eliminates clicks at buffer boundaries ──────────
            // The physics snapshot is a point-in-time read; amplitude can jump
            // discontinuously between processBlock calls. Lerping from the previous
            // buffer's end amplitude to the current snapshot value smooths the step
            // into a ramp across the buffer, which the ear cannot distinguish.
            // Voice level: envelope × calibration × per-Emitter gain × per-anchor
            // volume field. (Master gain is applied to the whole bus below.)
            const float targetAmp = m.amplitude * VOICE_SCALE * m.gainScale * m.anchorVolume;
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

            // ── Granular setup (slice 1) ─────────────────────────────────────────
            // Resolve the dominant granular source for this Morphon (blended by the
            // physics thread). Pitch is decoupled: playback rate follows MIDI note
            // (note 60 = source's natural pitch) and the source/host SR ratio; scrub
            // position comes from geometry. The granular and additive renders are
            // equal-power crossfaded by m.granularWeight.
            const float* grainSrc       = nullptr;
            int          grainSrcLen    = 0;
            double       grainRate      = 1.0;
            float        grainLenSamp   = 0.0f;
            float        grainSpawnSamp = 0.0f;
            float        grainJitterSamp= 0.0f;
            float        grainSpray     = 0.0f;
            float        granW          = juce::jlimit(0.0f, 1.0f, m.granularWeight);

            if (granW > 1.0e-4f && m.granularSourceId >= 0 && m.granularSourceId < MAX_SOURCES)
            {
                if (auto* src = sources_[m.granularSourceId].load(std::memory_order_acquire))
                {
                    if (src->valid())
                    {
                        grainSrc    = src->data();
                        grainSrcLen = src->numSamples();
                        // Pitch: MIDI note (60 = natural) + per-anchor semitone field + zone pitch.
                        const double srRatio   = src->sampleRate / sampleRate_;
                        const double pitchMul2 = std::pow(2.0, (m.midiNote - 60) / 12.0);
                        const double anchorPit = std::pow(2.0, m.granularPitch / 12.0);
                        grainRate    = srRatio * pitchMul2 * anchorPit * static_cast<double>(pitchMult);
                        // Grain size (per-anchor field, seconds) → window span in samples.
                        grainLenSamp = juce::jmax(1.0f, m.granularGrainSize * static_cast<float>(sampleRate_));
                        // Density [0,1] → overlap factor [0.5, 8]; spawn interval = len / overlap.
                        const float overlap = 0.5f + juce::jlimit(0.0f, 1.0f, m.granularDensity) * 7.5f;
                        grainSpawnSamp = grainLenSamp / overlap;
                        // Jitter spreads the read start over up to ±10% of the buffer.
                        grainJitterSamp = juce::jlimit(0.0f, 1.0f, m.granularJitter) * 0.1f * static_cast<float>(grainSrcLen);
                        grainSpray      = juce::jlimit(0.0f, 1.0f, m.granularSpray);
                    }
                }
            }
            if (grainSrc == nullptr) granW = 0.0f;   // no usable source → no grain power

            // Equal-power group gains: gain = sqrt(weight share). Each render group
            // contributes its own power; groups not rendered (other granular sources
            // in slice 1) simply contribute none — so regions with no additive
            // anchors produce no additive bleed. Granular output is trimmed by the
            // global grain level.
            // GRAIN_MAKEUP compensates the Hann window's ~0.5 average and brings
            // the (now peak-normalised) grain output to parity with additive at
            // default Grain Level. Tunable by ear alongside the global trim.
            constexpr float GRAIN_MAKEUP = 2.0f;
            const float addGain   = std::sqrt(juce::jlimit(0.0f, 1.0f, m.additiveWeight));
            const float grainGain = std::sqrt(granW) * globalGrainLevel * GRAIN_MAKEUP;

            for (int s = 0; s < numSamples; ++s)
            {
                // Per-sample amplitude: linear ramp from startAmp to targetAmp
                const float amp = startAmp + ampStep * static_cast<float>(s);

                // Lerp spectral shape from previous buffer's shape to this buffer's
                // shape.  Prevents clicks when timbreX/Y jump discontinuously at a
                // Manifold boundary crossing (Wrap / Klein bottle).  At steady state
                // prevPartialAmps == partialAmps, so the lerp is a no-op.
                const float tFrac = static_cast<float>(s) * invN;

                float additive = 0.0f;

                for (int k = 0; k < NUM_PARTIALS; ++k)
                {
                    voice.phases[k] += partialFreqs[k] * invSampleRate;
                    if (voice.phases[k] >= 1.0f) voice.phases[k] -= 1.0f;
                    const float pa = voice.prevPartialAmps[k]
                                   + (partialAmps[k] - voice.prevPartialAmps[k]) * tFrac;
                    additive += pa * std::sin(voice.phases[k] * twoPi);
                }

                // Granular render (read-head fixed for this buffer; geometry-driven).
                const float grain = (grainSrc != nullptr)
                    ? grainVoices_[i].process(grainSrc, grainSrcLen, m.granularReadPos,
                                              grainRate, grainLenSamp, grainSpawnSamp,
                                              grainJitterSamp, grainSpray)
                    : 0.0f;

                float sample = (additive * addGain + grain * grainGain) * amp;

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

    // latestSnapshotForUI_ is a relaxed copy written by the audio thread each
    // processBlock() and read by the UI timer — same access pattern as the editor.
    const auto& snap = latestSnapshotForUI_;

    auto manifoldData = juce::ValueTree("ManifoldObjects");
    manifoldData.setProperty("version",     3,                             nullptr);
    manifoldData.setProperty("boundary",    (int)snap.globalBoundary,      nullptr);
    manifoldData.setProperty("glideTime",   snap.globalGlideTime,          nullptr);
    manifoldData.setProperty("friction",    snap.globalFriction,           nullptr);
    manifoldData.setProperty("grainLevel",  snap.globalGrainLevel,         nullptr);
    manifoldData.setProperty("editorW",     editorWidth_,                  nullptr);
    manifoldData.setProperty("editorH",     editorHeight_,                 nullptr);

    for (int i = 0; i < MAX_FIELD_OBJECTS; ++i)
    {
        const auto& fo = snap.fieldObjects[i];
        if (!fo.active) continue;
        auto node = juce::ValueTree("FieldObject");
        node.setProperty("type",      (int)fo.type, nullptr);
        node.setProperty("x",         fo.x,         nullptr);
        node.setProperty("y",         fo.y,         nullptr);
        node.setProperty("strength",  fo.strength,  nullptr);
        node.setProperty("radius",    fo.radius,    nullptr);
        node.setProperty("chirality", fo.chirality, nullptr);
        node.setProperty("trajPath",  fo.trajectoryPathIndex, nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_EMITTERS; ++i)
    {
        const auto& em = snap.emitters[i];
        if (!em.active) continue;
        auto node = juce::ValueTree("Emitter");
        node.setProperty("x",              em.x,                       nullptr);
        node.setProperty("y",              em.y,                       nullptr);
        node.setProperty("launchAngle",    em.launchAngle,              nullptr);
        node.setProperty("launchSpeed",    em.launchSpeed,              nullptr);
        node.setProperty("spawnMass",      em.spawnMass,                nullptr);
        node.setProperty("spawnDrag",      em.spawnDrag,                nullptr);
        node.setProperty("attack",         em.attackTime,               nullptr);
        node.setProperty("decay",          em.decayTime,                nullptr);
        node.setProperty("sustain",        em.sustainLevel,             nullptr);
        node.setProperty("release",        em.releaseTime,              nullptr);
        node.setProperty("keyLow",         em.keyLow,                   nullptr);
        node.setProperty("keyHigh",        em.keyHigh,                  nullptr);
        node.setProperty("transposeOct",   em.transposeOct,             nullptr);
        node.setProperty("transposeSemi",  em.transposeSemi,            nullptr);
        node.setProperty("transposeCents", em.transposeCents,           nullptr);
        node.setProperty("pan",            em.pan,                      nullptr);
        node.setProperty("terminusOn",     em.terminusEnabled ? 1 : 0,  nullptr);
        node.setProperty("terminusStr",    em.terminusStrength,         nullptr);
        node.setProperty("terminusRad",    em.terminusArrivalRadius,    nullptr);
        node.setProperty("polyMode",       (int)em.polyMode,            nullptr);
        node.setProperty("trajPath",       em.trajectoryPathIndex,      nullptr);
        node.setProperty("gain",           em.gain,                     nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < snap.activeTimbralAnchorCount; ++i)
    {
        const auto& a = snap.timbralAnchors[i];
        auto node = juce::ValueTree("Anchor");
        node.setProperty("x",       a.x,       nullptr);
        node.setProperty("y",       a.y,       nullptr);
        node.setProperty("timbreX", a.timbreX, nullptr);
        node.setProperty("timbreY", a.timbreY, nullptr);
        node.setProperty("trajPath", a.trajectoryPathIndex, nullptr);
        // Granular: readPosition always persists; sourceId persists but only
        // rebinds while the source registry still holds it (same session). The
        // registry itself isn't serialised yet (path+embed lands in a later
        // phase), so a stale sourceId degrades gracefully to additive on load.
        node.setProperty("sourceId",     a.sourceId,     nullptr);
        node.setProperty("readPosition", a.readPosition, nullptr);
        node.setProperty("density",      a.density,      nullptr);
        node.setProperty("jitter",       a.jitter,       nullptr);
        node.setProperty("spray",        a.spray,        nullptr);
        node.setProperty("grainSize",    a.grainSize,    nullptr);
        node.setProperty("pitchSemis",   a.pitchSemis,   nullptr);
        node.setProperty("posEnabled",   a.positionEnabled, nullptr);
        node.setProperty("volume",       a.volume,       nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_EFFECT_ZONES; ++i)
    {
        const auto& z = snap.effectZones[i];
        if (!z.active) continue;
        auto node = juce::ValueTree("Zone");
        node.setProperty("x",       z.x,            nullptr);
        node.setProperty("y",       z.y,            nullptr);
        node.setProperty("radius",  z.radius,       nullptr);
        node.setProperty("depth",   z.depth,        nullptr);
        node.setProperty("target",  (int)z.target,  nullptr);
        node.setProperty("falloff", (int)z.falloff, nullptr);
        node.setProperty("trajPath", z.trajectoryPathIndex, nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_FLUX_GATES; ++i)
    {
        const auto& fg = snap.fluxGates[i];
        if (!fg.active) continue;
        auto node = juce::ValueTree("Gate");
        node.setProperty("x",      fg.x,        nullptr);
        node.setProperty("y",      fg.y,        nullptr);
        node.setProperty("shape",  (int)fg.shape, nullptr);
        node.setProperty("length", fg.length,     nullptr);
        node.setProperty("angle",  fg.angleRad,   nullptr);
        node.setProperty("radius", fg.radius,     nullptr);
        node.setProperty("trajPath", fg.trajectoryPathIndex, nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_PATH_OBJECTS; ++i)
    {
        const auto& p = snap.pathObjects[i];
        if (!p.active) continue;
        auto node = juce::ValueTree("Path");
        node.setProperty("shape",       (int)p.shape,    nullptr);
        node.setProperty("x",           p.x,             nullptr);
        node.setProperty("y",           p.y,             nullptr);
        node.setProperty("radius",      p.radius,        nullptr);
        node.setProperty("snapRadius",  p.snapRadius,    nullptr);
        node.setProperty("escapeForce", p.escapeForce,   nullptr);
        node.setProperty("trajPath",    p.trajectoryPathIndex, nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_TRAJECTORY_PATHS; ++i)
    {
        const auto& tp = snap.trajectoryPaths[i];
        if (!tp.active) continue;
        auto node = juce::ValueTree("Traj");
        node.setProperty("shape",    (int)tp.shape, nullptr);
        node.setProperty("x",        tp.x,          nullptr);
        node.setProperty("y",        tp.y,          nullptr);
        node.setProperty("radius",   tp.radius,     nullptr);
        node.setProperty("length",   tp.length,     nullptr);
        node.setProperty("angle",    tp.angleRad,   nullptr);
        node.setProperty("curve",    (int)tp.curve, nullptr);
        node.setProperty("mode",     (int)tp.mode,  nullptr);
        node.setProperty("speed",    tp.speed,      nullptr);
        node.setProperty("currentT", tp.currentT,   nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    for (int i = 0; i < MAX_TANGENT_PATHS; ++i)
    {
        const auto& tp = snap.tangentPaths[i];
        if (!tp.active) continue;
        auto node = juce::ValueTree("Flow");
        node.setProperty("shape",     (int)tp.shape, nullptr);
        node.setProperty("x",         tp.x,          nullptr);
        node.setProperty("y",         tp.y,          nullptr);
        node.setProperty("radius",    tp.radius,     nullptr);
        node.setProperty("width",     tp.width,      nullptr);
        node.setProperty("strength",  tp.strength,   nullptr);
        node.setProperty("chirality", tp.chirality,  nullptr);
        node.setProperty("trajPath",  tp.trajectoryPathIndex, nullptr);
        manifoldData.appendChild(node, nullptr);
    }

    // Mod-matrix connections
    for (int i = 0; i < MAX_MOD_CONNECTIONS; ++i)
    {
        const auto& c = snap.modConnections[i];
        if (!c.active) continue;
        auto node = juce::ValueTree("Mod");
        node.setProperty("srcType",  (int)c.srcType,  nullptr);
        node.setProperty("srcIndex", c.srcIndex,      nullptr);
        node.setProperty("dstType",  (int)c.dstType,  nullptr);
        node.setProperty("dstIndex", c.dstIndex,      nullptr);
        node.setProperty("depth",    c.depth,         nullptr);
        node.setProperty("base",     c.base,          nullptr);
        manifoldData.appendChild(node, nullptr);
    }

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

    if (loadedState.hasType(apvts_.state.getType()))
        apvts_.replaceState(loadedState);

    auto manifoldData = loadedState.getChildWithName("ManifoldObjects");
    if (!manifoldData.isValid())
        return;
    const int patchVersion = (int)manifoldData.getProperty("version", 0);
    if (patchVersion < 1)
        return;

    // Version 1 stored polyMode as a single global. Version 2 moves it onto each
    // Emitter. Loading v1: read the global value and broadcast it to all Emitters
    // so the user's prior voice-mode choice survives.
    const PolyMode v1GlobalPolyMode = (patchVersion < 2)
        ? static_cast<PolyMode>((int)manifoldData.getProperty("polyMode", 0))
        : PolyMode::Polyphonic;

    PatchState patch;
    patch.boundary     = static_cast<BoundaryBehavior>(
                             (int)manifoldData.getProperty("boundary",  0));
    patch.glideTimeSec   = (float)manifoldData.getProperty("glideTime", 0.0f);
    patch.globalFriction   = (float)manifoldData.getProperty("friction",   0.0f);
    patch.globalGrainLevel = (float)manifoldData.getProperty("grainLevel", 1.0f);

    // Window size (v2+). Defaults preserve the current editor size on older saves.
    if (patchVersion >= 2)
    {
        const int w = (int)manifoldData.getProperty("editorW", editorWidth_);
        const int h = (int)manifoldData.getProperty("editorH", editorHeight_);
        editorWidth_  = w;
        editorHeight_ = h;
        if (auto* ed = dynamic_cast<MorphosEditor*>(getActiveEditor()))
            ed->setSize(w, h);
    }

    int fieldObjSlot = 0, emitterSlot = 0, anchorSlot = 0, zoneSlot = 0, gateSlot = 0, pathSlot = 0, trajSlot = 0, flowSlot = 0, modSlot = 0;

    for (int i = 0; i < manifoldData.getNumChildren(); ++i)
    {
        auto child = manifoldData.getChild(i);

        if (child.hasType("FieldObject") && fieldObjSlot < MAX_FIELD_OBJECTS)
        {
            auto& fo     = patch.fieldObjects[fieldObjSlot++];
            fo.type      = static_cast<FieldObjectType>((int)child.getProperty("type",      0));
            fo.x         = (float)child.getProperty("x",         0.5f);
            fo.y         = (float)child.getProperty("y",         0.5f);
            fo.strength  = (float)child.getProperty("strength",  0.25f);
            fo.radius    = (float)child.getProperty("radius",    0.45f);
            fo.chirality = (float)child.getProperty("chirality", 1.0f);
            fo.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            fo.active    = true;
        }
        else if (child.hasType("Emitter") && emitterSlot < MAX_EMITTERS)
        {
            auto& em                 = patch.emitters[emitterSlot++];
            em.x                     = (float)child.getProperty("x",             0.5f);
            em.y                     = (float)child.getProperty("y",             0.5f);
            em.launchAngle           = (float)child.getProperty("launchAngle",   0.0f);
            em.launchSpeed           = (float)child.getProperty("launchSpeed",   0.18f);
            em.spawnMass             = (float)child.getProperty("spawnMass",     1.0f);
            em.spawnDrag             = (float)child.getProperty("spawnDrag",     0.001f);
            em.attackTime            = (float)child.getProperty("attack",        0.05f);
            em.decayTime             = (float)child.getProperty("decay",         0.15f);
            em.sustainLevel          = (float)child.getProperty("sustain",       0.70f);
            em.releaseTime           = (float)child.getProperty("release",       0.35f);
            em.keyLow                = (int)child.getProperty("keyLow",          0);
            em.keyHigh               = (int)child.getProperty("keyHigh",         127);
            em.transposeOct          = (int)child.getProperty("transposeOct",    0);
            em.transposeSemi         = (int)child.getProperty("transposeSemi",   0);
            em.transposeCents        = (float)child.getProperty("transposeCents",0.0f);
            em.pan                   = (float)child.getProperty("pan",           0.0f);
            em.terminusEnabled       = (int)child.getProperty("terminusOn",      0) != 0;
            em.terminusStrength      = (float)child.getProperty("terminusStr",   0.30f);
            em.terminusArrivalRadius = (float)child.getProperty("terminusRad",   0.04f);
            em.polyMode              = (patchVersion >= 2)
                ? static_cast<PolyMode>((int)child.getProperty("polyMode", 0))
                : v1GlobalPolyMode;  // v1 fallback: broadcast the old global value
            em.trajectoryPathIndex   = (int)child.getProperty("trajPath", -1);
            em.gain                  = (float)child.getProperty("gain", 1.0f);
            em.active                = true;
        }
        else if (child.hasType("Anchor") && anchorSlot < MAX_TIMBRAL_ANCHORS)
        {
            auto& a   = patch.timbralAnchors[anchorSlot++];
            a.x       = (float)child.getProperty("x",       0.5f);
            a.y       = (float)child.getProperty("y",       0.5f);
            a.timbreX = (float)child.getProperty("timbreX", 0.5f);
            a.timbreY = (float)child.getProperty("timbreY", 0.0f);
            a.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            a.sourceId     = (int)  child.getProperty("sourceId",     -1);
            a.readPosition = (float)child.getProperty("readPosition", 0.5f);
            a.density      = (float)child.getProperty("density",      0.3f);
            a.jitter       = (float)child.getProperty("jitter",       0.0f);
            a.spray        = (float)child.getProperty("spray",        0.0f);
            a.grainSize    = (float)child.getProperty("grainSize",    0.06f);
            a.pitchSemis   = (float)child.getProperty("pitchSemis",   0.0f);
            a.positionEnabled = (bool)child.getProperty("posEnabled", true);
            a.volume       = (float)child.getProperty("volume",       1.0f);
            a.active  = true;
        }
        else if (child.hasType("Zone") && zoneSlot < MAX_EFFECT_ZONES)
        {
            auto& z   = patch.effectZones[zoneSlot++];
            z.x       = (float)child.getProperty("x",      0.5f);
            z.y       = (float)child.getProperty("y",      0.5f);
            z.radius  = (float)child.getProperty("radius", 0.15f);
            z.depth   = (float)child.getProperty("depth",  0.5f);
            z.target  = static_cast<ZoneTarget>( (int)child.getProperty("target",  0));
            z.falloff = static_cast<ZoneFalloff>((int)child.getProperty("falloff", 1));
            z.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            z.active  = true;
        }
        else if (child.hasType("Gate") && gateSlot < MAX_FLUX_GATES)
        {
            auto& fg    = patch.fluxGates[gateSlot++];
            fg.x        = (float)child.getProperty("x",      0.5f);
            fg.y        = (float)child.getProperty("y",      0.5f);
            fg.shape    = static_cast<FluxGateShape>((int)child.getProperty("shape", 0));
            fg.length   = (float)child.getProperty("length", 0.20f);
            fg.angleRad = (float)child.getProperty("angle",  0.0f);
            fg.radius   = (float)child.getProperty("radius", 0.15f);
            fg.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            fg.active   = true;
        }
        else if (child.hasType("Path") && pathSlot < MAX_PATH_OBJECTS)
        {
            auto& p       = patch.pathObjects[pathSlot++];
            p.shape       = static_cast<PathShape>((int)child.getProperty("shape", 0));
            p.x           = (float)child.getProperty("x",           0.5f);
            p.y           = (float)child.getProperty("y",           0.5f);
            p.radius      = (float)child.getProperty("radius",      0.15f);
            p.snapRadius  = (float)child.getProperty("snapRadius",  0.04f);
            p.escapeForce = (float)child.getProperty("escapeForce", 0.0f);
            p.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            p.active      = true;
        }
        else if (child.hasType("Traj") && trajSlot < MAX_TRAJECTORY_PATHS)
        {
            auto& tp    = patch.trajectoryPaths[trajSlot++];
            tp.shape    = static_cast<PathShape>((int)child.getProperty("shape", 0));
            tp.x        = (float)child.getProperty("x",        0.5f);
            tp.y        = (float)child.getProperty("y",        0.5f);
            tp.radius   = (float)child.getProperty("radius",   0.15f);
            tp.length   = (float)child.getProperty("length",   0.30f);
            tp.angleRad = (float)child.getProperty("angle",    0.0f);
            tp.curve    = static_cast<TrajectoryLineCurve>((int)child.getProperty("curve", 0));
            tp.mode     = static_cast<TrajectoryMode>((int)child.getProperty("mode", 0));
            tp.speed    = (float)child.getProperty("speed",    0.5f);
            tp.currentT = (float)child.getProperty("currentT", 0.0f);
            tp.active   = true;
        }
        else if (child.hasType("Flow") && flowSlot < MAX_TANGENT_PATHS)
        {
            auto& tp     = patch.tangentPaths[flowSlot++];
            tp.shape     = static_cast<PathShape>((int)child.getProperty("shape", 0));
            tp.x         = (float)child.getProperty("x",         0.5f);
            tp.y         = (float)child.getProperty("y",         0.5f);
            tp.radius    = (float)child.getProperty("radius",    0.15f);
            tp.width     = (float)child.getProperty("width",     0.08f);
            tp.strength  = (float)child.getProperty("strength",  0.40f);
            tp.chirality = (float)child.getProperty("chirality", 1.0f);
            tp.trajectoryPathIndex = (int)child.getProperty("trajPath", -1);
            tp.active    = true;
        }
        else if (child.hasType("Mod") && modSlot < MAX_MOD_CONNECTIONS)
        {
            auto& c    = patch.modConnections[modSlot++];
            c.srcType  = static_cast<ModSourceType>((int)child.getProperty("srcType", 0));
            c.srcIndex = (int)child.getProperty("srcIndex", 0);
            c.dstType  = static_cast<ModDestType>((int)child.getProperty("dstType", 0));
            c.dstIndex = (int)child.getProperty("dstIndex", 0);
            c.depth    = (float)child.getProperty("depth",   0.0f);
            c.base     = (float)child.getProperty("base",    0.0f);
            c.active   = true;
        }
    }

    patch.activeAnchorCount = anchorSlot;
    physicsEngine_.applyPatch(patch);
}

// ─────────────────────────────────────────────────────────────────────────────
// Plugin factory — required by JUCE
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphosProcessor();
}
