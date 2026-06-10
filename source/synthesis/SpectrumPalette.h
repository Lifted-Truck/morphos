#pragma once
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// SpectrumPalette.h — additive "Morph Surface" spectra
//
// Each additive TimbralAnchor carries a normalised partial-amplitude table
// (a single-cycle spectrum). The physics thread IDW-blends these tables across
// the Manifold per Morphon, and the audive engine synthesises directly from the
// blended table — so a Morphon's *waveform* morphs with geometry, on-concept.
//
// Slice 1: each anchor picks one of a built-in palette. fillSpectrum() runs only
// on the cold path (preset change / patch load), never per audio sample.
// Slice 2 (later): editable tables + derive-from-sample (FFT).
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MORPH_NUM_PARTIALS = 64;

enum class SpectrumType : int
{
    Sine = 0,
    Triangle,
    Saw,
    Square,
    Pulse,      // 25% duty
    Organ,      // drawbar-style harmonic set
    Formant,    // vowel-like resonant peaks
    Bright,     // slow rolloff, lots of highs
    Count
};

inline const char* spectrumTypeName(SpectrumType t) noexcept
{
    switch (t)
    {
        case SpectrumType::Sine:     return "Sine";
        case SpectrumType::Triangle: return "Triangle";
        case SpectrumType::Saw:      return "Saw";
        case SpectrumType::Square:   return "Square";
        case SpectrumType::Pulse:    return "Pulse";
        case SpectrumType::Organ:    return "Organ";
        case SpectrumType::Formant:  return "Formant";
        case SpectrumType::Bright:   return "Bright";
        default:                     return "Saw";
    }
}

// Fill out[0..n) with the named spectrum's per-partial magnitudes (partial k = i+1),
// normalised to unit sum. Cold path only (uses pow/sin/exp).
inline void fillSpectrum(SpectrumType t, float* out, int n) noexcept
{
    constexpr float PI = 3.14159265358979323846f;

    for (int i = 0; i < n; ++i)
    {
        const int   k  = i + 1;
        const float kf = static_cast<float>(k);
        float a = 0.0f;

        switch (t)
        {
            case SpectrumType::Sine:
                a = (k == 1) ? 1.0f : 0.0f;
                break;
            case SpectrumType::Triangle:
                a = (k % 2 == 1) ? 1.0f / (kf * kf) : 0.0f;
                break;
            case SpectrumType::Saw:
                a = 1.0f / kf;
                break;
            case SpectrumType::Square:
                a = (k % 2 == 1) ? 1.0f / kf : 0.0f;
                break;
            case SpectrumType::Pulse:   // 25% duty: amp ∝ |sin(kπd)| / k, d = 0.25
                a = std::fabs(std::sin(kf * PI * 0.25f)) / kf;
                break;
            case SpectrumType::Organ:   // drawbar-ish: emphasise a few low harmonics
            {
                switch (k)
                {
                    case 1:  a = 1.00f; break;
                    case 2:  a = 0.80f; break;
                    case 3:  a = 0.60f; break;
                    case 4:  a = 0.50f; break;
                    case 6:  a = 0.35f; break;
                    case 8:  a = 0.25f; break;
                    default: a = 0.04f / kf; break;  // faint bed
                }
                break;
            }
            case SpectrumType::Formant: // 1/k bed + two Gaussian resonant peaks
            {
                a = 0.35f / kf;
                const float f1 = std::exp(-0.5f * ((kf - 4.0f)  / 1.6f) * ((kf - 4.0f)  / 1.6f));
                const float f2 = std::exp(-0.5f * ((kf - 11.0f) / 2.4f) * ((kf - 11.0f) / 2.4f));
                a += 1.0f * f1 + 0.7f * f2;
                break;
            }
            case SpectrumType::Bright:
                a = 1.0f / std::sqrt(kf);
                break;
            default:
                a = 1.0f / kf;
                break;
        }

        out[i] = a;
    }

    // Normalise to unit sum so every preset (and any blend of them) has comparable
    // loudness; the audio engine re-normalises after the brightness tilt anyway.
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += out[i];
    if (sum > 0.0f)
    {
        const float inv = 1.0f / sum;
        for (int i = 0; i < n; ++i) out[i] *= inv;
    }
}
