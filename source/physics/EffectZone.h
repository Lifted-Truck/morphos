#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// EffectZone.h — Spatial Modulator Zones
//
// An EffectZone is a circular region on the Manifold that modulates a Morphon's
// synthesis parameters proportionally to proximity. Unlike Timbral Anchors (which
// define a timbral "preset" blended by RBF), a zone applies an additive delta
// that scales with distance from the zone centre.
//
// The core idea: replace LFOs with geometry. Instead of "modulate X over time at
// rate R", you say "region at (x,y) pushes X toward this value". Timbral change
// is a consequence of the Morphon's trajectory through modulation space.
//
// Parameters:
//   radius  — influence radius [0,1] in Manifold units (same units as FieldObject.radius)
//   depth   — maximum modulation amount at zone centre. Units depend on target:
//               TimbreX / TimbreY / Amplitude / Pan : [-1, +1] additive offset
//               Pitch                                : semitones [-24, +24]
//   target  — which Morphon synthesis parameter is modulated
//   falloff — Linear (1 − d/R) or Gaussian (exp(−d²/2σ²), σ = R/3)
//             Gaussian gives a smooth, centre-weighted feel; Linear gives
//             a harder-edged modulation region.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_EFFECT_ZONES = 16;

enum class ZoneTarget : uint8_t
{
    TimbreX,    // Additive offset to spectral rolloff [0,1]
    TimbreY,    // Additive offset to inharmonicity   [0,1]
    Amplitude,  // Additive offset to envelope amplitude [0,1]
    Pan,        // Additive offset to stereo pan [-1,+1]
    Pitch,      // Pitch shift in semitones (applied multiplicatively to fundamentalHz)
};

enum class ZoneFalloff : uint8_t
{
    Linear,     // w = 1 − dist/radius        (hard-edged, constant slope)
    Gaussian,   // w = exp(−dist²/2σ²), σ=R/3 (smooth, peaked at centre)
};

struct EffectZone
{
    float       x       = 0.5f;
    float       y       = 0.5f;
    float       radius  = 0.15f;
    float       depth   = 0.5f;
    ZoneTarget  target  = ZoneTarget::TimbreX;
    ZoneFalloff falloff = ZoneFalloff::Gaussian;
    int         trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    bool        active  = false;
};
