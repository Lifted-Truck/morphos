#pragma once
#include <atomic>
#include <array>

#include <juce_core/juce_core.h>

#include "PhysicsState.h"
#include "FieldObject.h"

// ─────────────────────────────────────────────────────────────────────────────
// Emitter — Morphon generator (Phase 1 minimal version)
//
// Full Emitter spec (key ranges, macro ranges, velocity layering, etc.)
// is implemented in Phase 3. This struct holds only the parameters needed
// to spawn and initialise a Morphon from a MIDI note-on event.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int MAX_EMITTERS = 8;

struct Emitter
{
    float           x            = 0.5f;    // Spawn position on Manifold
    float           y            = 0.5f;
    float           launchAngle  = 0.0f;    // Radians; 0 = rightward. Phase 4: vel-mappable
    float           launchSpeed  = 0.0f;    // Initial speed (Manifold units/sec); 0 = field carries
    float           spawnMass    = 1.0f;
    float           spawnDrag    = 0.02f;
    float           attackTime   = 0.05f;   // Envelope attack, seconds
    float           releaseTime  = 0.30f;   // Envelope release, seconds
    BoundaryBehavior boundary    = BoundaryBehavior::Wrap;
    bool            active       = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsEngine — dedicated simulation thread
//
// Tick loop runs at TICK_RATE_HZ. Each tick:
//   1. Drain NoteEvent queue (audio → physics)
//   2. Rebuild field grid if any object changed
//   3. Integrate all active Morphon positions
//   4. Update amplitude envelopes
//   5. Publish snapshot via triple buffer
//
// All mutable state lives on the physics thread. External threads communicate
// only via atomics (parameters) and lock-free queues (events).
// ─────────────────────────────────────────────────────────────────────────────
class PhysicsEngine : public juce::Thread
{
public:
    static constexpr double TICK_RATE_HZ      = 500.0;
    static constexpr double TICK_INTERVAL_SEC = 1.0 / TICK_RATE_HZ;

    PhysicsEngine();
    ~PhysicsEngine() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void startSimulation();
    void stopSimulation();

    // ── Audio thread interface (lock-free) ───────────────────────────────────
    bool pushNoteEvent(const NoteEvent& event) noexcept;
    const PhysicsStateSnapshot& getLatestState() noexcept;

    // ── Any thread (atomic writes) ───────────────────────────────────────────
    void setGlobalTimeScale(float scale) noexcept;

private:
    // ── juce::Thread ──────────────────────────────────────────────────────────
    void run() override;

    // ── Tick sub-steps ────────────────────────────────────────────────────────
    void tick(double dtSeconds);
    void drainNoteEvents();
    void rebuildFieldGridIfDirty();
    void integrateMorphons(double dt);
    void updateEnvelopes(double dt);
    void applyBoundary(MorphonState& m) const noexcept;
    void writeSnapshot();

    // ── Note handling (physics thread only) ──────────────────────────────────
    void handleNoteOn(int channel, int note, int velocity);
    void handleNoteOff(int channel, int note);
    int  findFreeSlot() const noexcept;   // Returns -1 if none
    int  findActiveNote(int channel, int note) const noexcept;

    // ── Communication ─────────────────────────────────────────────────────────
    PhysicsAudioBridge bridge_;

    static constexpr int EVENT_QUEUE_CAPACITY = 128;
    juce::AbstractFifo              eventFifo_{ EVENT_QUEUE_CAPACITY };
    std::array<NoteEvent, EVENT_QUEUE_CAPACITY> eventBuffer_;

    // ── Parameters (atomic) ───────────────────────────────────────────────────
    std::atomic<float> globalTimeScale_{ 1.0f };

    // ── Simulation state (physics thread only) ────────────────────────────────
    std::array<MorphonState, MAX_MORPHONS>   morphons_{};
    std::array<FieldObject,  MAX_FIELD_OBJECTS> fieldObjects_{};
    std::array<Emitter,      MAX_EMITTERS>   emitters_{};
    FieldGrid fieldGrid_;

    uint64_t tickIndex_        = 0;
    double   simulationTimeMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhysicsEngine)
};
