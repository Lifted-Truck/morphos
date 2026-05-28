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

                // ── Global manifold topology ──────────────────────────────────
                case ManifoldEdit::Type::SetGlobalBoundary:
                    globalBoundary_ = static_cast<BoundaryBehavior>(
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
            }
        }
    };

    apply(s1, n1);
    apply(s2, n2);
    editFifo_.finishedRead(n1 + n2);
}

void PhysicsEngine::handleNoteOn(int channel, int note, int /*velocity*/)
{
    // Each active Emitter whose key range contains `note` fires independently.
    // If a Morphon from that Emitter is already sounding this note, it is
    // retriggered (envelope reset, position reset to emitter origin).
    // Otherwise a new Morphon is spawned from that Emitter's slot.
    for (int ei = 0; ei < MAX_EMITTERS; ++ei)
    {
        const auto& em = emitters_[ei];
        if (!em.active)                          continue;
        if (note < em.keyLow || note > em.keyHigh) continue;

        // Find an existing Morphon owned by this Emitter for this note
        int existing = -1;
        for (int i = 0; i < MAX_MORPHONS; ++i)
        {
            const auto& m = morphons_[i];
            if (m.active && m.midiNote == note
                && m.midiChannel == channel && m.emitterIndex == ei)
            { existing = i; break; }
        }

        if (existing >= 0)
        {
            // Retrigger: reset envelope + kinematics, keep same slot
            auto& m        = morphons_[existing];
            m.noteReleased = false;
            m.envStage     = EnvelopeStage::Attack;
            m.age          = 0.0f;
            m.x            = em.x;
            m.y            = em.y;
            m.vx           = std::cosf(em.launchAngle) * em.launchSpeed;
            m.vy           = std::sinf(em.launchAngle) * em.launchSpeed;
        }
        else
        {
            const int slot = findFreeSlot();
            if (slot < 0) continue;   // Pool full; skip this Emitter

            auto& m         = morphons_[slot];
            m.active        = true;
            m.noteReleased  = false;
            m.envStage      = EnvelopeStage::Attack;
            m.midiNote      = note;
            m.midiChannel   = channel;
            m.emitterIndex  = ei;
            m.x             = em.x;
            m.y             = em.y;
            m.vx            = std::cosf(em.launchAngle) * em.launchSpeed;
            m.vy            = std::sinf(em.launchAngle) * em.launchSpeed;
            m.mass          = em.spawnMass;
            m.drag          = em.spawnDrag;
            m.amplitude     = 0.0f;
            m.age           = 0.0f;
            m.timbreX       = 0.5f;
            m.timbreY       = 0.0f;
            m.fundamentalHz = midiNoteToHz(note);
        }
    }
}

void PhysicsEngine::handleNoteOff(int channel, int note)
{
    // Release ALL Morphons matching this note+channel — there may be one per
    // Emitter if multiple Emitters had overlapping key ranges covering `note`.
    for (auto& m : morphons_)
        if (m.active && m.midiNote == note && m.midiChannel == channel)
            m.noteReleased = true;
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
                m.active = false;
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
    const auto& emitter = emitters_[0];

    const float attackRate  = (emitter.attackTime  > 0.0f)
                              ? dtF / emitter.attackTime  : 1.0f;
    const float decayRate   = (emitter.decayTime   > 0.0f)
                              ? dtF / emitter.decayTime   : 1.0f;
    const float releaseRate = (emitter.releaseTime > 0.0f)
                              ? dtF / emitter.releaseTime : 1.0f;
    const float sustainLvl  = emitter.sustainLevel;

    for (auto& m : morphons_)
    {
        if (!m.active) continue;

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
        dst.attackTime   = src.attackTime;
        dst.decayTime    = src.decayTime;
        dst.sustainLevel = src.sustainLevel;
        dst.releaseTime  = src.releaseTime;
        dst.keyLow       = src.keyLow;
        dst.keyHigh      = src.keyHigh;
        dst.active       = src.active;
    }

    snap.globalBoundary = globalBoundary_;

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
