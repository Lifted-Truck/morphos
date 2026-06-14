#pragma once
#include <cmath>

#include "SpectrumPalette.h"   // MORPH_NUM_PARTIALS (log2k table size)

// ─────────────────────────────────────────────────────────────────────────────
// SineTable.h — shared lookup tables for the additive hot loop
//
// Morph Surface raised the additive engine to 64 partials, making std::sin the
// dominant processBlock cost (64 calls per voice per sample). This table
// replaces it: 4096 entries + linear interpolation ≈ −130 dB worst-case error,
// far below the synth's noise floor, at ~2 loads + 1 multiply per partial.
//
// Also carries log2(k) per partial so the brightness tilt k^tiltExp can be
// computed as exp2(tiltExp · log2 k) — identical math, much cheaper than pow.
//
// Initialised once at static-init time (before any audio callback can run);
// the audio thread only ever reads it. ~16 KB, comfortably cache-resident.
// ─────────────────────────────────────────────────────────────────────────────

struct SineTable
{
    static constexpr int SIZE = 4096;

    // [SIZE] duplicates [0] so the interpolated read never needs a wrap branch.
    float table[SIZE + 1];

    // log2(k) for partial k = i + 1 (used by the per-buffer brightness tilt).
    float log2k[MORPH_NUM_PARTIALS];

    SineTable() noexcept
    {
        constexpr double twoPi = 6.283185307179586476925286766559;
        for (int i = 0; i < SIZE; ++i)
            table[i] = static_cast<float>(std::sin(twoPi * i / SIZE));
        table[SIZE] = table[0];

        for (int i = 0; i < MORPH_NUM_PARTIALS; ++i)
            log2k[i] = std::log2(static_cast<float>(i + 1));
    }

    // sin(2π·phase). Precondition: phase ∈ [0, 1) — the caller's phasor wrap
    // guarantees this as long as the per-sample increment is < 1 (always true
    // for partials below Nyquist; above-Nyquist partials are skipped upstream).
    float operator()(float phase) const noexcept
    {
        const float x    = phase * static_cast<float>(SIZE);
        const int   i    = static_cast<int>(x);
        const float frac = x - static_cast<float>(i);
        return table[i] + (table[i + 1] - table[i]) * frac;
    }
};

inline const SineTable kSineTable;
