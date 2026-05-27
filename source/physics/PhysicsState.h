#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#include "FieldObject.h"

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsState.h — Shared data structures for physics ↔ audio/UI communication
//
// Threading model (unchanged from Phase 0):
//   Physics thread  → writes PhysicsStateSnapshot, publishes via triple buffer
//   Audio thread    → reads latest snapshot at start of each processBlock()
//   UI thread       → reads a relaxed copy for display (~30 Hz)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_MORPHONS = 256;

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
    std::array<MorphonState,       MAX_MORPHONS>       morphons{};
    std::array<FieldObjectSnapshot, MAX_FIELD_OBJECTS> fieldObjects{};

    int      activeMorphonCount  = 0;
    int      activeFieldObjCount = 0;
    uint64_t tickIndex           = 0;
    double   simulationTimeMs    = 0.0;
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
