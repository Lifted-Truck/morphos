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
    // Use field-by-field assignment rather than positional brace init so a
    // new member added between existing fields (like trajectoryPathIndex)
    // doesn't silently shift positional values into the wrong slot.
    //
    // Anchor 0 — upper-left: dark (low rolloff brightness) and purely harmonic
    timbralAnchors_[0].x       = 0.15f;
    timbralAnchors_[0].y       = 0.15f;
    timbralAnchors_[0].timbreX = 0.05f;
    timbralAnchors_[0].timbreY = 0.00f;
    timbralAnchors_[0].trajectoryPathIndex = -1;
    timbralAnchors_[0].active  = true;
    // Anchor 1 — lower-right: bright (flat spectrum) and heavily inharmonic
    timbralAnchors_[1].x       = 0.85f;
    timbralAnchors_[1].y       = 0.85f;
    timbralAnchors_[1].timbreX = 0.92f;
    timbralAnchors_[1].timbreY = 0.80f;
    timbralAnchors_[1].trajectoryPathIndex = -1;
    timbralAnchors_[1].active  = true;

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

void PhysicsEngine::setMacroValue(int idx, float value) noexcept
{
    if (idx >= 0 && idx < NUM_MACROS)
        macroValues_[idx].store(value, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Offline rendering
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::setOfflineMode(bool offline) noexcept
{
    const bool prev = offlineMode_.exchange(offline, std::memory_order_acq_rel);
    if (offline && !prev)
        offlineAccumulator_ = 0.0;   // Fresh accumulator for a new offline session
    if (!offline)
        notify();   // Wake the parked thread so it resumes wall-clock ticking
}

void PhysicsEngine::advance(double seconds)
{
    if (! offlineMode_.load(std::memory_order_acquire)) return;
    if (seconds <= 0.0) return;

    // Wait for the thread to confirm it's parked in wait() (i.e., not mid-tick).
    // This spin happens only on the first advance() after offline mode is
    // engaged; subsequent calls return immediately because the thread is
    // already parked. Worst case is one tick (~2 ms) of yielding.
    while (! offlineParked_.load(std::memory_order_acquire))
        std::this_thread::yield();

    const double timeScale = static_cast<double>(
        globalTimeScale_.load(std::memory_order_relaxed));

    offlineAccumulator_ += seconds;
    while (offlineAccumulator_ >= TICK_INTERVAL_SEC)
    {
        tick(TICK_INTERVAL_SEC * timeScale);
        offlineAccumulator_ -= TICK_INTERVAL_SEC;
    }
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
        // Park the thread while the audio processor is driving physics
        // synchronously for offline rendering. We never tick from this thread
        // when offlineMode_ is true — that would race the caller's tick().
        if (offlineMode_.load(std::memory_order_acquire))
        {
            offlineParked_.store(true, std::memory_order_release);
            wait(50);   // ms; wakes early on notify() when offline ends
            offlineParked_.store(false, std::memory_order_release);
            nextTick = steady_clock::now();   // Discard backlog on resume
            continue;
        }

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
    drainEditCommands();          // Apply UI edits first so subsequent steps see them
    advanceTrajectoryPaths(dtSeconds);
    updateAttachedEmitters();     // Move attached Emitters to their path positions
    drainNoteEvents();            // Pass 1: refreshes MIDI mod-source state (CC,
                                  //   keytrack, velocity); buffers note-on/off into
                                  //   pendingNoteEvents_ for later dispatch.
    evaluateModMatrix();          // Now sees this tick's MIDI events as well as the
                                  //   fresh trajectory positions; FieldObject writes
                                  //   here flag the grid dirty before the rebuild.
    applyPendingNoteEvents();     // Pass 2: dispatch note-on/off so Morphons spawn
                                  //   using mod-modulated emitter state (launch
                                  //   angle, position, etc.).
    rebuildFieldGridIfDirty();
    integrateMorphons(dtSeconds);
    applyPathConstraints();   // Snap pinned Morphons before gates so crossings
                              // see the post-snap trajectory (prev→snapped pos).
    applyFluxGates();         // Detect crossings before envelope update so the
                              // snap-to-Attack takes effect in this same tick.
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

    // Pass 1: capture CC events for MIDI mod-source state, but defer all
    // note-on/off dispatch into pendingNoteEvents_ so the mod matrix can
    // write to emitter params (e.g. launchAngle) before any Morphon is
    // actually spawned this tick. lastNote_ / lastVelocity_ are NOT updated
    // here — they're advanced per note-on inside applyPendingNoteEvents so
    // simultaneous keys each trigger their own mod re-eval.
    pendingNoteEventCount_ = 0;
    auto process = [this](int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto& e = eventBuffer_[i];
            switch (e.type)
            {
                case NoteEvent::Type::NoteOn:
                case NoteEvent::Type::NoteOff:
                    if (pendingNoteEventCount_ < (int) pendingNoteEvents_.size())
                        pendingNoteEvents_[pendingNoteEventCount_++] = e;
                    break;
                case NoteEvent::Type::ControlChange:
                    if (e.note >= 0 && e.note < 128)
                        midiCC_[e.note] = (uint8_t) juce::jlimit(0, 127, e.velocity);
                    break;
            }
        }
    };

    process(s1, n1);
    process(s2, n2);
    eventFifo_.finishedRead(n1 + n2);
}

void PhysicsEngine::applyPendingNoteEvents()
{
    // Pass 2: events buffered by drainNoteEvents are dispatched now. For each
    // NoteOn we update Keytrack / Velocity state and re-run the mod matrix
    // BEFORE the spawn — so simultaneous note-ons in the same tick each see
    // an emitter that has been freshly modulated for their own note/velocity.
    // Without this re-eval, all N simultaneous notes share whichever note's
    // value happened to land last in the FIFO drain.
    for (int i = 0; i < pendingNoteEventCount_; ++i)
    {
        const auto& e = pendingNoteEvents_[i];
        if (e.type == NoteEvent::Type::NoteOn)
        {
            lastNote_     = (uint8_t) juce::jlimit(0, 127, e.note);
            lastVelocity_ = (uint8_t) juce::jlimit(0, 127, e.velocity);
            evaluateModMatrix();   // Per-note re-eval; cheap (16 connections)
            handleNoteOn(e.channel, e.note, e.velocity);
        }
        else if (e.type == NoteEvent::Type::NoteOff)
        {
            handleNoteOff(e.channel, e.note);
        }
    }
    pendingNoteEventCount_ = 0;
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

                case ManifoldEdit::Type::SetEmitterPolyMode:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].polyMode = static_cast<PolyMode>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                case ManifoldEdit::Type::SetEmitterSpawnMass:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                        emitters_[idx].spawnMass = juce::jlimit(0.1f, 4.0f, e.x);
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

                case ManifoldEdit::Type::SetGlobalFriction:
                    globalFriction_ = juce::jlimit(0.0f, 10.0f, e.x);
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
                    em.trajectoryPathIndex = -1;
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
                        a.trajectoryPathIndex = -1;   // Don't inherit stale state from a previously-removed slot
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
                    {
                        emitters_[idx].active = false;
                        emitterHeldCount_[idx] = 0;  // Drop held-note state
                    }
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

                // ── Flux gate spawn / remove / edits ───────────────────────────
                case ManifoldEdit::Type::AddFluxGate:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_FLUX_GATES; ++k)
                        if (!fluxGates_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& g    = fluxGates_[slot];
                    g.x        = e.x;
                    g.y        = e.y;
                    g.length   = 0.20f;
                    g.angleRad = 0.0f;
                    g.active   = true;
                    break;
                }

                case ManifoldEdit::Type::RemoveFluxGate:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                        fluxGates_[idx].active = false;
                    break;

                case ManifoldEdit::Type::MoveFluxGate:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                    {
                        fluxGates_[idx].x = e.x;
                        fluxGates_[idx].y = e.y;
                    }
                    break;

                case ManifoldEdit::Type::SetFluxGateLength:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                        fluxGates_[idx].length = juce::jlimit(0.02f, 0.9f, e.x);
                    break;

                case ManifoldEdit::Type::SetFluxGateAngle:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                        fluxGates_[idx].angleRad = juce::jlimit(
                            -juce::MathConstants<float>::pi,
                             juce::MathConstants<float>::pi, e.x);
                    break;

                case ManifoldEdit::Type::SetFluxGateShape:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                        fluxGates_[idx].shape = static_cast<FluxGateShape>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                case ManifoldEdit::Type::SetFluxGateRadius:
                    if (idx >= 0 && idx < MAX_FLUX_GATES)
                        fluxGates_[idx].radius = juce::jlimit(0.02f, 0.45f, e.x);
                    break;

                // ── Mod-matrix edits ───────────────────────────────────────────
                case ManifoldEdit::Type::AddModConnection:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_MOD_CONNECTIONS; ++k)
                        if (!modConnections_[k].active) { slot = k; break; }
                    if (slot < 0) break;
                    auto& c    = modConnections_[slot];
                    c.srcType  = ModSourceType::None;
                    c.srcIndex = 0;
                    c.dstType  = ModDestType::None;
                    c.dstIndex = 0;
                    c.depth    = 0.0f;
                    c.base     = 0.0f;
                    c.active   = true;
                    break;
                }

                case ManifoldEdit::Type::RemoveModConnection:
                    if (idx >= 0 && idx < MAX_MOD_CONNECTIONS)
                        modConnections_[idx].active = false;
                    break;

                case ManifoldEdit::Type::SetModConnectionSource:
                    if (idx >= 0 && idx < MAX_MOD_CONNECTIONS)
                    {
                        auto& c    = modConnections_[idx];
                        c.srcType  = static_cast<ModSourceType>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                        c.srcIndex = static_cast<int>(e.y);
                    }
                    break;

                case ManifoldEdit::Type::SetModConnectionDest:
                    if (idx >= 0 && idx < MAX_MOD_CONNECTIONS)
                    {
                        auto& c    = modConnections_[idx];
                        c.dstType  = static_cast<ModDestType>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                        c.dstIndex = static_cast<int>(e.y);
                        // Capture the dest's current value as the base so the
                        // initial modulation pivots around what the user sees
                        // on the slider right now.
                        c.base = 0.0f;
                        auto readX = [&](auto& arr, int n) {
                            if (c.dstIndex >= 0 && c.dstIndex < n)
                                c.base = arr[c.dstIndex].x;
                        };
                        auto readY = [&](auto& arr, int n) {
                            if (c.dstIndex >= 0 && c.dstIndex < n)
                                c.base = arr[c.dstIndex].y;
                        };
                        switch (c.dstType)
                        {
                            // Object positions
                            case ModDestType::FieldObjectX:    readX(fieldObjects_,    MAX_FIELD_OBJECTS);    break;
                            case ModDestType::FieldObjectY:    readY(fieldObjects_,    MAX_FIELD_OBJECTS);    break;
                            case ModDestType::EmitterX:        readX(emitters_,        MAX_EMITTERS);         break;
                            case ModDestType::EmitterY:        readY(emitters_,        MAX_EMITTERS);         break;
                            case ModDestType::EmitterLaunchAngle:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].launchAngle;
                                break;
                            case ModDestType::EmitterLaunchSpeed:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].launchSpeed;
                                break;
                            case ModDestType::EmitterSpawnMass:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].spawnMass;
                                break;
                            case ModDestType::EmitterAttack:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].attackTime;
                                break;
                            case ModDestType::EmitterDecay:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].decayTime;
                                break;
                            case ModDestType::EmitterSustain:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].sustainLevel;
                                break;
                            case ModDestType::EmitterRelease:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].releaseTime;
                                break;
                            case ModDestType::EmitterTransposeCents:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].transposeCents;
                                break;
                            case ModDestType::EmitterPan:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].pan;
                                break;
                            case ModDestType::EmitterTerminusStrength:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].terminusStrength;
                                break;
                            case ModDestType::EmitterTerminusRadius:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EMITTERS)
                                    c.base = emitters_[c.dstIndex].terminusArrivalRadius;
                                break;
                            case ModDestType::AnchorX:         readX(timbralAnchors_,  activeAnchorCount_);   break;
                            case ModDestType::AnchorY:         readY(timbralAnchors_,  activeAnchorCount_);   break;
                            case ModDestType::EffectZoneX:     readX(effectZones_,     MAX_EFFECT_ZONES);     break;
                            case ModDestType::EffectZoneY:     readY(effectZones_,     MAX_EFFECT_ZONES);     break;
                            case ModDestType::FluxGateX:       readX(fluxGates_,       MAX_FLUX_GATES);       break;
                            case ModDestType::FluxGateY:       readY(fluxGates_,       MAX_FLUX_GATES);       break;
                            case ModDestType::PathObjectX:     readX(pathObjects_,     MAX_PATH_OBJECTS);     break;
                            case ModDestType::PathObjectY:     readY(pathObjects_,     MAX_PATH_OBJECTS);     break;
                            case ModDestType::TrajectoryPathX: readX(trajectoryPaths_, MAX_TRAJECTORY_PATHS); break;
                            case ModDestType::TrajectoryPathY: readY(trajectoryPaths_, MAX_TRAJECTORY_PATHS); break;
                            case ModDestType::TangentPathX:    readX(tangentPaths_,    MAX_TANGENT_PATHS);    break;
                            case ModDestType::TangentPathY:    readY(tangentPaths_,    MAX_TANGENT_PATHS);    break;

                            // Non-position destinations
                            case ModDestType::FieldObjectStrength:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_FIELD_OBJECTS)
                                    c.base = fieldObjects_[c.dstIndex].strength;
                                break;
                            case ModDestType::FieldObjectRadius:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_FIELD_OBJECTS)
                                    c.base = fieldObjects_[c.dstIndex].radius;
                                break;
                            case ModDestType::EffectZoneDepth:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EFFECT_ZONES)
                                    c.base = effectZones_[c.dstIndex].depth;
                                break;
                            case ModDestType::EffectZoneRadius:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_EFFECT_ZONES)
                                    c.base = effectZones_[c.dstIndex].radius;
                                break;
                            case ModDestType::TrajectoryCurrentT:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_TRAJECTORY_PATHS)
                                    c.base = trajectoryPaths_[c.dstIndex].currentT;
                                break;
                            case ModDestType::AnchorTimbreX:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_TIMBRAL_ANCHORS)
                                    c.base = timbralAnchors_[c.dstIndex].timbreX;
                                break;
                            case ModDestType::AnchorTimbreY:
                                if (c.dstIndex >= 0 && c.dstIndex < MAX_TIMBRAL_ANCHORS)
                                    c.base = timbralAnchors_[c.dstIndex].timbreY;
                                break;
                            case ModDestType::GlobalFriction:
                                c.base = globalFriction_;
                                break;
                            case ModDestType::None: default: break;
                        }
                    }
                    break;

                case ManifoldEdit::Type::SetModConnectionDepth:
                    if (idx >= 0 && idx < MAX_MOD_CONNECTIONS)
                        modConnections_[idx].depth = juce::jlimit(-1.0f, 1.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetModConnectionBase:
                    if (idx >= 0 && idx < MAX_MOD_CONNECTIONS)
                        modConnections_[idx].base = e.x;
                    break;

                // ── Path object spawn / remove / edits ────────────────────────
                case ManifoldEdit::Type::AddPathObject:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_PATH_OBJECTS; ++k)
                        if (!pathObjects_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& p      = pathObjects_[slot];
                    p.shape      = PathShape::Circle;
                    p.x          = e.x;
                    p.y          = e.y;
                    p.radius     = 0.15f;
                    p.snapRadius = 0.04f;
                    p.active     = true;
                    break;
                }

                case ManifoldEdit::Type::RemovePathObject:
                    if (idx >= 0 && idx < MAX_PATH_OBJECTS)
                    {
                        pathObjects_[idx].active = false;
                        // Unpin any Morphons attached to this path so they
                        // don't snap to a deactivated curve next tick.
                        for (auto& m : morphons_)
                            if (m.pathIndex == idx)
                                m.pathIndex = -1;
                    }
                    break;

                case ManifoldEdit::Type::MovePathObject:
                    if (idx >= 0 && idx < MAX_PATH_OBJECTS)
                    {
                        pathObjects_[idx].x = e.x;
                        pathObjects_[idx].y = e.y;
                    }
                    break;

                case ManifoldEdit::Type::SetPathObjectRadius:
                    if (idx >= 0 && idx < MAX_PATH_OBJECTS)
                        pathObjects_[idx].radius = juce::jlimit(0.02f, 0.45f, e.x);
                    break;

                case ManifoldEdit::Type::SetPathObjectSnapRadius:
                    if (idx >= 0 && idx < MAX_PATH_OBJECTS)
                        pathObjects_[idx].snapRadius = juce::jlimit(0.005f, 0.15f, e.x);
                    break;

                case ManifoldEdit::Type::SetPathObjectEscapeForce:
                    if (idx >= 0 && idx < MAX_PATH_OBJECTS)
                        pathObjects_[idx].escapeForce = juce::jlimit(0.0f, 5.0f, e.x);
                    break;

                // ── Trajectory path spawn / remove / edits ────────────────────
                case ManifoldEdit::Type::AddTrajectoryPath:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_TRAJECTORY_PATHS; ++k)
                        if (!trajectoryPaths_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& tp    = trajectoryPaths_[slot];
                    tp.shape    = PathShape::Circle;
                    tp.x        = e.x;
                    tp.y        = e.y;
                    tp.radius   = 0.15f;
                    tp.mode     = TrajectoryMode::AutoPlay;
                    tp.speed    = 0.5f;
                    tp.currentT = 0.0f;
                    tp.active   = true;
                    break;
                }

                case ManifoldEdit::Type::RemoveTrajectoryPath:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                    {
                        trajectoryPaths_[idx].active = false;
                        // Detach every attached object so nothing snaps to a
                        // deactivated curve next tick.
                        auto detachIfMatch = [idx](auto& arr, int count) {
                            for (int i = 0; i < count; ++i)
                                if (arr[i].trajectoryPathIndex == idx)
                                    arr[i].trajectoryPathIndex = -1;
                        };
                        detachIfMatch(emitters_,       MAX_EMITTERS);
                        detachIfMatch(fieldObjects_,   MAX_FIELD_OBJECTS);
                        detachIfMatch(timbralAnchors_, activeAnchorCount_);
                        detachIfMatch(effectZones_,    MAX_EFFECT_ZONES);
                        detachIfMatch(fluxGates_,      MAX_FLUX_GATES);
                        detachIfMatch(pathObjects_,    MAX_PATH_OBJECTS);
                        detachIfMatch(tangentPaths_,   MAX_TANGENT_PATHS);
                    }
                    break;

                case ManifoldEdit::Type::MoveTrajectoryPath:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                    {
                        trajectoryPaths_[idx].x = e.x;
                        trajectoryPaths_[idx].y = e.y;
                    }
                    break;

                case ManifoldEdit::Type::SetTrajectoryPathRadius:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                        trajectoryPaths_[idx].radius = juce::jlimit(0.02f, 0.45f, e.x);
                    break;

                case ManifoldEdit::Type::SetTrajectoryPathSpeed:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                        trajectoryPaths_[idx].speed = juce::jlimit(-4.0f, 4.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetTrajectoryPathMode:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                        trajectoryPaths_[idx].mode = static_cast<TrajectoryMode>(
                            static_cast<uint8_t>(static_cast<int>(e.x)));
                    break;

                case ManifoldEdit::Type::SetTrajectoryPathCurrentT:
                    if (idx >= 0 && idx < MAX_TRAJECTORY_PATHS)
                    {
                        // Wrap to [0, 1). Manual mode reads currentT directly;
                        // AutoPlay's advanceTrajectoryPaths will overwrite it
                        // next tick, which is the documented behavior.
                        float t = std::fmod(e.x, 1.0f);
                        if (t < 0.0f) t += 1.0f;
                        trajectoryPaths_[idx].currentT = t;
                    }
                    break;

                case ManifoldEdit::Type::SetEmitterTrajectoryPath:
                    if (idx >= 0 && idx < MAX_EMITTERS)
                    {
                        const int v = (int)e.x;
                        emitters_[idx].trajectoryPathIndex
                            = (v >= 0 && v < MAX_TRAJECTORY_PATHS) ? v : -1;
                    }
                    break;

                // Generic attachment: set <kind>[idx].trajectoryPathIndex.
                // Helper macro keeps the per-type cases minimal and identical.
                #define MORPHOS_HANDLE_ATTACH(EditType, Array, MaxN)              \
                    case ManifoldEdit::Type::EditType:                            \
                        if (idx >= 0 && idx < (MaxN))                             \
                        {                                                         \
                            const int v = (int)e.x;                               \
                            Array[idx].trajectoryPathIndex                        \
                                = (v >= 0 && v < MAX_TRAJECTORY_PATHS) ? v : -1;  \
                        }                                                         \
                        break;
                MORPHOS_HANDLE_ATTACH(SetFieldObjectTrajectoryPath,    fieldObjects_,   MAX_FIELD_OBJECTS)
                MORPHOS_HANDLE_ATTACH(SetTimbralAnchorTrajectoryPath,  timbralAnchors_, MAX_TIMBRAL_ANCHORS)
                MORPHOS_HANDLE_ATTACH(SetEffectZoneTrajectoryPath,     effectZones_,    MAX_EFFECT_ZONES)
                MORPHOS_HANDLE_ATTACH(SetFluxGateTrajectoryPath,       fluxGates_,      MAX_FLUX_GATES)
                MORPHOS_HANDLE_ATTACH(SetPathObjectTrajectoryPath,     pathObjects_,    MAX_PATH_OBJECTS)
                MORPHOS_HANDLE_ATTACH(SetTangentPathTrajectoryPath,    tangentPaths_,   MAX_TANGENT_PATHS)
                #undef MORPHOS_HANDLE_ATTACH

                // ── Tangent-force ("Flow") path spawn / remove / edits ────────
                case ManifoldEdit::Type::AddTangentPath:
                {
                    int slot = -1;
                    for (int k = 0; k < MAX_TANGENT_PATHS; ++k)
                        if (!tangentPaths_[k].active) { slot = k; break; }
                    if (slot < 0) break;

                    auto& tp     = tangentPaths_[slot];
                    tp.shape     = PathShape::Circle;
                    tp.x         = e.x;
                    tp.y         = e.y;
                    tp.radius    = 0.15f;
                    tp.width     = 0.08f;
                    tp.strength  = 0.40f;
                    tp.chirality = 1.0f;
                    tp.active    = true;
                    break;
                }

                case ManifoldEdit::Type::RemoveTangentPath:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                        tangentPaths_[idx].active = false;
                    break;

                case ManifoldEdit::Type::MoveTangentPath:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                    {
                        tangentPaths_[idx].x = e.x;
                        tangentPaths_[idx].y = e.y;
                    }
                    break;

                case ManifoldEdit::Type::SetTangentPathRadius:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                        tangentPaths_[idx].radius = juce::jlimit(0.02f, 0.45f, e.x);
                    break;

                case ManifoldEdit::Type::SetTangentPathWidth:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                        tangentPaths_[idx].width = juce::jlimit(0.01f, 0.30f, e.x);
                    break;

                case ManifoldEdit::Type::SetTangentPathStrength:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                        tangentPaths_[idx].strength = juce::jlimit(0.0f, 2.0f, e.x);
                    break;

                case ManifoldEdit::Type::SetTangentPathChirality:
                    if (idx >= 0 && idx < MAX_TANGENT_PATHS)
                        tangentPaths_[idx].chirality = juce::jlimit(-1.0f, 1.0f, e.x);
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
    // Per-Emitter voice routing: each Emitter applies its own polyMode independently.
    // A patch can mix modes (e.g. mono bass Emitter + poly lead Emitter on the same
    // key range) — every active Emitter that matches the note range either spawns
    // or retargets one of its own voices according to its individual setting.
    for (int ei = 0; ei < MAX_EMITTERS; ++ei)
    {
        const auto& em = emitters_[ei];
        if (!em.active)                            continue;
        if (note < em.keyLow || note > em.keyHigh) continue;

        const PolyMode mode = em.polyMode;

        // ── Legato / Slur retarget ──────────────────────────────────────────────
        // Legato (gap-sensitive): retarget only a voice from this Emitter that is
        //   still held; if its prior note was already released, fall through and
        //   spawn a fresh voice (a "gap" between notes resets the slide).
        // LegatoSlur (always): retarget any of this Emitter's active voices, even
        //   ones in Release — keeps the slide connected across note gaps.
        // Both preserve Manifold position + velocity; only pitch + envelope change.
        if (mode == PolyMode::Legato || mode == PolyMode::LegatoSlur)
        {
            bool retargeted = false;
            for (auto& m : morphons_)
            {
                if (!m.active || m.midiChannel != channel || m.emitterIndex != ei)
                    continue;
                if (mode == PolyMode::Legato && m.noteReleased)
                    continue;   // Gap: voice released before new note — spawn fresh

                const float xp = std::pow(2.0f, em.transposeOct
                                                + em.transposeSemi / 12.0f
                                                + em.transposeCents / 1200.0f);
                const float newHz     = midiNoteToHz(note) * xp;
                m.targetFundamentalHz = newHz;
                if (globalGlideTimeSec_ <= 0.0f)
                    m.fundamentalHz = newHz;   // Instant pitch when glide is off

                m.midiNote     = note;
                m.noteReleased = false;
                m.envStage     = EnvelopeStage::Attack;
                m.age          = 0.0f;
                retargeted     = true;
                break;
            }
            if (retargeted)
            {
                pushHeldNote(ei, note);   // Track for legato fall-back on note-off
                continue;                 // Next Emitter; this one handled its note
            }
        }

        // ── Mono / Legato gap / Slur gap: release this Emitter's other voices ──
        // Mono always releases; Legato/Slur reach here only when no retarget was
        // possible (i.e. fall-through case). Filter by emitter index so other
        // Emitters' voices are untouched.
        if (mode != PolyMode::Polyphonic)
        {
            for (auto& m : morphons_)
                if (m.active && m.midiChannel == channel && m.emitterIndex == ei)
                    m.noteReleased = true;
        }
        else
        {
            // Polyphonic same-note retrigger from this Emitter: release the prior
            // voice gracefully and spawn a fresh one (clean Attack, no click).
            for (int i = 0; i < MAX_MORPHONS; ++i)
            {
                auto& m = morphons_[i];
                if (m.active && m.midiNote == note
                    && m.midiChannel == channel && m.emitterIndex == ei)
                {
                    m.noteReleased  = true;
                    m.terminusArmed = false;
                    break;
                }
            }
        }

        const int slot = findFreeSlot();
        if (slot < 0) continue;   // Pool full — skip this Emitter, try the next

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
        m.pathIndex          = -1;   // Fresh voice starts unpinned
        {
            const float newHz = midiNoteToHz(note)
                * std::pow(2.0f, em.transposeOct
                               + em.transposeSemi / 12.0f
                               + em.transposeCents / 1200.0f);
            m.fundamentalHz       = newHz;  // Fresh spawn: always instant (no glide)
            m.targetFundamentalHz = newHz;
        }

        pushHeldNote(ei, note);   // Track for legato fall-back on note-off
    }
}

// Move-to-top push: if `note` is already in this Emitter's stack, remove the
// old entry so the new push goes to the top — keeps the stack a "set of held
// notes with insertion-order tracking" rather than a literal event log.
void PhysicsEngine::pushHeldNote(int ei, int note) noexcept
{
    if (ei < 0 || ei >= MAX_EMITTERS) return;
    auto& stack = emitterHeldNotes_[ei];
    int&  count = emitterHeldCount_[ei];

    int kept = 0;
    for (int i = 0; i < count; ++i)
        if (stack[i] != note)
            stack[kept++] = stack[i];
    count = kept;

    if (count < LEGATO_STACK_SIZE)
        stack[count++] = note;
    else
    {
        // Stack full — drop the oldest entry to make room.
        for (int i = 0; i < LEGATO_STACK_SIZE - 1; ++i)
            stack[i] = stack[i + 1];
        stack[LEGATO_STACK_SIZE - 1] = note;
    }
}

// Remove ALL instances of `note` from the stack (handles any leftover dupes
// from earlier polyphonic retrigger pushes). Returns true if at least one
// entry was removed — i.e. this Emitter was tracking the note.
bool PhysicsEngine::popHeldNote(int ei, int note) noexcept
{
    if (ei < 0 || ei >= MAX_EMITTERS) return false;
    auto& stack = emitterHeldNotes_[ei];
    int&  count = emitterHeldCount_[ei];

    int kept = 0;
    for (int i = 0; i < count; ++i)
        if (stack[i] != note)
            stack[kept++] = stack[i];

    const bool removed = (kept != count);
    count = kept;
    return removed;
}

void PhysicsEngine::handleNoteOff(int channel, int note)
{
    // Per-Emitter handling: each Emitter pops the note from its own held-note
    // stack. Legato/Slur Emitters with notes still held retarget to the new top
    // (last-note priority) instead of releasing — this is the "voice falls back
    // to the held lower note" behaviour expected from traditional MIDI legato.
    for (int ei = 0; ei < MAX_EMITTERS; ++ei)
    {
        const auto& em = emitters_[ei];
        if (!em.active) continue;

        if (!popHeldNote(ei, note)) continue;  // This Emitter wasn't tracking it

        const int    count = emitterHeldCount_[ei];
        const PolyMode mode = em.polyMode;

        // Legato fall-back: notes still held → retarget the voice rather than release.
        if (count > 0 && (mode == PolyMode::Legato || mode == PolyMode::LegatoSlur))
        {
            const int   topNote = emitterHeldNotes_[ei][count - 1];
            const float xp      = std::pow(2.0f, em.transposeOct
                                                 + em.transposeSemi / 12.0f
                                                 + em.transposeCents / 1200.0f);
            const float newHz   = midiNoteToHz(topNote) * xp;

            for (auto& m : morphons_)
            {
                if (!m.active || m.midiChannel != channel || m.emitterIndex != ei)
                    continue;
                m.targetFundamentalHz = newHz;
                if (globalGlideTimeSec_ <= 0.0f)
                    m.fundamentalHz = newHz;   // Instant when glide is off
                m.midiNote = topNote;
                break;
            }
            continue;   // Don't release — voice keeps sounding the fall-back note
        }

        // Otherwise: release this Emitter's voices matching `note`.
        // (Filtering by emitter index keeps releases scoped: if two Emitters had
        // overlapping key ranges, each releases only its own voice.)
        for (auto& m : morphons_)
        {
            if (!m.active || m.midiNote != note || m.midiChannel != channel) continue;
            if (m.emitterIndex != ei) continue;

            m.noteReleased = true;

            // Arm Terminus only if the morphon is currently OUTSIDE the arrival zone.
            // This prevents the "immediate-fire" click that occurred when the morphon
            // was already inside the radius at the moment of note-off — the terminus
            // should only activate on a crossing event (outside → inside), not a
            // state check (already inside at note-off).
            if (em.terminusEnabled)
            {
                const float dx    = m.x - em.x;
                const float dy    = m.y - em.y;
                const float dist2 = dx * dx + dy * dy;
                const float ar    = em.terminusArrivalRadius;
                m.terminusArmed   = (dist2 >= ar * ar);
            }
            else
            {
                m.terminusArmed = false;
            }
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
// Mod matrix
//
// Each tick, every active connection samples its source (normalised to [0,1])
// and writes the dest = base + (src − 0.5) × 2 × depth. Sources that reference
// a removed/inactive object return the neutral 0.5, so a stale connection is
// effectively silent rather than pinning its dest at one rail. FieldObject
// param writes flag the field grid dirty so the rebuild call later in the
// tick picks them up.
// ─────────────────────────────────────────────────────────────────────────────

float PhysicsEngine::readModSource(ModSourceType type, int index) const
{
    switch (type)
    {
        case ModSourceType::TrajectoryT:
            if (index >= 0 && index < MAX_TRAJECTORY_PATHS && trajectoryPaths_[index].active)
                return juce::jlimit(0.0f, 1.0f, trajectoryPaths_[index].currentT);
            break;

        case ModSourceType::TrajectoryX:
        case ModSourceType::TrajectoryY:
            if (index >= 0 && index < MAX_TRAJECTORY_PATHS && trajectoryPaths_[index].active)
            {
                float tx, ty;
                trajectoryPathSample(trajectoryPaths_[index], trajectoryPaths_[index].currentT, tx, ty);
                return juce::jlimit(0.0f, 1.0f,
                    type == ModSourceType::TrajectoryX ? tx : ty);
            }
            break;

        case ModSourceType::MidiCC:
            if (index >= 0 && index < 128)
                return midiCC_[index] / 127.0f;
            break;

        case ModSourceType::Keytrack:
            return lastNote_ / 127.0f;

        case ModSourceType::Velocity:
            return lastVelocity_ / 127.0f;

        case ModSourceType::Macro:
            if (index >= 0 && index < NUM_MACROS)
                return juce::jlimit(0.0f, 1.0f,
                    macroValues_[index].load(std::memory_order_relaxed));
            break;

        case ModSourceType::None: default: break;
    }
    return 0.5f;   // Neutral — bipolar mid-point so an inactive source contributes nothing.
}

bool PhysicsEngine::writeModDest(ModDestType type, int index, float value)
{
    // Generic per-object X/Y writers. Position values are clamped to [0,1]
    // manifold coords. For FieldObject moves the field grid must be rebuilt
    // (forces depend on position); other object types don't cache force fields.
    auto writeObjX = [&](auto& arr, int n, bool dirtyGrid) -> bool {
        if (index >= 0 && index < n)
        {
            arr[index].x = juce::jlimit(0.0f, 1.0f, value);
            if (dirtyGrid) fieldGrid_.dirty = true;
            return true;
        }
        return false;
    };
    auto writeObjY = [&](auto& arr, int n, bool dirtyGrid) -> bool {
        if (index >= 0 && index < n)
        {
            arr[index].y = juce::jlimit(0.0f, 1.0f, value);
            if (dirtyGrid) fieldGrid_.dirty = true;
            return true;
        }
        return false;
    };

    switch (type)
    {
        // ── Object positions ──────────────────────────────────────────────────
        case ModDestType::FieldObjectX:   return writeObjX(fieldObjects_,   MAX_FIELD_OBJECTS,    true);
        case ModDestType::FieldObjectY:   return writeObjY(fieldObjects_,   MAX_FIELD_OBJECTS,    true);
        case ModDestType::EmitterX:       return writeObjX(emitters_,       MAX_EMITTERS,         false);
        case ModDestType::EmitterY:       return writeObjY(emitters_,       MAX_EMITTERS,         false);
        case ModDestType::EmitterLaunchAngle:
            if (index >= 0 && index < MAX_EMITTERS)
            {
                // Wrap to (-π, +π] so a modulator overshooting the endpoints
                // produces a smooth full rotation instead of clamping.
                const float pi = juce::MathConstants<float>::pi;
                float a = std::fmod(value + pi, 2.0f * pi);
                if (a < 0.0f) a += 2.0f * pi;
                emitters_[index].launchAngle = a - pi;
                return true;
            }
            break;
        case ModDestType::EmitterLaunchSpeed:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].launchSpeed = juce::jlimit(0.0f, 2.0f, value); return true; }
            break;
        case ModDestType::EmitterSpawnMass:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].spawnMass = juce::jlimit(0.1f, 4.0f, value); return true; }
            break;
        case ModDestType::EmitterAttack:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].attackTime = juce::jlimit(0.001f, 5.0f, value); return true; }
            break;
        case ModDestType::EmitterDecay:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].decayTime = juce::jlimit(0.001f, 5.0f, value); return true; }
            break;
        case ModDestType::EmitterSustain:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].sustainLevel = juce::jlimit(0.0f, 1.0f, value); return true; }
            break;
        case ModDestType::EmitterRelease:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].releaseTime = juce::jlimit(0.001f, 5.0f, value); return true; }
            break;
        case ModDestType::EmitterTransposeCents:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].transposeCents = juce::jlimit(-100.0f, 100.0f, value); return true; }
            break;
        case ModDestType::EmitterPan:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].pan = juce::jlimit(-1.0f, 1.0f, value); return true; }
            break;
        case ModDestType::EmitterTerminusStrength:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].terminusStrength = juce::jlimit(0.0f, 2.0f, value); return true; }
            break;
        case ModDestType::EmitterTerminusRadius:
            if (index >= 0 && index < MAX_EMITTERS)
            { emitters_[index].terminusArrivalRadius = juce::jlimit(0.005f, 0.45f, value); return true; }
            break;
        case ModDestType::AnchorX:        return writeObjX(timbralAnchors_, activeAnchorCount_,   false);
        case ModDestType::AnchorY:        return writeObjY(timbralAnchors_, activeAnchorCount_,   false);
        case ModDestType::EffectZoneX:    return writeObjX(effectZones_,    MAX_EFFECT_ZONES,     false);
        case ModDestType::EffectZoneY:    return writeObjY(effectZones_,    MAX_EFFECT_ZONES,     false);
        case ModDestType::FluxGateX:      return writeObjX(fluxGates_,      MAX_FLUX_GATES,       false);
        case ModDestType::FluxGateY:      return writeObjY(fluxGates_,      MAX_FLUX_GATES,       false);
        case ModDestType::PathObjectX:    return writeObjX(pathObjects_,    MAX_PATH_OBJECTS,     false);
        case ModDestType::PathObjectY:    return writeObjY(pathObjects_,    MAX_PATH_OBJECTS,     false);
        case ModDestType::TrajectoryPathX:return writeObjX(trajectoryPaths_, MAX_TRAJECTORY_PATHS, false);
        case ModDestType::TrajectoryPathY:return writeObjY(trajectoryPaths_, MAX_TRAJECTORY_PATHS, false);
        case ModDestType::TangentPathX:   return writeObjX(tangentPaths_,   MAX_TANGENT_PATHS,    false);
        case ModDestType::TangentPathY:   return writeObjY(tangentPaths_,   MAX_TANGENT_PATHS,    false);

        case ModDestType::FieldObjectStrength:
            if (index >= 0 && index < MAX_FIELD_OBJECTS)
            {
                fieldObjects_[index].strength = juce::jlimit(-1.0f, 1.0f, value);
                fieldGrid_.dirty = true;
                return true;
            }
            break;
        case ModDestType::FieldObjectRadius:
            if (index >= 0 && index < MAX_FIELD_OBJECTS)
            {
                fieldObjects_[index].radius = juce::jlimit(0.05f, 0.95f, value);
                fieldGrid_.dirty = true;
                return true;
            }
            break;
        case ModDestType::EffectZoneDepth:
            if (index >= 0 && index < MAX_EFFECT_ZONES)
            {
                effectZones_[index].depth = juce::jlimit(-24.0f, 24.0f, value);
                return true;
            }
            break;
        case ModDestType::EffectZoneRadius:
            if (index >= 0 && index < MAX_EFFECT_ZONES)
            {
                effectZones_[index].radius = juce::jlimit(0.02f, 0.45f, value);
                return true;
            }
            break;
        case ModDestType::TrajectoryCurrentT:
            if (index >= 0 && index < MAX_TRAJECTORY_PATHS
                && trajectoryPaths_[index].mode == TrajectoryMode::Manual)
            {
                // Wrap into [0, 1) so a modulator overshooting the endpoints
                // produces a smooth loop instead of clamping at the boundary.
                float t = std::fmod(value, 1.0f);
                if (t < 0.0f) t += 1.0f;
                trajectoryPaths_[index].currentT = t;
                return true;
            }
            break;
        case ModDestType::AnchorTimbreX:
            if (index >= 0 && index < MAX_TIMBRAL_ANCHORS)
            {
                timbralAnchors_[index].timbreX = juce::jlimit(0.0f, 1.0f, value);
                return true;
            }
            break;
        case ModDestType::AnchorTimbreY:
            if (index >= 0 && index < MAX_TIMBRAL_ANCHORS)
            {
                timbralAnchors_[index].timbreY = juce::jlimit(0.0f, 1.0f, value);
                return true;
            }
            break;
        case ModDestType::GlobalFriction:
            globalFriction_ = juce::jlimit(0.0f, 10.0f, value);
            return true;
        case ModDestType::None: default: break;
    }
    return false;
}

int PhysicsEngine::findActiveConnectionForDest(ModDestType type, int index) const noexcept
{
    if (type == ModDestType::None) return -1;
    for (int i = 0; i < MAX_MOD_CONNECTIONS; ++i)
    {
        const auto& c = modConnections_[i];
        if (c.active && c.dstType == type && c.dstIndex == index)
            return i;
    }
    return -1;
}

void PhysicsEngine::evaluateModMatrix()
{
    for (auto& c : modConnections_)
    {
        if (!c.active) continue;
        if (c.srcType == ModSourceType::None || c.dstType == ModDestType::None) continue;

        const float src    = readModSource(c.srcType, c.srcIndex);
        // Bipolar around 0.5, scaled to the destination's musically meaningful
        // full-swing range so depth = ±1 = full sweep regardless of dest units.
        const float offset = (src - 0.5f) * 2.0f * c.depth * perDestSwing(c.dstType);
        writeModDest(c.dstType, c.dstIndex, c.base + offset);
    }
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

        // Snapshot the entry position so applyFluxGates can test the per-tick
        // trajectory segment (prev → cur) against gate lines later in the tick.
        m.prevX = m.x;
        m.prevY = m.y;

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

        // ── Tangent-force ("Flow") paths ───────────────────────────────────────
        // Each active Flow path adds a tangential push + centering pull when the
        // Morphon is inside its influence band. These are soft forces (unlike the
        // rail constraint), so a strong field elsewhere can still pull the Morphon
        // out of the stream.
        for (const auto& tp : tangentPaths_)
        {
            if (!tp.active) continue;
            tangentPathForce(tp, m.x, m.y, fx, fy);
        }

        // F = ma  →  a = F/mass
        const float ax = fx / m.mass;
        const float ay = fy / m.mass;

        // Semi-implicit Euler with drag:
        //   v' = v*(1 - drag) + a*dt
        //   x' = x + v'*dt
        // globalFriction_ is the only damping authority and now reads as a
        // decay rate per SECOND (1/s) rather than per-tick. The per-tick
        // factor below is the exponential conversion: damping = 1 - exp(−r·dt).
        // At r = 0 → damping = 0 (free coasting); r = 1 → ~37% velocity
        // retained after 1 s; r = 10 → essentially fully damped after 1 s.
        // (m.drag is kept on the struct for patch-format compatibility but is
        // ignored — the single Friction slider is the only damping knob.)
        const float rate    = juce::jmax(0.0f, globalFriction_);
        const float damping = (rate > 0.0f) ? (1.0f - std::exp(-rate * dtF)) : 0.0f;
        m.vx = m.vx * (1.0f - damping) + ax * dtF;
        m.vy = m.vy * (1.0f - damping) + ay * dtF;

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
// Trajectory paths — parameter advance + Emitter position update
//
// advanceTrajectoryPaths: every AutoPlay path's currentT advances by
//   speed * dt and wraps modulo 1.0 (closed shapes only for v1). Manual mode
//   paths are skipped — their currentT is driven externally (Phase 6 mod
//   matrix destination).
//
// updateAttachedEmitters: every Emitter with trajectoryPathIndex >= 0 has its
//   (x, y) sampled from the path's current parameter. If the path was removed
//   between ticks, the Emitter detaches and resumes its previous static
//   position (held over from before the path was deleted).
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::advanceTrajectoryPaths(double dt)
{
    const float dtF = static_cast<float>(dt);

    for (auto& tp : trajectoryPaths_)
    {
        if (!tp.active) continue;
        if (tp.mode != TrajectoryMode::AutoPlay) continue;

        tp.currentT += tp.speed * dtF;

        // Wrap to [0, 1). Use fmod to handle large jumps cleanly (e.g. extreme
        // speed on the first frame after a session resume).
        tp.currentT = std::fmod(tp.currentT, 1.0f);
        if (tp.currentT < 0.0f) tp.currentT += 1.0f;
    }
}

void PhysicsEngine::updateAttachedEmitters()
{
    // Generalised across every attachable object type. The contract for each
    // type: it has `.active`, `.x`, `.y`, and `.trajectoryPathIndex` fields.
    // Any object with trajectoryPathIndex >= 0 gets its (x, y) sampled from
    // the path's current t each tick; if the path was removed under it, the
    // object detaches and holds its last position.
    auto driveAttached = [this](auto& arrayLike, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            auto& o = arrayLike[i];
            if (!o.active) continue;
            const int ti = o.trajectoryPathIndex;
            if (ti < 0 || ti >= MAX_TRAJECTORY_PATHS) continue;

            const auto& tp = trajectoryPaths_[ti];
            if (!tp.active)
            {
                o.trajectoryPathIndex = -1;
                continue;
            }

            float tx, ty;
            trajectoryPathSample(tp, tp.currentT, tx, ty);
            o.x = tx;
            o.y = ty;
        }
    };

    driveAttached(emitters_,        MAX_EMITTERS);
    driveAttached(timbralAnchors_,  activeAnchorCount_);
    driveAttached(effectZones_,     MAX_EFFECT_ZONES);
    driveAttached(fluxGates_,       MAX_FLUX_GATES);
    driveAttached(pathObjects_,     MAX_PATH_OBJECTS);
    driveAttached(tangentPaths_,    MAX_TANGENT_PATHS);

    // FieldObjects feed a precomputed force-grid; if we move any of them via
    // a trajectory attachment, the grid must be invalidated so the next call
    // to rebuildFieldGridIfDirty (later in the same tick) rebuilds it with the
    // updated positions. Without this, the visual glyph slides along the
    // trajectory but the actual force field stays at the original location.
    for (int i = 0; i < MAX_FIELD_OBJECTS; ++i)
    {
        auto& o = fieldObjects_[i];
        if (!o.active) continue;
        const int ti = o.trajectoryPathIndex;
        if (ti < 0 || ti >= MAX_TRAJECTORY_PATHS) continue;
        const auto& tp = trajectoryPaths_[ti];
        if (!tp.active)
        {
            o.trajectoryPathIndex = -1;
            continue;
        }
        float tx, ty;
        trajectoryPathSample(tp, tp.currentT, tx, ty);
        o.x = tx;
        o.y = ty;
        fieldGrid_.dirty = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Path constraints — rail pinning + tangent projection
//
// Two passes per tick after integration:
//   1. Each unpinned active Morphon is checked against every active path; if
//      its post-integration position is within a path's snap radius, the
//      Morphon pins to that path (pathIndex = i).
//   2. Each pinned Morphon has its position snapped to the closest point on
//      its path and its velocity projected onto the local tangent. Field
//      forces still applied through the integrator survive only in their
//      tangential component, so the Morphon walks along the rail.
//
// If a path is deleted (active = false), drainEditCommands clears pathIndex
// for all Morphons attached to it, so they resume free motion the next tick.
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::applyPathConstraints()
{
    // Cheap activity check — skip per-Morphon iteration if no paths are active.
    bool anyActive = false;
    for (const auto& p : pathObjects_)
        if (p.active) { anyActive = true; break; }
    if (!anyActive)
    {
        // Still need to unpin any Morphons whose paths got removed without
        // the edit command running this tick. (Defensive — drainEditCommands
        // already handles RemovePathObject, but if all paths went away another
        // way, this keeps state consistent.)
        for (auto& m : morphons_)
            if (m.pathIndex >= 0)
                m.pathIndex = -1;
        return;
    }

    // Pass 1: pin Morphons that entered any path's snap zone this tick.
    for (auto& m : morphons_)
    {
        if (!m.active || m.pathIndex >= 0) continue;

        for (int i = 0; i < MAX_PATH_OBJECTS; ++i)
        {
            const auto& p = pathObjects_[i];
            if (!p.active) continue;

            float px, py, tx, ty;
            pathClosestPoint(p, m.x, m.y, px, py, tx, ty);

            const float dx = m.x - px;
            const float dy = m.y - py;
            if (dx * dx + dy * dy > p.snapRadius * p.snapRadius)
                continue;

            // On escape-enabled rails, require the Morphon to actually be moving
            // toward the contact point. Without this, a Morphon that just
            // escaped outward would re-pin on the very next tick while it is
            // still inside the snap zone but heading away.
            if (p.escapeForce > 0.0f)
            {
                const float toContactX = px - m.x;
                const float toContactY = py - m.y;
                const float vTowardPath = m.vx * toContactX + m.vy * toContactY;
                if (vTowardPath <= 0.0f) continue;
            }

            m.pathIndex = i;
            m.x = px;
            m.y = py;
            // Project current velocity onto tangent; discard normal component.
            const float vDotT = m.vx * tx + m.vy * ty;
            m.vx = tx * vDotT;
            m.vy = ty * vDotT;
            break;
        }
    }

    // Pass 2: snap pinned Morphons to their path each tick. Escape-enabled
    // rails check the perpendicular field force at the contact point; if it
    // exceeds escapeForce, the pin is released and the Morphon flies free
    // with whatever velocity it accumulated during integration.
    for (auto& m : morphons_)
    {
        if (!m.active || m.pathIndex < 0) continue;

        const auto& p = pathObjects_[m.pathIndex];
        if (!p.active)
        {
            m.pathIndex = -1;  // Path deactivated mid-flight — release the pin.
            continue;
        }

        float px, py, tx, ty;
        pathClosestPoint(p, m.x, m.y, px, py, tx, ty);

        // Escape check: re-sample the field at the contact point and decompose
        // into the outward-radial component. Skip on sticky rails (escapeForce == 0).
        if (p.escapeForce > 0.0f)
        {
            float ffx = 0.0f, ffy = 0.0f;
            fieldGrid_.sample(px, py, ffx, ffy);
            // Outward radial normal at contact = tangent rotated −90° = (ty, -tx).
            const float nx = ty;
            const float ny = -tx;
            const float fNormal = ffx * nx + ffy * ny;
            if (std::abs(fNormal) > p.escapeForce)
            {
                m.pathIndex = -1;
                continue;  // Skip snap and tangent projection — let it fly.
            }
        }

        m.x = px;
        m.y = py;
        const float vDotT = m.vx * tx + m.vy * ty;
        m.vx = tx * vDotT;
        m.vy = ty * vDotT;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Flux Gates — crossing detection + envelope re-trigger
//
// For each active Morphon, the per-tick trajectory is the line segment from
// (prevX, prevY) to (x, y). If that segment crosses any active gate's segment,
// the Morphon's envelope snaps back to Attack (terminus state cleared so a
// post-noteOff re-trigger doesn't drag toward terminus mid-burst).
//
// Released voices are skipped — updateEnvelopes would immediately switch them
// back to Release on its next pass and the gate would have no audible effect.
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::applyFluxGates()
{
    // Cheap activity check — bail before per-Morphon iteration if no gates are active.
    bool anyActive = false;
    for (const auto& g : fluxGates_)
        if (g.active) { anyActive = true; break; }
    if (!anyActive) return;

    for (auto& m : morphons_)
    {
        if (!m.active || m.noteReleased) continue;
        if (m.prevX == m.x && m.prevY == m.y) continue;  // No movement, no crossing

        for (const auto& g : fluxGates_)
        {
            if (!g.active) continue;

            bool crossed = false;
            if (g.shape == FluxGateShape::Line)
            {
                float ax, ay, bx, by;
                fluxGateEndpointA(g, ax, ay);
                fluxGateEndpointB(g, bx, by);
                crossed = segmentsCross(m.prevX, m.prevY, m.x, m.y, ax, ay, bx, by);
            }
            else   // Circle
            {
                crossed = segmentCrossesCircle(m.prevX, m.prevY, m.x, m.y,
                                                g.x, g.y, g.radius);
            }

            if (crossed)
            {
                m.envStage        = EnvelopeStage::Attack;
                m.terminusArmed   = false;
                m.terminusReached = false;
                break;   // Multiple gates in one tick → one re-trigger is enough
            }
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
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
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
        dst.polyMode               = src.polyMode;
        dst.trajectoryPathIndex    = src.trajectoryPathIndex;
        dst.active                 = src.active;
    }

    snap.globalBoundary  = globalBoundary_;
    snap.globalGlideTime = globalGlideTimeSec_;
    snap.globalFriction  = globalFriction_;

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
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
        dst.active  = src.active;
        if (src.active) ++activeZones;
    }
    snap.activeEffectZoneCount = activeZones;

    // Copy flux gates for UI rendering
    int activeGates = 0;
    for (int i = 0; i < MAX_FLUX_GATES; ++i)
    {
        const auto& src = fluxGates_[i];
        auto&       dst = snap.fluxGates[i];
        dst.shape    = src.shape;
        dst.x        = src.x;
        dst.y        = src.y;
        dst.length   = src.length;
        dst.angleRad = src.angleRad;
        dst.radius   = src.radius;
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
        dst.active   = src.active;
        if (src.active) ++activeGates;
    }
    snap.activeFluxGateCount = activeGates;

    // Copy path objects for UI rendering
    int activePaths = 0;
    for (int i = 0; i < MAX_PATH_OBJECTS; ++i)
    {
        const auto& src = pathObjects_[i];
        auto&       dst = snap.pathObjects[i];
        dst.shape       = src.shape;
        dst.x           = src.x;
        dst.y           = src.y;
        dst.radius      = src.radius;
        dst.snapRadius  = src.snapRadius;
        dst.escapeForce = src.escapeForce;
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
        dst.active      = src.active;
        if (src.active) ++activePaths;
    }
    snap.activePathObjectCount = activePaths;

    // Copy trajectory paths for UI rendering
    int activeTrajectories = 0;
    for (int i = 0; i < MAX_TRAJECTORY_PATHS; ++i)
    {
        const auto& src = trajectoryPaths_[i];
        auto&       dst = snap.trajectoryPaths[i];
        dst.shape    = src.shape;
        dst.x        = src.x;
        dst.y        = src.y;
        dst.radius   = src.radius;
        dst.mode     = src.mode;
        dst.speed    = src.speed;
        dst.currentT = src.currentT;
        dst.active   = src.active;
        if (src.active) ++activeTrajectories;
    }
    snap.activeTrajectoryPathCount = activeTrajectories;

    // Copy tangent-force ("Flow") paths for UI rendering
    int activeTangents = 0;
    for (int i = 0; i < MAX_TANGENT_PATHS; ++i)
    {
        const auto& src = tangentPaths_[i];
        auto&       dst = snap.tangentPaths[i];
        dst.shape     = src.shape;
        dst.x         = src.x;
        dst.y         = src.y;
        dst.radius    = src.radius;
        dst.width     = src.width;
        dst.strength  = src.strength;
        dst.chirality = src.chirality;
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
        dst.active    = src.active;
        if (src.active) ++activeTangents;
    }
    snap.activeTangentPathCount = activeTangents;

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
        dst.trajectoryPathIndex = src.trajectoryPathIndex;
        dst.active  = (i < activeAnchorCount_);
    }

    // Copy mod-matrix connections for the UI's Mod tab.
    for (int i = 0; i < MAX_MOD_CONNECTIONS; ++i)
        snap.modConnections[i] = modConnections_[i];

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
    fluxGates_          = patch.fluxGates;
    pathObjects_        = patch.pathObjects;
    trajectoryPaths_    = patch.trajectoryPaths;
    tangentPaths_       = patch.tangentPaths;
    modConnections_     = patch.modConnections;
    activeAnchorCount_  = patch.activeAnchorCount;
    globalBoundary_     = patch.boundary;
    globalGlideTimeSec_ = patch.glideTimeSec;
    globalFriction_     = patch.globalFriction;

    morphons_        = {};    // Kill all active voices from the previous patch
    emitterHeldCount_.fill(0); // Drop any tracked held notes — old patch is gone
    fieldGrid_.dirty = true;  // Force grid rebuild on first tick

    startSimulation();
}
