#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsState.h — Shared data structures for physics ↔ audio communication
//
// Threading model:
//   Physics thread  → writes PhysicsStateSnapshot, publishes via PhysicsAudioBridge
//   Audio thread    → reads latest PhysicsStateSnapshot at start of each block
//   UI thread       → reads via a separate polling mechanism (see PhysicsEngine)
//
// Lock-free triple buffer: at any moment, three buffers exist —
//   one owned by physics (writing), one published (latest complete tick),
//   one owned by audio (reading). No buffer is ever shared between two threads.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_MORPHONS = 256;

// Per-Morphon state produced by the physics tick.
// Read by the audio thread to drive synthesis.
struct MorphonState
{
    // Manifold position, normalised [0, 1] × [0, 1]
    float x = 0.5f;
    float y = 0.5f;

    // Velocity vector (Manifold units per second)
    float vx = 0.0f;
    float vy = 0.0f;

    // Lifecycle
    float age      = 0.0f;  // Normalised 0..1 (age / lifetime)
    float lifetime = 2.0f;  // Seconds; ∞ if <= 0
    bool  active   = false;

    // MIDI identity — lets the audio thread know which voice this is
    int midiNote    = 60;
    int midiChannel = 1;

    // Amplitude from the ADSR envelope (0..1).
    // Synthesis engine multiplies by this.
    float amplitude = 0.0f;

    // Blended timbral parameters — output of Anchor RBF interpolation.
    // For Phase 2 (additive engine): timbreX = spectral rolloff, timbreY = inharmonicity.
    // Extended in later phases as more engines are added.
    float timbreX = 0.5f;
    float timbreY = 0.5f;

    // Fundamental frequency derived from MIDI note (Hz)
    float fundamentalHz = 440.0f;
};

// Full simulation snapshot — one per physics tick, triple-buffered.
struct PhysicsStateSnapshot
{
    std::array<MorphonState, MAX_MORPHONS> morphons{};
    int      activeMorphonCount = 0;
    uint64_t tickIndex          = 0;   // Monotonically increasing; useful for debugging
    double   simulationTimeMs   = 0.0; // Wall-clock simulation time in ms
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsAudioBridge — wait-free triple buffer
//
// Invariant: writeIdx_ ≠ publishedIdx_ ≠ readIdx_ at all times.
// Physics and audio each own one index exclusively; the third is the
// "hot" published buffer available for atomic swap.
// ─────────────────────────────────────────────────────────────────────────────
class PhysicsAudioBridge
{
public:
    PhysicsAudioBridge()
        : publishedIdx_(0), writeIdx_(1), readIdx_(2)
    {}

    // ── Physics thread ────────────────────────────────────────────────────────

    // Get the buffer physics should write into this tick.
    PhysicsStateSnapshot& getWriteBuffer() noexcept
    {
        return buffers_[writeIdx_];
    }

    // Call after writing to make the new state visible to the audio thread.
    // Physics receives the old published index as its next write buffer.
    void publish() noexcept
    {
        writeIdx_ = publishedIdx_.exchange(writeIdx_, std::memory_order_acq_rel);
    }

    // ── Audio thread ──────────────────────────────────────────────────────────

    // Call at the start of each processBlock(). Atomically grabs the latest
    // published snapshot. Safe to read for the entire duration of the block.
    const PhysicsStateSnapshot& getLatestForAudio() noexcept
    {
        readIdx_ = publishedIdx_.exchange(readIdx_, std::memory_order_acq_rel);
        return buffers_[readIdx_];
    }

private:
    std::array<PhysicsStateSnapshot, 3> buffers_;
    std::atomic<int> publishedIdx_;
    int writeIdx_;   // Exclusively owned by physics thread
    int readIdx_;    // Exclusively owned by audio thread
};

// ─────────────────────────────────────────────────────────────────────────────
// NoteEvent — audio thread → physics thread event queue
// ─────────────────────────────────────────────────────────────────────────────
struct NoteEvent
{
    enum class Type : uint8_t { NoteOn, NoteOff };
    Type type       = Type::NoteOff;
    int  channel    = 1;
    int  note       = 60;
    int  velocity   = 0;
};
