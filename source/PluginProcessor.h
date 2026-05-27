#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "physics/PhysicsEngine.h"
#include "Parameters.h"

// ─────────────────────────────────────────────────────────────────────────────
// MorphosProcessor — audio thread entry point (VST3 AudioProcessor)
//
// Responsibilities:
//   - Owns the PhysicsEngine (starts/stops with the plugin)
//   - Owns the AudioProcessorValueTreeState (all automatable parameters)
//   - Forwards MIDI note events to the PhysicsEngine via lock-free queue
//   - Reads the latest PhysicsStateSnapshot and generates audio  →  Phase 2+
//   - Serialises/deserialises full plugin state (parameters + Manifold objects)
//
// Real-time safety: processBlock() must never allocate, lock a mutex, or
// call any OS function that can block. All PhysicsEngine communication is
// lock-free. All parameter reads use cached std::atomic<float>* pointers.
// ─────────────────────────────────────────────────────────────────────────────

class MorphosProcessor : public juce::AudioProcessor
{
public:
    MorphosProcessor();
    ~MorphosProcessor() override;

    // ── AudioProcessor interface ──────────────────────────────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi()  const override { return true;  }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()                               override { return 1; }
    int  getCurrentProgram()                            override { return 0; }
    void setCurrentProgram(int)                         override {}
    const juce::String getProgramName(int)              override { return {}; }
    void changeProgramName(int, const juce::String&)    override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ── Public accessors (called from editor / UI thread) ─────────────────────

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts_; }

    // UI thread reads the latest physics snapshot for visualisation.
    // Safe to call from the UI thread; returns a recent (possibly 1-tick-old)
    // snapshot without blocking the audio thread.
    const PhysicsStateSnapshot& getPhysicsStateForUI() const noexcept
    {
        // The editor polls this at ~30 Hz via a timer.
        // We deliberately do NOT use the triple-buffer here — the UI does not
        // need the absolute latest frame, just a recent one. In Phase 1+ this
        // will route through a separate UI-side ring buffer.
        return latestSnapshotForUI_;
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ── Subsystems ────────────────────────────────────────────────────────────
    juce::AudioProcessorValueTreeState apvts_;
    PhysicsEngine                      physicsEngine_;

    // Cached parameter pointers — accessed on the audio thread without locking.
    std::atomic<float>* pGain_           = nullptr;
    std::atomic<float>* pGlobalTimeScale_= nullptr;

    // Snapshot copied from physics for UI thread consumption (approximate/stale OK).
    // Updated in processBlock() by a relaxed copy — not lock-free-perfect, but
    // safe for read-only display use. Replace with a proper UI bridge in Phase 3+.
    PhysicsStateSnapshot latestSnapshotForUI_;

    // Current sample rate — set in prepareToPlay, read in processBlock
    double sampleRate_ = 44100.0;

    // ── Phase 2: Additive synthesis voice state ───────────────────────────────
    // Phasor accumulators for each Morphon slot's additive engine.
    // Indexed by Morphon slot (not MIDI note) so state is preserved across retrigs.
    // Reset on each new activation (wasActive false → true transition).
    static constexpr int NUM_PARTIALS = 20;

    struct AdditiveVoice
    {
        std::array<float, NUM_PARTIALS> phases{};
        bool wasActive = false;
    };

    std::array<AdditiveVoice, MAX_MORPHONS> voices_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphosProcessor)
};
