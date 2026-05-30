#pragma once
#include <atomic>
#include <array>

#include <juce_core/juce_core.h>

#include "PhysicsState.h"
#include "EffectZone.h"
#include "FieldObject.h"
#include "FluxGate.h"
#include "PathObject.h"
#include "TangentPath.h"
#include "TrajectoryPath.h"
#include "synthesis/TimbralAnchor.h"

// ─────────────────────────────────────────────────────────────────────────────
// Emitter — Morphon generator (Phase 1 minimal version)
//
// Full Emitter spec (key ranges, macro ranges, velocity layering, etc.)
// is implemented in Phase 3. This struct holds only the parameters needed
// to spawn and initialise a Morphon from a MIDI note-on event.
// ─────────────────────────────────────────────────────────────────────────────
struct Emitter
{
    float           x            = 0.5f;    // Spawn position on Manifold
    float           y            = 0.5f;
    float           launchAngle  = 0.0f;    // Radians; 0 = rightward. Phase 4: vel-mappable
    float           launchSpeed  = 0.0f;    // Initial speed (Manifold units/sec); 0 = field carries
    float           spawnMass    = 1.0f;
    float           spawnDrag    = 0.02f;
    float           attackTime    = 0.05f;   // Envelope attack, seconds
    float           decayTime    = 0.15f;   // Envelope decay, seconds (attack peak → sustain)
    float           sustainLevel = 0.70f;   // Sustain amplitude [0..1]
    float           releaseTime  = 0.30f;   // Envelope release, seconds (after note-off)
    int             keyLow       = 0;       // Lowest MIDI note this Emitter responds to [0, 127]
    int             keyHigh      = 127;     // Highest MIDI note this Emitter responds to [0, 127]
    int             transposeOct  = 0;      // Octave offset [-4, +4]
    int             transposeSemi = 0;      // Semitone offset [-12, +12]
    float           transposeCents= 0.0f;  // Fine offset in cents [-100, +100]
    float           pan            = 0.0f;  // Stereo position [-1 L, 0 C, +1 R]
    // ── Terminus — key-off attractor ─────────────────────────────────────────
    // Phase 4: position = Emitter origin (em.x, em.y). Phase 5: Fixed canvas object.
    bool            terminusEnabled       = false;
    float           terminusStrength      = 0.30f; // Pull force magnitude (Manifold units/s²)
    float           terminusArrivalRadius = 0.04f; // Deactivate when within this distance
    PolyMode        polyMode              = PolyMode::Polyphonic;  // Per-Emitter voice routing
    int             trajectoryPathIndex   = -1;   // -1 = stationary; else attached to traj[index]
    bool            active       = false;   // Slots are inactive by default; constructor enables [0]
};

// ─────────────────────────────────────────────────────────────────────────────
// PatchState — complete serialisable snapshot of all Manifold objects and global
// topology parameters. Used for DAW session save/restore.
// ─────────────────────────────────────────────────────────────────────────────
struct PatchState
{
    std::array<FieldObject,    MAX_FIELD_OBJECTS>   fieldObjects{};
    std::array<Emitter,        MAX_EMITTERS>        emitters{};
    std::array<TimbralAnchor,  MAX_TIMBRAL_ANCHORS> timbralAnchors{};
    std::array<EffectZone,     MAX_EFFECT_ZONES>    effectZones{};
    std::array<FluxGate,       MAX_FLUX_GATES>      fluxGates{};
    std::array<PathObject,     MAX_PATH_OBJECTS>    pathObjects{};
    std::array<TrajectoryPath, MAX_TRAJECTORY_PATHS> trajectoryPaths{};
    std::array<TangentPath,    MAX_TANGENT_PATHS>    tangentPaths{};
    int              activeAnchorCount = 0;
    BoundaryBehavior boundary          = BoundaryBehavior::Wrap;
    float            glideTimeSec      = 0.0f;
    float            globalFriction    = 0.0f;
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

    // ── UI thread interface (lock-free) ──────────────────────────────────────
    // Send an edit command (object move, parameter change) from the UI thread.
    // Applied at the start of the next physics tick — no wait, no blocking.
    bool pushManifoldEdit(const ManifoldEdit& edit) noexcept;

    // Replace all Manifold state from a saved patch. Stops and restarts the
    // physics thread; call only from the message thread (DAW save/restore).
    void applyPatch(const PatchState& patch);

    // ── Any thread (atomic writes) ───────────────────────────────────────────
    void setGlobalTimeScale(float scale) noexcept;

    // ── Offline-rendering hooks ──────────────────────────────────────────────
    // DAW bounce-out / freeze runs processBlock faster than wall clock, but
    // the physics thread is locked to wall clock — left alone, bounced audio
    // would reflect a near-static Manifold. Instead the processor calls
    // setOfflineMode(true) before driving physics directly via advance(), and
    // setOfflineMode(false) when returning to live playback.
    //
    // While offline mode is true the dedicated physics thread parks in a
    // wait() loop; advance() runs tick() synchronously from the caller's
    // thread, accumulating fractional buffer durations so the simulation
    // advances by exactly `seconds` virtual time per call.
    void setOfflineMode(bool offline) noexcept;
    void advance(double seconds);

private:
    // ── juce::Thread ──────────────────────────────────────────────────────────
    void run() override;

    // ── Tick sub-steps ────────────────────────────────────────────────────────
    void tick(double dtSeconds);
    void drainNoteEvents();
    void drainEditCommands();        // Apply UI → physics edits before integration
    void rebuildFieldGridIfDirty();
    void integrateMorphons(double dt);
    void updateEnvelopes(double dt);
    void applyBoundary(MorphonState& m) const noexcept;
    void applyEffectZones();
    void applyFluxGates();
    void applyPathConstraints();
    void advanceTrajectoryPaths(double dt);
    void updateAttachedEmitters();
    void writeSnapshot();

    // ── Note handling (physics thread only) ──────────────────────────────────
    void handleNoteOn(int channel, int note, int velocity);
    void handleNoteOff(int channel, int note);
    int  findFreeSlot() const noexcept;   // Returns -1 if none

    // ── Communication ─────────────────────────────────────────────────────────
    PhysicsAudioBridge bridge_;

    // Note events: audio → physics
    static constexpr int EVENT_QUEUE_CAPACITY = 128;
    juce::AbstractFifo               eventFifo_{ EVENT_QUEUE_CAPACITY };
    std::array<NoteEvent, EVENT_QUEUE_CAPACITY> eventBuffer_;

    // Manifold edits: UI → physics
    static constexpr int EDIT_QUEUE_CAPACITY = 64;
    juce::AbstractFifo                   editFifo_{ EDIT_QUEUE_CAPACITY };
    std::array<ManifoldEdit, EDIT_QUEUE_CAPACITY> editBuffer_;

    // ── Parameters (atomic) ───────────────────────────────────────────────────
    std::atomic<float> globalTimeScale_{ 1.0f };

    // ── Offline-rendering state ───────────────────────────────────────────────
    // offlineMode_ signals the run() loop to park in wait() rather than tick.
    // offlineParked_ is set true by the thread once it is sitting in that
    // wait() — advance() spins on this so the caller never overlaps a tick.
    std::atomic<bool>  offlineMode_   { false };
    std::atomic<bool>  offlineParked_ { false };
    double             offlineAccumulator_ = 0.0;

    // ── Global manifold topology (physics thread; set via ManifoldEdit queue) ──
    BoundaryBehavior globalBoundary_    = BoundaryBehavior::Wrap;
    float            globalGlideTimeSec_ = 0.0f;   // Portamento time in seconds
    // Extra per-Morphon velocity damping applied on top of m.drag each tick.
    // Range [0, 1]; 0 = no extra friction, larger = quicker velocity decay.
    // Treated as a per-tick fraction (compounds at TICK_RATE_HZ).
    float            globalFriction_     = 0.0f;

    // ── Per-Emitter held-note stack — supports legato last-note priority ──────
    // On note-off, Legato/Slur Emitters fall back to the most recently pressed
    // note that is still held (rather than killing the voice). Each Emitter
    // keeps its own stack so per-Emitter polyMode stays self-contained.
    static constexpr int LEGATO_STACK_SIZE = 16;
    std::array<std::array<int, LEGATO_STACK_SIZE>, MAX_EMITTERS> emitterHeldNotes_{};
    std::array<int, MAX_EMITTERS>                                emitterHeldCount_{};

    void pushHeldNote(int emitterIndex, int note) noexcept;   // Move-to-top semantics
    bool popHeldNote (int emitterIndex, int note) noexcept;   // Returns true if note was present

    // ── Simulation state (physics thread only) ────────────────────────────────
    std::array<MorphonState, MAX_MORPHONS>      morphons_{};
    std::array<FieldObject,  MAX_FIELD_OBJECTS> fieldObjects_{};
    std::array<Emitter,      MAX_EMITTERS>      emitters_{};
    FieldGrid                                   fieldGrid_;

    // ── Timbral Anchors ───────────────────────────────────────────────────────
    int activeAnchorCount_ = 2;
    std::array<TimbralAnchor, MAX_TIMBRAL_ANCHORS> timbralAnchors_{};

    // ── Effect Zones ──────────────────────────────────────────────────────────
    std::array<EffectZone, MAX_EFFECT_ZONES> effectZones_{};

    // ── Flux Gates — crossing-triggered envelope re-trigger ───────────────────
    std::array<FluxGate, MAX_FLUX_GATES> fluxGates_{};

    // ── Path Objects — rail-constraint curves ─────────────────────────────────
    std::array<PathObject, MAX_PATH_OBJECTS> pathObjects_{};

    // ── Trajectory Paths — position-driver curves (drive object x,y) ──────────
    std::array<TrajectoryPath, MAX_TRAJECTORY_PATHS> trajectoryPaths_{};

    // ── Tangent-force ("Flow") paths — apply stream forces to nearby Morphons ──
    std::array<TangentPath, MAX_TANGENT_PATHS> tangentPaths_{};

    uint64_t tickIndex_        = 0;
    double   simulationTimeMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhysicsEngine)
};
