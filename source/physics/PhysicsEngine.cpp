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
    // One of each type, arranged to produce visually interesting trajectories
    // so Phase 1 is immediately testable. Phase 3+: replaced by user placement.

    // Attractor — upper-left; pulls Morphons gently toward that region
    fieldObjects_[0].type     = FieldObjectType::Attractor;
    fieldObjects_[0].x        = 0.30f;
    fieldObjects_[0].y        = 0.30f;
    fieldObjects_[0].strength = 0.22f;
    fieldObjects_[0].radius   = 0.50f;
    fieldObjects_[0].active   = true;

    // Repeller — lower-right; pushes Morphons away from that corner
    fieldObjects_[1].type     = FieldObjectType::Repeller;
    fieldObjects_[1].x        = 0.72f;
    fieldObjects_[1].y        = 0.72f;
    fieldObjects_[1].strength = 0.18f;
    fieldObjects_[1].radius   = 0.40f;
    fieldObjects_[1].active   = true;

    // Vortex — centre; imparts CCW spin, combines with Attractor for orbital paths
    fieldObjects_[2].type     = FieldObjectType::Vortex;
    fieldObjects_[2].x        = 0.50f;
    fieldObjects_[2].y        = 0.50f;
    fieldObjects_[2].strength = 0.15f;
    fieldObjects_[2].radius   = 0.60f;
    fieldObjects_[2].chirality = 1.0f;  // CCW
    fieldObjects_[2].active   = true;

    // ── Default emitter ───────────────────────────────────────────────────────
    emitters_[0].x           = 0.50f;
    emitters_[0].y           = 0.50f;
    emitters_[0].launchSpeed = 0.0f;   // Stationary start; field carries the Morphon
    emitters_[0].spawnMass   = 1.0f;
    emitters_[0].spawnDrag   = 0.015f;
    emitters_[0].attackTime  = 0.05f;
    emitters_[0].releaseTime = 0.35f;
    emitters_[0].boundary    = BoundaryBehavior::Wrap;
    emitters_[0].active      = true;

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

void PhysicsEngine::handleNoteOn(int channel, int note, int /*velocity*/)
{
    // If the same note is already sounding, retrigger it
    const int existing = findActiveNote(channel, note);
    if (existing >= 0)
    {
        morphons_[existing].noteReleased = false;
        morphons_[existing].age         = 0.0f;
        morphons_[existing].x           = emitters_[0].x;
        morphons_[existing].y           = emitters_[0].y;
        morphons_[existing].vx          = std::cosf(emitters_[0].launchAngle)
                                          * emitters_[0].launchSpeed;
        morphons_[existing].vy          = std::sinf(emitters_[0].launchAngle)
                                          * emitters_[0].launchSpeed;
        return;
    }

    const int slot = findFreeSlot();
    if (slot < 0) return;   // Polyphony cap reached — Phase 4 adds voice stealing

    auto& m        = morphons_[slot];
    m.active       = true;
    m.noteReleased = false;
    m.midiNote     = note;
    m.midiChannel  = channel;
    m.x            = emitters_[0].x;
    m.y            = emitters_[0].y;
    m.vx           = std::cosf(emitters_[0].launchAngle) * emitters_[0].launchSpeed;
    m.vy           = std::sinf(emitters_[0].launchAngle) * emitters_[0].launchSpeed;
    m.mass         = emitters_[0].spawnMass;
    m.drag         = emitters_[0].spawnDrag;
    m.boundary     = emitters_[0].boundary;
    m.amplitude    = 0.0f;
    m.age          = 0.0f;
    m.timbreX      = emitters_[0].x;
    m.timbreY      = emitters_[0].y;
    m.fundamentalHz = midiNoteToHz(note);
}

void PhysicsEngine::handleNoteOff(int channel, int note)
{
    const int slot = findActiveNote(channel, note);
    if (slot >= 0)
        morphons_[slot].noteReleased = true;
}

int PhysicsEngine::findFreeSlot() const noexcept
{
    for (int i = 0; i < MAX_MORPHONS; ++i)
        if (!morphons_[i].active)
            return i;
    return -1;
}

int PhysicsEngine::findActiveNote(int channel, int note) const noexcept
{
    for (int i = 0; i < MAX_MORPHONS; ++i)
        if (morphons_[i].active
            && morphons_[i].midiNote    == note
            && morphons_[i].midiChannel == channel)
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

        // Update timbral parameters from position.
        // Phase 2+: replaced by Timbral Anchor RBF blending.
        m.timbreX = m.x;
        m.timbreY = m.y;
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

    switch (m.boundary)
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
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Envelope — simple attack / release (Phase 2+: full ADSR)
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::updateEnvelopes(double dt)
{
    const float dtF         = static_cast<float>(dt);
    const auto& emitter     = emitters_[0];

    const float attackRate  = (emitter.attackTime  > 0.0f)
                              ? dtF / emitter.attackTime  : 1.0f;
    const float releaseRate = (emitter.releaseTime > 0.0f)
                              ? dtF / emitter.releaseTime : 1.0f;

    for (auto& m : morphons_)
    {
        if (!m.active) continue;

        if (!m.noteReleased)
        {
            m.amplitude = std::min(1.0f, m.amplitude + attackRate);
        }
        else
        {
            m.amplitude = std::max(0.0f, m.amplitude - releaseRate);
            if (m.amplitude <= 0.0f)
                m.active = false;
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

    bridge_.publish();
}
