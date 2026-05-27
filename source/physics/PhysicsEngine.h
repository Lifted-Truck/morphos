#pragma once
#include <atomic>
#include <array>

#include <juce_core/juce_core.h>

#include "PhysicsState.h"

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsEngine — dedicated physics simulation thread
//
// Runs at TICK_RATE_HZ (500 Hz by default). On each tick it:
//   1. Drains the NoteEvent queue (note-on/off from the audio thread)
//   2. Integrates Morphon positions under field forces
//   3. Evaluates Timbral Anchor blending (RBF)
//   4. Publishes a PhysicsStateSnapshot via the triple-buffer bridge
//
// Currently (Phase 0): the tick loop is structurally complete but produces
// only an empty snapshot. Morphon logic is added in Phase 1–2.
//
// Thread safety:
//   - Parameters (globalTimeScale_, etc.) are written from the audio/UI thread
//     via std::atomic. No locks required.
//   - Note events travel audio → physics via a lock-free SPSC queue.
//   - Snapshot travels physics → audio via PhysicsAudioBridge (triple buffer).
// ─────────────────────────────────────────────────────────────────────────────

class PhysicsEngine : public juce::Thread
{
public:
    // Target physics tick rate. Exposed so the audio thread can compute
    // interpolation coefficients. Lowering this trades accuracy for CPU.
    static constexpr double TICK_RATE_HZ      = 500.0;
    static constexpr double TICK_INTERVAL_SEC = 1.0 / TICK_RATE_HZ;

    PhysicsEngine();
    ~PhysicsEngine() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void startSimulation();
    void stopSimulation();

    // ── Called from audio thread (lock-free) ──────────────────────────────────

    // Push a note event into the SPSC queue. Called from processBlock().
    // Returns false and drops the event if the queue is full (should not
    // happen in practice; queue capacity is generous).
    bool pushNoteEvent(const NoteEvent& event) noexcept;

    // Grab the latest physics snapshot for synthesis.
    // Call once at the start of processBlock(); hold reference for that block.
    const PhysicsStateSnapshot& getLatestState() noexcept;

    // ── Called from audio or UI thread (atomic writes) ────────────────────────

    void setGlobalTimeScale(float scale) noexcept;

private:
    // ── juce::Thread ──────────────────────────────────────────────────────────
    void run() override;

    // ── Internal tick ─────────────────────────────────────────────────────────
    void tick(double dtSeconds);
    void drainNoteEvents();

    // ── Communication ─────────────────────────────────────────────────────────
    PhysicsAudioBridge bridge_;

    // Note event queue: audio → physics (SPSC, lock-free)
    static constexpr int EVENT_QUEUE_CAPACITY = 128;
    juce::AbstractFifo              eventFifo_{ EVENT_QUEUE_CAPACITY };
    std::array<NoteEvent, EVENT_QUEUE_CAPACITY> eventBuffer_;

    // ── Parameters (atomic; written from any thread) ──────────────────────────
    std::atomic<float> globalTimeScale_{ 1.0f };

    // ── Internal simulation state ──────────────────────────────────────────────
    // Phase 1+: Morphon array, field objects, etc. will live here.
    uint64_t tickIndex_        = 0;
    double   simulationTimeMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhysicsEngine)
};
