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

    // latestSnapshotForUI_ is a relaxed copy written by the audio thread each
    // processBlock() and read by the UI timer — same access pattern as the editor.
    const auto& snap = latestSnapshotForUI_;

    auto manifoldData = juce::ValueTree("ManifoldObjects");
    manifoldData.setProperty("version",     3,                             nullptr);
    manifoldData.setProperty("boundary",    (int)snap.globalBoundary,      nullptr);
    manifoldData.setProperty("glideTime",   snap.globalGlideTime,          nullptr);
    manifoldData.setProperty("friction",    snap.globalFriction,           nullptr);
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
    patch.globalFriction = (float)manifoldData.getProperty("friction",  0.0f);

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

    int fieldObjSlot = 0, emitterSlot = 0, anchorSlot = 0, zoneSlot = 0, gateSlot = 0, pathSlot = 0, trajSlot = 0, flowSlot = 0;

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
