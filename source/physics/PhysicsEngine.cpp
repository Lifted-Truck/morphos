#include "PhysicsEngine.h"

PhysicsEngine::PhysicsEngine()
    : juce::Thread("MorphosPhysics")
{
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
    notify();           // Wake the thread if it's sleeping
    stopThread(2000);   // Wait up to 2s for clean exit
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread interface
// ─────────────────────────────────────────────────────────────────────────────

bool PhysicsEngine::pushNoteEvent(const NoteEvent& event) noexcept
{
    int start1, size1, start2, size2;
    eventFifo_.prepareToWrite(1, start1, size1, start2, size2);

    if (size1 == 0)
        return false; // Queue full — event dropped

    eventBuffer_[start1] = event;
    eventFifo_.finishedWrite(1);
    return true;
}

const PhysicsStateSnapshot& PhysicsEngine::getLatestState() noexcept
{
    return bridge_.getLatestForAudio();
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter writes (any thread)
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

    constexpr auto TICK_DURATION = duration_cast<nanoseconds>(
        duration<double>(TICK_INTERVAL_SEC));

    auto nextTick = steady_clock::now();

    while (!threadShouldExit())
    {
        const double timeScale = static_cast<double>(
            globalTimeScale_.load(std::memory_order_relaxed));

        const double effectiveDt = TICK_INTERVAL_SEC * timeScale;

        tick(effectiveDt);

        // Sleep until the next scheduled tick.
        nextTick += TICK_DURATION;
        const auto now = steady_clock::now();
        if (nextTick > now)
            std::this_thread::sleep_until(nextTick);
        else
            nextTick = now; // Catch-up: don't pile up overdue ticks
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — called TICK_RATE_HZ times per second from the physics thread
// ─────────────────────────────────────────────────────────────────────────────

void PhysicsEngine::tick(double dtSeconds)
{
    // 1. Drain note events from the audio thread
    drainNoteEvents();

    // 2. Integrate Morphon positions  →  Phase 1
    // 3. Evaluate field forces        →  Phase 1
    // 4. RBF Anchor blending          →  Phase 2

    // 5. Write snapshot and publish
    auto& snapshot = bridge_.getWriteBuffer();
    snapshot.activeMorphonCount = 0;
    snapshot.tickIndex          = tickIndex_;
    snapshot.simulationTimeMs   = simulationTimeMs_;

    bridge_.publish();

    ++tickIndex_;
    simulationTimeMs_ += dtSeconds * 1000.0;
}

void PhysicsEngine::drainNoteEvents()
{
    int start1, size1, start2, size2;
    eventFifo_.prepareToRead(eventFifo_.getNumReady(), start1, size1, start2, size2);

    // Process batch 1
    for (int i = start1; i < start1 + size1; ++i)
    {
        const auto& e = eventBuffer_[i];
        // Phase 1+: spawn or kill Morphons here
        (void)e;
    }
    // Process batch 2 (ring buffer wrap-around)
    for (int i = start2; i < start2 + size2; ++i)
    {
        const auto& e = eventBuffer_[i];
        (void)e;
    }

    eventFifo_.finishedRead(size1 + size2);
}
