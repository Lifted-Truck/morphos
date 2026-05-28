#include "PhysicsEngine.h"
#include <cmath>
#include <chrono>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static float midiNoteToHz(int note) noexcept
{
    return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — initialise default patch for Phase 1 demonstration
// ─────────────────────────────────────────────────────────────────────────────

PhysicsEngine::PhysicsEngine()
    : juce::Thread("MorphosPhysics")
{
    // ── Default field objects ─────────────────────────────────────────────────
    // Phase 3+: replaced by user placement. These are chosen so the default
    // patch has immediately interesting orbital behaviour without any editing.
    //
    // Design intent:
    //   Attractor bottom-centre  — the gravitational well; Morphons orbit it.
    //   Vortex upper-left        — off-centre from spawn so it has non-zero arm
    //                              and imparts CCW angular momentum immediately.
    //   Repeller upper-right     — redirects trajectories that escape the orbit
    //                              upward, keeping everything in play.

    // Attractor — bottom-centre; the main orbital anchor
    fieldObjects_[0].type     = FieldObjectType::Attractor;
    fieldObjects_[0].x        = 0.50f;
    fieldObjects_[0].y        = 0.75f;
    fieldObjects_[0].strength = 0.28f;
    fieldObjects_[0].radius   = 0.62f;
    fieldObjects_[0].active   = true;

    // Vortex — upper-left; imparts CCW spin. Arm to spawn (0.5,0.5): ~0.25
    // so force is non-zero from the first tick.
    fieldObjects_[1].type      = FieldObjectType::Vortex;
    fieldObjects_[1].x         = 0.38f;
    fieldObjects_[1].y         = 0.30f;
    fieldObjects_[1].strength  = 0.22f;
    fieldObjects_[1].radius    = 0.65f;
    fieldObjects_[1].chirality = 1.0f;  // CCW
    fieldObjects_[1].active    = true;

    // Repeller — upper-right; deflects escaping Morphons back into the orbit
    fieldObjects_[2].type     = FieldObjectType::Repeller;
    fieldObjects_[2].x        = 0.72f;
    fieldObjects_[2].y        = 0.25f;
    fieldObjects_[2].strength = 0.15f;
    fieldObjects_[2].radius   = 0.38f;
    fieldObjects_[2].active   = true;

    // ── Default emitter ───────────────────────────────────────────────────────
    // launchSpeed > 0 gives the Morphon initial angular momentum so the
    // Attractor bends it into an orbit rather than a straight plunge.
    // spawnDrag = 0.001 lets orbits persist for several seconds — enough to
    // hear clear timbral evolution — before decaying toward the Attractor.
    emitters_[0].x            = 0.50f;
    emitters_[0].y            = 0.50f;
    emitters_[0].launchAngle  = 0.0f;   // Rightward; field curves into orbit
    emitters_[0].launchSpeed  = 0.18f;  // Initial velocity (Manifold units/sec)
    emitters_[0].spawnMass    = 1.0f;
    emitters_[0].spawnDrag    = 0.001f; // Low drag: ~60% velocity after 1 s
    emitters_[0].attackTime   = 0.05f;
    emitters_[0].decayTime    = 0.15f;
    emitters_[0].sustainLevel = 0.70f;
    emitters_[0].releaseTime  = 0.35f;
    emitters_[0].active       = true;

    // ── Timbral Anchors for Phase 2/3 demonstration ───────────────────────────
    // Anchor 0 — upper-left: dark (low rolloff brightness) and purely harmonic
    timbralAnchors_[0] = { 0.15f, 0.15f, 0.05f, 0.00f, true };
    // Anchor 1 — lower-right: bright (flat spectrum) and heavily inharmonic
    timbralAnchors_[1] = { 0.85f, 0.85f, 0.92f, 0.80f, true };

    fieldGrid_.dirty = true;  // Will be rebuilt on first tick
}

PhysicsEngine::~PhysicsEngine()
{
    stopSimulation();
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::startSimulation()
{
    startThread(juce::Thread::Priority::high);
}

void PhysicsEngine::stopSimulation()
{
    signalThreadShouldExit();
    notify();
    stopThread(2000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread interface
// ─────────────────────────────────────────────────────────────────────────────

bool PhysicsEngine::pushNoteEvent(const NoteEvent& event) noexcept
{
    int s1, n1, s2, n2;
    eventFifo_.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return false;
    eventBuffer_[s1] = event;
    eventFifo_.finishedWrite(1);
    return true;
}

bool PhysicsEngine::pushManifoldEdit(const ManifoldEdit& edit) noexcept
{
    int s1, n1, s2, n2;
    editFifo_.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return false;
    editBuffer_[s1] = edit;
    editFifo_.finishedWrite(1);
    return true;
}

const PhysicsStateSnapshot& PhysicsEngine::getLatestState() noexcept
{
    return bridge_.getLatestForAudio();
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter writes
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::setGlobalTimeScale(float scale) noexcept
{
    globalTimeScale_.store(scale, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread loop
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::run()
{
    using namespace std::chrono;
    constexpr auto TICK_DUR = duration_cast<nanoseconds>(
        duration<double>(TICK_INTERVAL_SEC));

    auto nextTick = steady_clock::now();

    while (!threadShouldExit())
    {
        const double timeScale = static_cast<double>(
            globalTimeScale_.load(std::memory_order_relaxed));

        tick(TICK_INTERVAL_SEC * timeScale);

        nextTick += TICK_DUR;
        const auto now = steady_clock::now();
        if (nextTick > now)
            std::this_thread::sleep_until(nextTick);
        else
            nextTick = now;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::tick(double dtSeconds)
{
    drainNoteEvents();
    drainEditCommands();      // Apply UI edits before rebuilding grid / integrating
    rebuildFieldGridIfDirty();
    integrateMorphons(dtSeconds);
    updateEnvelopes(dtSeconds);
    applyEffectZones();       // After envelope so amplitude modulation is applied last
    writeSnapshot();

    ++tickIndex_;
    simulationTimeMs_ += dtSeconds * 1000.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Note handling
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::drainNoteEvents()
{
    int s1, n1, s2, n2;
    eventFifo_.prepareToRead(eventFifo_.getNumReady(), s1, n1, s2, n2);

    auto process = [this](int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto& e = eventBuffer_[i];
            if (e.type == NoteEvent::Type::NoteOn)
                handleNoteOn(e.channel, e.note, e.velocity);
            else
                handleNoteOff(e.channel, e.note);
        }
    };

    process(s1, n1);
    process(s2, n2);
    eventFifo_.finishedRead(n1 + n2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edit command drain — UI → physics; runs at start of each tick
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::drainEditCommands()
{
    int s1, n1, s2, n2;
    editFifo_.prepareToRead(editFifo_.getNumReady(), s1, n1, s2, n2);

    auto apply = [this](int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto& e   = editBuffer_[i];
            const int   idx = e.index;

            switch (e.type)
            {
                // ── Position edits ────────────────────────────────────────────
                case ManifoldEdit::Type::MoveFieldObject:
                    if (idx >= 0 && idx < MAX_FIELD_OBJECTS)
                    {
                        fieldObjects_[idx].x = e.x;
                        fieldObjects_[idx].y = e.y;
                        fieldGrid_.dirty = true;
                    }
                    break;

                case ManifoldEdit::Type::MoveEmitter:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                    {
                        emitters_[idx].x = e.x;
                        emitters_[idx].y = e.y;
                    }
                    break;

                case ManifoldEdit::Type::MoveTimbralAnchor:
                    if (idx >= 0 && idx < activeAnchorCount_)
                    {
                        timbralAnchors_[idx].x = e.x;
                        timbralAnchors_[idx].y = e.y;
                    }
                    break;

                // ── Field object property edits ───────────────────────────────
                case ManifoldEdit::Type::SetFieldObjectStrength:
                    if (idx >= 0 && idx < MAX_FIELD_OBJECTS)
                    {
                        fieldObjects_[idx].strength = e.x;
                        fieldGrid_.dirty = true;
                    }
                    break;

                case ManifoldEdit::Type::SetFieldObjectRadius:
                    if (idx >= 0 && idx < MAX_FIELD_OBJECTS)
                    {
                        fieldObjects_[idx].radius = e.x;
                        fieldGrid_.dirty = true;
                    }
                    break;

                case ManifoldEdit::Type::SetFieldObjectChirality:
                    if (idx >= 0 && idx < MAX_FIELD_OBJECTS)
                    {
                        fieldObjects_[idx].chirality = e.x;
                        fieldGrid_.dirty = true;
                    }
                    break;

                // ── Emitter property edits ────────────────────────────────────
                case ManifoldEdit::Type::SetEmitterLaunchAngle:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].launchAngle = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterLaunchSpeed:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].launchSpeed = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterAttack:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].attackTime = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterDecay:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].decayTime = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterSustain:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].sustainLevel = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterRelease:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].releaseTime = e.x;
                    break;

                case ManifoldEdit::Type::SetEmitterKeyLow:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].keyLow = juce::jlimit(0, 127, (int)e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterKeyHigh:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].keyHigh = juce::jlimit(0, 127, (int)e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterTransposeOct:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].transposeOct = juce::jlimit(-4, 4, (int)e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterTransposeSemi:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].transposeSemi = juce::jlimit(-12, 12, (int)e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterTransposeCents:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].transposeCents = juce::jlimit(-100.0f, 100.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterPan:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].pan = juce::jlimit(-1.0f, 1.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterTerminusEnabled:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].terminusEnabled = (e.x >= 0.5f);
                    break;

                case ManifoldEdit::Type::SetEmitterTerminusStrength:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].terminusStrength = juce::jlimit(0.0f, 2.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetEmitterTerminusRadius:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].terminusArrivalRadius = juce::jlimit(0.005f, 0.25f, e.x);
                    break;

                // ── Global manifold topology ──────────────────────────────────
                case ManifoldEdit::Type::SetGlobalBoundary:
                    globalBoundary_ = static_cast<BoundaryBehavior>(
                        static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                case ManifoldEdit::Type::SetPolyMode:
                    globalPolyMode_ = static_cast<PolyMode>(
                        static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                // ── Timbral Anchor property edits ─────────────────────────────
                case ManifoldEdit::Type::SetTimbralAnchorTimbreX:
                    if (idx >= 0 && idx < activeAnchorCount_)
                        timbralAnchors_[idx].timbreX = e.x;
                    break;

                case ManifoldEdit::Type::SetTimbralAnchorTimbreY:
                    if (idx >= 0 && idx < activeAnchorCount_)
                        timbralAnchors_[idx].timbreY = e.x;
                    break;

                case ManifoldEdit::Type::SetGlideTime:
                    globalGlideTimeSec_ = juce::jlimit(0.0f, 5.0f, e.x);
                    break;

                // ── Spawn / add ────────────────────────────────────────────────
                case ManifoldEdit::Type::AddAttractor:
                case ManifoldEdit::Type::AddRepeller:
                case ManifoldEdit::Type::AddVortex:
                {
                    // Find first inactive field-object slot
                    int slot = -1;
                    for (int k = 0; k < MAX_FIELD_OBJECTS; ++k)
                        if (!fieldObjects_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& obj    = fieldObjects_[slot];
                    obj.x        = e.x;
                    obj.y        = e.y;
                    obj.strength = 0.25f;
                    obj.radius   = 0.45f;
                    obj.chirality = 1.0f;
                    obj.active   = true;

                    if (e.type == ManifoldEdit::Type::AddAttractor)
                        obj.type = FieldObjectType::Attractor;
                    else if (e.type == ManifoldEdit::Type::AddRepeller)
                        obj.type = FieldObjectType::Repeller;
                    else
                        obj.type = FieldObjectType::Vortex;

                    fieldGrid_.dirty = true;
                    break;
                }

                case ManifoldEdit::Type::AddEmitter:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_EMITTERS; ++k)
                        if (!emitters_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& em        = emitters_[slot];
                    em.x            = e.x;
                    em.y            = e.y;
                    em.launchAngle  = 0.0f;
                    em.launchSpeed  = 0.18f;
                    em.spawnMass    = 1.0f;
                    em.spawnDrag    = 0.001f;
                    em.attackTime   = 0.05f;
                    em.decayTime    = 0.15f;
                    em.sustainLevel = 0.70f;
                    em.releaseTime  = 0.35f;
                    em.active       = true;
                    break;
                }

                case ManifoldEdit::Type::AddTimbralAnchor:
                    if (activeAnchorCount_ < MAX_TIMBRAL_ANCHORS)
                    {
                        auto& a  = timbralAnchors_[activeAnchorCount_];
                        a.x      = e.x;
                        a.y      = e.y;
                        a.timbreX = 0.5f;
                        a.timbreY = 0.0f;
                        a.active  = true;
                        ++activeAnchorCount_;
                    }
                    break;

                // ── Remove / deactivate ────────────────────────────────────────
                case ManifoldEdit::Type::RemoveFieldObject:
                    if (idx >= 0 && idx < MAX_FIELD_OBJECTS)
                    {
                        fieldObjects_[idx].active = false;
                        fieldGrid_.dirty = true;
                    }
                    break;

                case ManifoldEdit::Type::RemoveEmitter:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].active = false;
                    break;

                case ManifoldEdit::Type::RemoveTimbralAnchor:
                    if (idx >= 0 && idx < activeAnchorCount_)
                    {
                        // Maintain compaction: swap with last active anchor
                        --activeAnchorCount_;
                        if (idx < activeAnchorCount_)
                            timbralAnchors_[idx] = timbralAnchors_[activeAnchorCount_];
                        timbralAnchors_[activeAnchorCount_].active = false;
                    }
                    break;

                // ── Effect zone position edits ─────────────────────────────────
                case ManifoldEdit::Type::MoveEffectZone:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                    {
                        effectZones_[idx].x = e.x;
                        effectZones_[idx].y = e.y;
                    }
                    break;

                // ── Effect zone property edits ─────────────────────────────────
                case ManifoldEdit::Type::SetEffectZoneRadius:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                        effectZones_[idx].radius = juce::jlimit(0.01f, 0.75f, e.x);
                    break;

                case ManifoldEdit::Type::SetEffectZoneDepth:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                        effectZones_[idx].depth = e.x;
                    break;

                case ManifoldEdit::Type::SetEffectZoneTarget:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                        effectZones_[idx].target = static_cast<ZoneTarget>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                case ManifoldEdit::Type::SetEffectZoneFalloff:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                        effectZones_[idx].falloff = static_cast<ZoneFalloff>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                // ── Effect zone spawn / remove ─────────────────────────────────
                case ManifoldEdit::Type::AddEffectZone:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_EFFECT_ZONES; ++k)
                        if (!effectZones_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& z   = effectZones_[slot];
                    z.x       = e.x;
                    z.y       = e.y;
                    z.radius  = 0.15f;
                    z.depth   = 0.5f;
                    z.target  = ZoneTarget::TimbreX;
                    z.falloff = ZoneFalloff::Gaussian;
                    z.active  = true;
                    break;
                }

                case ManifoldEdit::Type::RemoveEffectZone:
                    if (idx >= 0 && idx < MAX_EFFECT_ZONES)
                        effectZones_[idx].active = false;
                    break;
            }
        }
    };

    apply(s1, n1);
    apply(s2, n2);
    editFifo_.finishedRead(n1 + n2);
}

void PhysicsEngine::handleNoteOn(int channel, int note, int /*velocity*/)
{
    // ── Legato / Slur retarget ────────────────────────────────────────────────
    // Legato (gap-sensitive): only retargets a voice that is still held.
    //   If the previous note was released before this note-on arrives, the voice
    //   is in Release — we skip it and fall through to a fresh spawn, just like
    //   pressing a completely new note.
    // LegatoSlur (always): retargets any active voice, even one in Release.
    //   Preserves the "always-connected" glide behaviour regardless of timing.
    // Both: keep Manifold position + velocity; only pitch and envelope change.
    //   If glide time > 0, fundamentalHz slides toward the new target rather than
    //   snapping instantly.
    if (globalPolyMode_ == PolyMode::Legato || globalPolyMode_ == PolyMode::LegatoSlur)
    {
        for (auto& m : morphons_)
        {
            if (!m.active || m.midiChannel != channel) continue;
            if (globalPolyMode_ == PolyMode::Legato && m.noteReleased)
                continue;   // Gap: voice released before new note — spawn fresh

            const int   ei = m.emitterIndex;
            const float xp = (ei >= 0 && ei < MAX_EMITTERS)
                ? std::pow(2.0f, emitters_[ei].transposeOct
                               + emitters_[ei].transposeSemi / 12.0f
                               + emitters_[ei].transposeCents / 1200.0f)
                : 1.0f;
            const float newHz         = midiNoteToHz(note) * xp;
            m.targetFundamentalHz     = newHz;
            if (globalGlideTimeSec_ <= 0.0f)
                m.fundamentalHz = newHz;   // Instant pitch when glide is off
            // else: integrateMorphons() slides fundamentalHz → targetFundamentalHz

            m.midiNote     = note;
            m.noteReleased = false;
            m.envStage     = EnvelopeStage::Attack;
            m.age          = 0.0f;
            return;
        }
        // No retargetable voice found — fall through to spawn
    }

    // ── Release all active voices on this channel before spawning ────────────
    // Covers Mono (always), Legato (gap case), and LegatoSlur (gap case).
    if (globalPolyMode_ != PolyMode::Polyphonic)
    {
        for (auto& m : morphons_)
            if (m.active && m.midiChannel == channel)
                m.noteReleased = true;
    }

    // ── Spawn — polyphonic: all in-range Emitters; mono/legato: first only ───
    for (int ei = 0; ei < MAX_EMITTERS; ++ei)
    {
        const auto& em = emitters_[ei];
        if (!em.active)                            continue;
        if (note < em.keyLow || note > em.keyHigh) continue;

        // In polyphonic mode, check for a same-note retrigger from this Emitter.
        // Release the existing voice gracefully so its envelope can tail out, then
        // fall through to spawn a fresh Morphon from the Emitter origin.
        // Previously we recycled the slot in-place (position snap + envelope reset),
        // which caused a click. The new voice gets its own slot and a clean Attack.
        if (globalPolyMode_ == PolyMode::Polyphonic)
        {
            for (int i = 0; i < MAX_MORPHONS; ++i)
            {
                auto& m = morphons_[i];
                if (m.active && m.midiNote == note
                    && m.midiChannel == channel && m.emitterIndex == ei)
                {
                    m.noteReleased  = true;   // Graceful release — envelope tails out
                    m.terminusArmed = false;  // Don't pull toward Terminus on retrigger-release
                    break;
                }
            }
            // Fall through — findFreeSlot() spawns the new voice below
        }

        const int slot = findFreeSlot();
        if (slot < 0) break;   // Pool full

        auto& m           = morphons_[slot];
        m.active          = true;
        m.noteReleased    = false;
        m.terminusArmed   = false;
        m.terminusReached = false;
        m.envStage        = EnvelopeStage::Attack;
        m.midiNote      = note;
        m.midiChannel   = channel;
        m.emitterIndex  = ei;
        m.x             = em.x;
        m.y             = em.y;
        m.vx            = std::cosf(em.launchAngle) * em.launchSpeed;
        m.vy            = std::sinf(em.launchAngle) * em.launchSpeed;
        m.mass          = em.spawnMass;
        m.drag          = em.spawnDrag;
        m.amplitude          = 0.0f;
        m.age                = 0.0f;
        m.timbreX            = 0.5f;
        m.timbreY            = 0.0f;
        m.basePan            = em.pan;
        m.pan                = em.pan;
        m.pitchZoneSemitones = 0.0f;
        {
            const float newHz = midiNoteToHz(note)
                * std::pow(2.0f, em.transposeOct
                               + em.transposeSemi / 12.0f
                               + em.transposeCents / 1200.0f);
            m.fundamentalHz       = newHz;  // Fresh spawn: always instant (no glide)
            m.targetFundamentalHz = newHz;
        }

        // Mono/Legato/Slur: one voice total — stop after first successful spawn
        if (globalPolyMode_ != PolyMode::Polyphonic) break;
    }
}

void PhysicsEngine::handleNoteOff(int channel, int note)
{
    // Release ALL Morphons matching this note+channel — there may be one per
    // Emitter if multiple Emitters had overlapping key ranges covering `note`.
    for (auto& m : morphons_)
    {
        if (!m.active || m.midiNote != note || m.midiChannel != channel) continue;

        m.noteReleased = true;

        // Arm Terminus only if the morphon is currently OUTSIDE the arrival zone.
        // This prevents the "immediate-fire" click that occurred when the morphon
        // was already inside the radius at the moment of note-off — the terminus
        // should only activate on a crossing event (outside → inside), not a
        // state check (already inside at note-off).
        const int ei = m.emitterIndex;
        if (ei >= 0 && ei < MAX_EMITTERS && emitters_[ei].terminusEnabled)
        {
            const auto& em    = emitters_[ei];
            const float dx    = m.x - em.x;
            const float dy    = m.y - em.y;
            const float dist2 = dx * dx + dy * dy;
            const float ar    = em.terminusArrivalRadius;
            m.terminusArmed   = (dist2 >= ar * ar);  // Armed only if currently outside
        }
        else
        {
            m.terminusArmed = false;
        }
    }
}

int PhysicsEngine::findFreeSlot() const noexcept
{
    for (int i = 0; i < MAX_MORPHONS; ++i)
        if (!morphons_[i].active)
            return i;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Field grid
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::rebuildFieldGridIfDirty()
{
    if (!fieldGrid_.dirty) return;
    fieldGrid_.rebuild(fieldObjects_.data(), MAX_FIELD_OBJECTS);
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::integrateMorphons(double dt)
{
    const float dtF = static_cast<float>(dt);

    for (auto& m : morphons_)
    {
        if (!m.active) continue;

        m.age += dtF;

        // Sample field force at current position
        float fx, fy;
        fieldGrid_.sample(m.x, m.y, fx, fy);

        // ── Terminus pull ─────────────────────────────────────────────────────
        // Active only after note-off AND when the morphon was outside the arrival
        // zone at note-off time (terminusArmed). This implements a crossing-event
        // model: Terminus fires only when the morphon travels from outside → inside,
        // never when it was already sitting inside the zone when the note was released.
        if (m.noteReleased && m.terminusArmed)
        {
            const int ei = m.emitterIndex;
            if (ei >= 0 && ei < MAX_EMITTERS && emitters_[ei].terminusEnabled)
            {
                const auto& em   = emitters_[ei];
                const float dx   = em.x - m.x;
                const float dy   = em.y - m.y;
                const float dist2 = dx * dx + dy * dy;
                const float ar    = em.terminusArrivalRadius;

                if (dist2 < ar * ar)
                {
                    // Pin at Terminus position with zero velocity.
                    // Do NOT deactivate here — let updateEnvelopes() drain
                    // the Release stage to zero cleanly (prevents click on
                    // arrival and preserves the configured release tail).
                    // terminusReached tells updateEnvelopes() to apply the
                    // arrival speed-up multiplier for a snappier fade-out.
                    m.x              = em.x;
                    m.y              = em.y;
                    m.vx             = 0.0f;
                    m.vy             = 0.0f;
                    m.terminusReached = true;
                    continue;
                }

                // Constant-magnitude pull toward Emitter origin
                const float invDist = 1.0f / std::sqrt(dist2 + 1e-12f);
                fx += em.terminusStrength * dx * invDist;
                fy += em.terminusStrength * dy * invDist;
            }
        }

        // F = ma  →  a = F/mass
        const float ax = fx / m.mass;
        const float ay = fy / m.mass;

        // Semi-implicit Euler with drag:
        //   v' = v*(1 - drag) + a*dt
        //   x' = x + v'*dt
        m.vx = m.vx * (1.0f - m.drag) + ax * dtF;
        m.vy = m.vy * (1.0f - m.drag) + ay * dtF;

        m.x += m.vx * dtF;
        m.y += m.vy * dtF;

        applyBoundary(m);

        // Blend Timbral Anchors at current Manifold position.
        // timbreX → spectral rolloff [0,1]; timbreY → inharmonicity [0,1].
        // Active anchors are compacted at the front; pass activeAnchorCount_.
        blendAnchors(m.x, m.y,
                     timbralAnchors_.data(), activeAnchorCount_,
                     m.timbreX, m.timbreY);

        // ── Pitch glide ───────────────────────────────────────────────────────
        // Exponential approach in log-frequency space so the slide rate is
        // perceptually constant regardless of interval size (semitones/sec).
        // Only runs when glide is on and the voice hasn't reached its target.
        // Threshold ≈ 0.01 cents — inaudible, prevents infinite asymptote.
        if (globalGlideTimeSec_ > 0.0f && m.fundamentalHz != m.targetFundamentalHz)
        {
            const float logRatio = std::log(m.targetFundamentalHz / m.fundamentalHz);
            if (std::abs(logRatio) > 0.0001f)
                m.fundamentalHz *= std::exp(logRatio * dtF / globalGlideTimeSec_);
            else
                m.fundamentalHz = m.targetFundamentalHz;
        }
    }
}

void PhysicsEngine::applyBoundary(MorphonState& m) const noexcept
{
    auto wrap = [](float v) -> float
    {
        // fmod + offset handles negatives safely
        v = std::fmodf(v, 1.0f);
        if (v < 0.0f) v += 1.0f;
        return v;
    };

    switch (globalBoundary_)
    {
        case BoundaryBehavior::Wrap:
            m.x = wrap(m.x);
            m.y = wrap(m.y);
            break;

        case BoundaryBehavior::Reflect:
            if (m.x < 0.0f) { m.x = -m.x;       m.vx = -m.vx; }
            if (m.x > 1.0f) { m.x = 2.0f - m.x; m.vx = -m.vx; }
            if (m.y < 0.0f) { m.y = -m.y;        m.vy = -m.vy; }
            if (m.y > 1.0f) { m.y = 2.0f - m.y;  m.vy = -m.vy; }
            break;

        case BoundaryBehavior::Terminate:
            if (m.x < 0.0f || m.x > 1.0f || m.y < 0.0f || m.y > 1.0f)
            {
                // Trigger the release envelope — same pattern as Terminus:
                // do NOT set m.active = false here (instant amplitude cut = click).
                // updateEnvelopes() will deactivate cleanly when amplitude reaches 0.
                m.noteReleased = true;
                // Pin to the boundary edge and stop so the Morphon stays visible
                // during its release tail instead of flying off-canvas.
                m.x  = juce::jlimit(0.0f, 1.0f, m.x);
                m.y  = juce::jlimit(0.0f, 1.0f, m.y);
                m.vx = 0.0f;
                m.vy = 0.0f;
            }
            break;

        case BoundaryBehavior::KleinBottle:
            // X wraps normally (cylinder identification on the X axis).
            m.x = wrap(m.x);
            // Y wraps with X-flip: crossing the top/bottom edge mirrors x to (1-x)
            // and negates vx — the Klein bottle identification.
            // Use while loops to handle corner cases where |vy| > 1 in a tick.
            while (m.y > 1.0f)
            {
                m.y  -= 1.0f;
                m.x   = 1.0f - m.x;
                m.vx  = -m.vx;
            }
            while (m.y < 0.0f)
            {
                m.y  += 1.0f;
                m.x   = 1.0f - m.x;
                m.vx  = -m.vx;
            }
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Envelope — full ADSR state machine
//
// Note-off transitions the Morphon immediately to Release regardless of current
// stage. On retrigger (NoteOn to an already-active slot) envStage is reset to
// Attack by handleNoteOn before this function runs.
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::updateEnvelopes(double dt)
{
    const float dtF = static_cast<float>(dt);

    for (auto& m : morphons_)
    {
        if (!m.active) continue;

        // Each Morphon uses its own Emitter's ADSR rates
        const int   ei      = (m.emitterIndex >= 0 && m.emitterIndex < MAX_EMITTERS)
                              ? m.emitterIndex : 0;
        const auto& emitter = emitters_[ei];

        const float attackRate  = (emitter.attackTime  > 0.0f)
                                  ? dtF / emitter.attackTime  : 1.0f;
        const float decayRate   = (emitter.decayTime   > 0.0f)
                                  ? dtF / emitter.decayTime   : 1.0f;
        // Terminus arrival speeds up the release by this factor so the morphon
        // fades out snappily once it lands on the zone, regardless of the
        // configured release time. Adjust the constant to taste.
        constexpr float TERMINUS_RELEASE_MULT = 8.0f;
        const float effectiveRelease = (m.terminusReached)
                                       ? emitter.releaseTime / TERMINUS_RELEASE_MULT
                                       : emitter.releaseTime;
        const float releaseRate = (effectiveRelease > 0.0f)
                                  ? dtF / effectiveRelease : 1.0f;
        const float sustainLvl  = emitter.sustainLevel;

        // Note-off forces Release regardless of current stage
        if (m.noteReleased && m.envStage != EnvelopeStage::Release)
            m.envStage = EnvelopeStage::Release;

        switch (m.envStage)
        {
            case EnvelopeStage::Attack:
                m.amplitude += attackRate;
                if (m.amplitude >= 1.0f)
                {
                    m.amplitude = 1.0f;
                    m.envStage  = EnvelopeStage::Decay;
                }
                break;

            case EnvelopeStage::Decay:
                m.amplitude -= decayRate;
                if (m.amplitude <= sustainLvl)
                {
                    m.amplitude = sustainLvl;
                    m.envStage  = EnvelopeStage::Sustain;
                }
                break;

            case EnvelopeStage::Sustain:
                m.amplitude = sustainLvl;
                break;

            case EnvelopeStage::Release:
                m.amplitude -= releaseRate;
                if (m.amplitude <= 0.0f)
                {
                    m.amplitude = 0.0f;
                    m.active    = false;
                }
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Effect zones — spatial modulation applied after envelope update
//
// Each tick: reset live pan and pitch contribution to their base values, then
// accumulate all active zone deltas weighted by proximity.
//
// Pan and timbre targets are clamped to their parameter ranges after accumulation.
// Pitch accumulates freely in semitones (no clamp — extreme detuning is valid).
// Amplitude is clamped to [0,1] so zones cannot push beyond envelope bounds.
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::applyEffectZones()
{
    for (auto& m : morphons_)
    {
        if (!m.active) continue;

        // Reset live modulation contributions — zones re-accumulate from scratch each tick
        m.pan                = m.basePan;
        m.pitchZoneSemitones = 0.0f;

        for (int zi = 0; zi < MAX_EFFECT_ZONES; ++zi)
        {
            const auto& z = effectZones_[zi];
            if (!z.active) continue;

            const float dx   = m.x - z.x;
            const float dy   = m.y - z.y;
            const float dist = std::sqrt(dx * dx + dy * dy);

            if (dist >= z.radius) continue;

            float w;
            if (z.falloff == ZoneFalloff::Linear)
            {
                w = 1.0f - dist / z.radius;
            }
            else  // Gaussian: sigma = radius/3 so w ≈ 0.01 at the zone edge
            {
                const float sigma = z.radius * 0.333f;
                w = std::exp(-(dist * dist) / (2.0f * sigma * sigma));
            }

            const float delta = w * z.depth;

            switch (z.target)
            {
                case ZoneTarget::TimbreX:
                    m.timbreX = juce::jlimit(0.0f, 1.0f, m.timbreX + delta);
                    break;

                case ZoneTarget::TimbreY:
                    m.timbreY = juce::jlimit(0.0f, 1.0f, m.timbreY + delta);
                    break;

                case ZoneTarget::Amplitude:
                    m.amplitude = juce::jlimit(0.0f, 1.0f, m.amplitude + delta);
                    break;

                case ZoneTarget::Pan:
                    m.pan = juce::jlimit(-1.0f, 1.0f, m.pan + delta);
                    break;

                case ZoneTarget::Pitch:
                    m.pitchZoneSemitones += delta;  // Depth in semitones; applied in processBlock
                    break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::writeSnapshot()
{
    auto& snap = bridge_.getWriteBuffer();

    snap.morphons          = morphons_;
    snap.tickIndex         = tickIndex_;
    snap.simulationTimeMs  = simulationTimeMs_;

    // Count active Morphons
    int activeMorphons = 0;
    for (const auto& m : morphons_)
        if (m.active) ++activeMorphons;
    snap.activeMorphonCount = activeMorphons;

    // Copy field objects for UI rendering
    int activeObjs = 0;
    for (int i = 0; i < MAX_FIELD_OBJECTS; ++i)
    {
        const auto& src = fieldObjects_[i];
        auto&       dst = snap.fieldObjects[i];
        dst.type      = src.type;
        dst.x         = src.x;
        dst.y         = src.y;
        dst.strength  = src.strength;
        dst.radius    = src.radius;
        dst.chirality = src.chirality;
        dst.active    = src.active;
        if (src.active) ++activeObjs;
    }
    snap.activeFieldObjCount = activeObjs;

    // Copy emitters for UI rendering
    for (int i = 0; i < MAX_EMITTERS; ++i)
    {
        const auto& src  = emitters_[i];
        auto&       dst  = snap.emitters[i];
        dst.x            = src.x;
        dst.y            = src.y;
        dst.launchAngle  = src.launchAngle;
        dst.launchSpeed  = src.launchSpeed;
        dst.spawnMass    = src.spawnMass;
        dst.spawnDrag    = src.spawnDrag;
        dst.attackTime   = src.attackTime;
        dst.decayTime    = src.decayTime;
        dst.sustainLevel = src.sustainLevel;
        dst.releaseTime  = src.releaseTime;
        dst.keyLow        = src.keyLow;
        dst.keyHigh       = src.keyHigh;
        dst.transposeOct  = src.transposeOct;
        dst.transposeSemi = src.transposeSemi;
        dst.transposeCents         = src.transposeCents;
        dst.pan                    = src.pan;
        dst.terminusEnabled        = src.terminusEnabled;
        dst.terminusStrength       = src.terminusStrength;
        dst.terminusArrivalRadius  = src.terminusArrivalRadius;
        dst.active                 = src.active;
    }

    snap.globalBoundary  = globalBoundary_;
    snap.globalPolyMode  = globalPolyMode_;
    snap.globalGlideTime = globalGlideTimeSec_;

    // Copy effect zones for UI rendering
    int activeZones = 0;
    for (int i = 0; i < MAX_EFFECT_ZONES; ++i)
    {
        const auto& src = effectZones_[i];
        auto&       dst = snap.effectZones[i];
        dst.x       = src.x;
        dst.y       = src.y;
        dst.radius  = src.radius;
        dst.depth   = src.depth;
        dst.target  = src.target;
        dst.falloff = src.falloff;
        dst.active  = src.active;
        if (src.active) ++activeZones;
    }
    snap.activeEffectZoneCount = activeZones;

    // Copy timbral anchors for UI rendering
    snap.activeTimbralAnchorCount = activeAnchorCount_;
    for (int i = 0; i < MAX_TIMBRAL_ANCHORS; ++i)
    {
        const auto& src = timbralAnchors_[i];
        auto&       dst = snap.timbralAnchors[i];
        dst.x       = src.x;
        dst.y       = src.y;
        dst.timbreX = src.timbreX;
        dst.timbreY = src.timbreY;
        dst.active  = (i < activeAnchorCount_);
    }

    bridge_.publish();
}

// ─────────────────────────────────────────────────────────────────────────────
// Patch apply — replaces all Manifold state from a saved patch.
//
// Stops the physics thread, replaces all object arrays and global params,
// clears all active Morphons (they belong to the old patch), and restarts.
// Must be called from the message thread only.
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::applyPatch(const PatchState& patch)
{
    stopSimulation();

    fieldObjects_       = patch.fieldObjects;
    emitters_           = patch.emitters;
    timbralAnchors_     = patch.timbralAnchors;
    effectZones_        = patch.effectZones;
    activeAnchorCount_  = patch.activeAnchorCount;
    globalBoundary_     = patch.boundary;
    globalPolyMode_     = patch.polyMode;
    globalGlideTimeSec_ = patch.glideTimeSec;

    morphons_        = {};    // Kill all active voices from the previous patch
    fieldGrid_.dirty = true;  // Force grid rebuild on first tick

    startSimulation();
}
