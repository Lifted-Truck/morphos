#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#include "FieldObject.h"
#include "synthesis/TimbralAnchor.h"   // for MAX_TIMBRAL_ANCHORS

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsState.h — Shared data structures for physics ↔ audio/UI communication
//
// Threading model (unchanged from Phase 0):
//   Physics thread  → writes PhysicsStateSnapshot, publishes via triple buffer
//   Audio thread    → reads latest snapshot at start of each processBlock()
//   UI thread       → reads a relaxed copy for display (~30 Hz)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_MORPHONS  = 256;
static constexpr int MAX_EMITTERS  = 8;

// Boundary behaviour when a Morphon exits the [0,1]×[0,1] Manifold.
enum class BoundaryBehavior : uint8_t { Wrap, Reflect, Terminate };

// Amplitude envelope state machine stages (ADSR)
enum class EnvelopeStage : uint8_t { Attack, Decay, Sustain, Release };

// ─────────────────────────────────────────────────────────────────────────────
// MorphonState — per-Morphon data shared between physics, audio, and UI
// ─────────────────────────────────────────────────────────────────────────────
struct MorphonState
{
    // ── Position & kinematics ─────────────────────────────────────────────────
    float x  = 0.5f;    // Manifold position, normalised [0,1]
    float y  = 0.5f;
    float vx = 0.0f;    // Velocity (Manifold units per second)
    float vy = 0.0f;

    // ── Physics parameters ────────────────────────────────────────────────────
    float mass = 1.0f;  // Resistance to field forces
    float drag = 0.02f; // Velocity dissipation per tick (0 = frictionless, 1 = instant stop)

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    float            age          = 0.0f;   // Seconds alive
    bool             active       = false;
    bool             noteReleased = false;  // true after note-off; triggers Release stage
    BoundaryBehavior boundary     = BoundaryBehavior::Wrap;

    // ── Envelope state ────────────────────────────────────────────────────────
    EnvelopeStage envStage = EnvelopeStage::Attack;

    // ── MIDI identity ─────────────────────────────────────────────────────────
    int midiNote    = 60;
    int midiChannel = 1;

    // ── Amplitude envelope ────────────────────────────────────────────────────
    // Computed by physics tick via ADSR state machine; read by audio engine each buffer.
    float amplitude = 0.0f;   // 0..1, authoritative value for synthesis scaling

    // ── Timbral parameters ────────────────────────────────────────────────────
    // Output of Timbral Anchor RBF blending at current Manifold position.
    // Phase 2: timbreX = spectral rolloff, timbreY = inharmonicity.
    // Extended in later phases.
    float timbreX = 0.5f;
    float timbreY = 0.5f;

    // Fundamental frequency derived from midiNote (Hz)
    float fundamentalHz = 440.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// EmitterSnapshot — Emitter position and launch params for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct EmitterSnapshot
{
    float x            = 0.5f;
    float y            = 0.5f;
    float launchAngle  = 0.0f;   // radians; used to draw direction arrow
    float launchSpeed  = 0.0f;   // scales arrow length
    float attackTime   = 0.05f;
    float decayTime    = 0.15f;
    float sustainLevel = 0.70f;
    float releaseTime  = 0.30f;
    bool  active       = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// TimbralAnchorSnapshot — Anchor position and timbre params for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct TimbralAnchorSnapshot
{
    float x       = 0.5f;
    float y       = 0.5f;
    float timbreX = 0.5f;   // spectral rolloff [0,1]
    float timbreY = 0.0f;   // inharmonicity    [0,1]
    bool  active  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldObjectSnapshot — lightweight field object data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct FieldObjectSnapshot
{
    FieldObjectType type      = FieldObjectType::Attractor;
    float           x         = 0.5f;
    float           y         = 0.5f;
    float           strength  = 0.0f;
    float           radius    = 0.0f;
    float           chirality = 1.0f;
    bool            active    = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsStateSnapshot — one complete physics tick output
// Triple-buffered between physics and audio threads.
// ─────────────────────────────────────────────────────────────────────────────
struct PhysicsStateSnapshot
{
    std::array<MorphonState,           MAX_MORPHONS>         morphons{};
    std::array<FieldObjectSnapshot,    MAX_FIELD_OBJECTS>    fieldObjects{};
    std::array<EmitterSnapshot,        MAX_EMITTERS>         emitters{};
    std::array<TimbralAnchorSnapshot,  MAX_TIMBRAL_ANCHORS>  timbralAnchors{};

    int      activeMorphonCount       = 0;
    int      activeFieldObjCount      = 0;
    int      activeTimbralAnchorCount = 0;
    uint64_t tickIndex                = 0;
    double   simulationTimeMs         = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ManifoldEdit — UI thread → physics thread edit command
//
// Sent via a lock-free SPSC queue; applied at the start of the next physics
// tick. Keeps the UI responsive (no waiting for physics) while guaranteeing
// that object state is mutated only on the physics thread.
//
// Scalar edits (non-Move types) carry the new value in `x`; `y` is unused.
// Move edits carry the new Manifold position in (x, y).
// ─────────────────────────────────────────────────────────────────────────────
struct ManifoldEdit
{
    enum class Type : uint8_t
    {
        // Position edits — x,y carry new Manifold coords [0,1]
        MoveFieldObject,
        MoveEmitter,
        MoveTimbralAnchor,

        // Scalar property edits — x carries new value, y unused
        SetFieldObjectStrength,
        SetFieldObjectRadius,
        SetFieldObjectChirality,
        SetEmitterLaunchAngle,
        SetEmitterLaunchSpeed,
        SetEmitterAttack,
        SetEmitterDecay,
        SetEmitterSustain,
        SetEmitterRelease,
        SetTimbralAnchorTimbreX,
        SetTimbralAnchorTimbreY,
    };

    Type  type  = Type::MoveFieldObject;
    int   index = 0;
    float x     = 0.0f;
    float y     = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsAudioBridge — wait-free triple buffer (unchanged from Phase 0)
// ─────────────────────────────────────────────────────────────────────────────
class PhysicsAudioBridge
{
public:
    PhysicsAudioBridge()
        : publishedIdx_(0), writeIdx_(1), readIdx_(2)
    {}

    PhysicsStateSnapshot& getWriteBuffer() noexcept
    {
        return buffers_[writeIdx_];
    }

    void publish() noexcept
    {
        writeIdx_ = publishedIdx_.exchange(writeIdx_, std::memory_order_acq_rel);
    }

    const PhysicsStateSnapshot& getLatestForAudio() noexcept
    {
        readIdx_ = publishedIdx_.exchange(readIdx_, std::memory_order_acq_rel);
        return buffers_[readIdx_];
    }

private:
    std::array<PhysicsStateSnapshot, 3> buffers_;
    std::atomic<int> publishedIdx_;
    int writeIdx_;
    int readIdx_;
};

// ─────────────────────────────────────────────────────────────────────────────
// NoteEvent — audio thread → physics thread (unchanged from Phase 0)
// ─────────────────────────────────────────────────────────────────────────────
struct NoteEvent
{
    enum class Type : uint8_t { NoteOn, NoteOff };
    Type type     = Type::NoteOff;
    int  channel  = 1;
    int  note     = 60;
    int  velocity = 0;
};
