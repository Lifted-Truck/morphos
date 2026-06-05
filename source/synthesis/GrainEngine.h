#pragma once
#include <array>
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// GrainVoice — per-Morphon granular voice: a small pool of overlapping windowed
// grains plus a spawn scheduler. Reads a mono source buffer at a scrub position
// (manifold geometry) while pitch is decoupled (playback rate from MIDI). This
// is the grain-cloud core for granular slice 1: grains spawn at the current
// scrub position, no jitter/spray yet.
//
// Realtime: no allocation; fixed grain pool. process() is called per output
// sample inside the audio loop. State persists across buffers (continuous
// granular); reset() on voice (re)spawn.
// ─────────────────────────────────────────────────────────────────────────────
struct GrainVoice
{
    static constexpr int   MAX_GRAINS = 8;
    static constexpr float TWO_PI     = 6.28318530717958648f;

    struct Grain
    {
        bool   active   = false;
        double pos      = 0.0;   // read position in source samples
        double rate     = 1.0;   // source samples advanced per output sample
        float  winPhase = 0.0f;  // 0..1 across grain lifetime
        float  winInc   = 0.0f;  // per-sample increment of winPhase
    };

    std::array<Grain, MAX_GRAINS> grains{};
    float    spawnTimer = 0.0f;     // output-samples until the next grain spawns
    bool     wasActive  = false;
    uint32_t rng = 0x9E3779B9u;     // xorshift state (kept evolving across notes)

    // Fast xorshift32 → [0,1). Not reset on respawn so voices decorrelate.
    float nextRand() noexcept
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return static_cast<float>(rng & 0xFFFFFFu) * (1.0f / 16777216.0f);
    }

    void reset() noexcept
    {
        for (auto& g : grains) g.active = false;
        spawnTimer = 0.0f;
        wasActive  = false;
    }

    // Render one output sample.
    //   src / n            : mono source buffer and its length
    //   readPos01          : scrub position [0,1] (grains spawn here)
    //   rate               : source samples per output sample (pitch × SR ratio)
    //   grainLenSamp       : grain length in output samples (Hann window span)
    //   spawnIntervalSamp  : output samples between grain spawns (1/density)
    //   jitterSamp         : ± read-start randomisation, in source samples
    //   sprayAmt           : [0,1] grain-timing randomisation
    float process(const float* src, int n,
                  float readPos01, double rate,
                  float grainLenSamp, float spawnIntervalSamp,
                  float jitterSamp, float sprayAmt) noexcept
    {
        if (src == nullptr || n <= 1) return 0.0f;

        // ── Spawn scheduling ─────────────────────────────────────────────────
        spawnTimer -= 1.0f;
        if (spawnTimer <= 0.0f)
        {
            float interval = (spawnIntervalSamp > 1.0f) ? spawnIntervalSamp : 1.0f;
            if (sprayAmt > 0.0f)
            {
                interval *= 1.0f + sprayAmt * (nextRand() - 0.5f);
                if (interval < 1.0f) interval = 1.0f;
            }
            spawnTimer += interval;

            for (auto& g : grains)
            {
                if (!g.active)
                {
                    const float p = readPos01 < 0.0f ? 0.0f : (readPos01 > 1.0f ? 1.0f : readPos01);
                    double pos = (double) p * (double) (n - 1);
                    if (jitterSamp > 0.0f)
                        pos += (double) ((nextRand() * 2.0f - 1.0f) * jitterSamp);
                    if (pos < 0.0)              pos = 0.0;
                    if (pos > (double)(n - 1))  pos = (double)(n - 1);

                    g.active   = true;
                    g.pos      = pos;
                    g.rate     = rate;
                    g.winPhase = 0.0f;
                    g.winInc   = (grainLenSamp > 1.0f) ? (1.0f / grainLenSamp) : 1.0f;
                    break;
                }
            }
        }

        // ── Sum active grains ────────────────────────────────────────────────
        float out = 0.0f;
        for (auto& g : grains)
        {
            if (!g.active) continue;

            const float win = 0.5f - 0.5f * std::cos(TWO_PI * g.winPhase);

            const int   i0   = (int) g.pos;
            const float frac = (float) (g.pos - (double) i0);
            float s = 0.0f;
            if (i0 >= 0 && i0 < n - 1) s = src[i0] + (src[i0 + 1] - src[i0]) * frac;
            else if (i0 >= 0 && i0 < n) s = src[i0];
            out += s * win;

            g.pos      += g.rate;
            g.winPhase += g.winInc;
            if (g.winPhase >= 1.0f || g.pos >= (double) n || g.pos < 0.0)
                g.active = false;
        }
        return out;
    }
};
