#pragma once
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// TimbralAnchor.h — Manifold timbre nodes + IDW blending
//
// A TimbralAnchor is a point on the Manifold that defines a synthesis preset.
// Morphons inherit a blend of the nearest Anchors' parameters, weighted by
// inverse squared distance (IDW). The result is written into MorphonState
// (timbreX / timbreY) on the physics thread each tick.
//
// Phase 2: two hardcoded Additive Anchors.
//   timbreX = spectral rolloff  [0..1]  — 0 = dark (few harmonics), 1 = bright (flat spectrum)
//   timbreY = inharmonicity     [0..1]  — 0 = purely harmonic,      1 = heavily stretched partials
//
// Phase 3+: anchors become user-placed objects with engine type per Anchor.
//           IDW replaced by RBF with configurable kernel width and influence radius.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_TIMBRAL_ANCHORS = 32;

struct TimbralAnchor
{
    float x       = 0.5f;   // Manifold position [0,1]
    float y       = 0.5f;
    float timbreX = 0.5f;   // Phase 2: spectral rolloff [0,1]
    float timbreY = 0.0f;   // Phase 2: inharmonicity    [0,1]
    int   trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    // ── Granular ──────────────────────────────────────────────────────────────
    // sourceId < 0 → additive anchor (timbreX/Y). sourceId >= 0 → granular anchor
    // bound to that audio source. Each granular param is its own spatial RBF
    // field, blended within the source group at a Morphon's position.
    int   sourceId     = -1;
    float readPosition = 0.5f;  // [0,1] read-head into the source buffer (scrub)
    float density      = 0.3f;  // [0,1] grain rate / overlap
    float jitter       = 0.0f;  // [0,1] read-position randomisation (smear)
    float spray        = 0.0f;  // [0,1] grain-timing randomisation (de-metallise)
    float grainSize    = 0.06f; // grain/window length in seconds
    float pitchSemis   = 0.0f;  // grain detune in semitones [-24,+24]
    bool  positionEnabled = true; // false → "texture waypoint": no read-pos contribution
    // Per-anchor output level — a spatial loudness field blended over ALL anchors
    // (additive and granular), applied to the voice. [0, 2], 1 = unity.
    float volume       = 1.0f;
    bool  active  = false;  // Slot in use (physics thread manages compaction)
};

// ─────────────────────────────────────────────────────────────────────────────
// blendAnchors — compute IDW-blended timbral parameters at Manifold position
//               (px, py) across 'count' anchors.
//
// Uses squared inverse distance with epsilon guard to avoid singularity when
// a Morphon sits exactly on an Anchor. Writes results into outTimbreX/outTimbreY.
// ─────────────────────────────────────────────────────────────────────────────

inline void blendAnchors(float px, float py,
                         const TimbralAnchor* anchors, int count,
                         float& outTimbreX, float& outTimbreY) noexcept
{
    constexpr float EPSILON = 1e-6f;

    float totalWeight = 0.0f;
    float sumX        = 0.0f;
    float sumY        = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        const auto& a  = anchors[i];
        const float dx = px - a.x;
        const float dy = py - a.y;
        const float d2 = dx * dx + dy * dy;
        const float w  = 1.0f / (d2 + EPSILON);

        totalWeight += w;
        sumX        += w * a.timbreX;
        sumY        += w * a.timbreY;
    }

    if (totalWeight > 0.0f)
    {
        outTimbreX = sumX / totalWeight;
        outTimbreY = sumY / totalWeight;
    }
    else
    {
        outTimbreX = 0.5f;
        outTimbreY = 0.0f;
    }
}

// Blended granular parameters at a Morphon position — all spatial RBF fields,
// averaged within the dominant source group. Defaults match the anchor struct.
struct GranularBlend
{
    int   sourceId  = -1;
    float readPos   = 0.5f;
    float density   = 0.3f;
    float jitter    = 0.0f;
    float spray     = 0.0f;
    float grainSize = 0.06f;
    float pitch     = 0.0f;
    float weight    = 0.0f;   // dominant group's share of total field weight
};

// ─────────────────────────────────────────────────────────────────────────────
// blendAnchorsGranular — IDW blend that splits anchors into additive vs granular
// groups (by sourceId). Computes:
//   - additive timbreX/Y over additive anchors (sourceId < 0), and the additive
//     group's weight share (outAdditiveWeight);
//   - the dominant granular source at (px,py) — the granular sourceId with the
//     greatest single-anchor weight — with every grain param IDW-blended within
//     that group, plus the group's weight share (g.weight).
//
// Read-position only accumulates from position-enabled anchors, so a
// position-disabled "texture waypoint" flavours density/jitter/spray/size/pitch
// without dragging the read-head. Cross-source crossfade between distinct files
// is a later phase; here a single granular group wins.
// ─────────────────────────────────────────────────────────────────────────────
inline void blendAnchorsGranular(float px, float py,
                                 const TimbralAnchor* anchors, int count,
                                 float& outTimbreX, float& outTimbreY,
                                 float& outAdditiveWeight, float& outVolume,
                                 GranularBlend& g) noexcept
{
    constexpr float EPSILON = 1e-6f;

    float totalW = 0.0f;
    float addW = 0.0f, addX = 0.0f, addY = 0.0f;
    float volSum = 0.0f;
    int   domSource = -1;
    float domW      = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        const auto& a  = anchors[i];
        const float dx = px - a.x, dy = py - a.y;
        const float w  = 1.0f / (dx * dx + dy * dy + EPSILON);
        totalW += w;
        volSum += w * a.volume;
        if (a.sourceId < 0)      { addW += w; addX += w * a.timbreX; addY += w * a.timbreY; }
        else if (w > domW)       { domW = w; domSource = a.sourceId; }
    }

    // Per-anchor volume — a loudness field over all anchors (additive + granular).
    outVolume = (totalW > 0.0f) ? (volSum / totalW) : 1.0f;

    if (addW > 0.0f) { outTimbreX = addX / addW; outTimbreY = addY / addW; }
    else             { outTimbreX = 0.5f;        outTimbreY = 0.0f;        }

    // Additive group's share of total field weight. With no anchors at all,
    // fall back to "fully additive" so a bare patch still makes its default tone.
    outAdditiveWeight = (totalW > 0.0f) ? (addW / totalW) : 1.0f;

    g = GranularBlend{};
    g.sourceId = domSource;

    if (domSource >= 0 && totalW > 0.0f)
    {
        float grW = 0.0f, posW = 0.0f, grPos = 0.0f;
        float grDen = 0.0f, grJit = 0.0f, grSpr = 0.0f, grSize = 0.0f, grPit = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const auto& a = anchors[i];
            if (a.sourceId != domSource) continue;
            const float dx = px - a.x, dy = py - a.y;
            const float w  = 1.0f / (dx * dx + dy * dy + EPSILON);
            grW    += w;
            grDen  += w * a.density;
            grJit  += w * a.jitter;
            grSpr  += w * a.spray;
            grSize += w * a.grainSize;
            grPit  += w * a.pitchSemis;
            if (a.positionEnabled) { posW += w; grPos += w * a.readPosition; }
        }
        if (grW > 0.0f)
        {
            g.density   = grDen  / grW;
            g.jitter    = grJit  / grW;
            g.spray     = grSpr  / grW;
            g.grainSize = grSize / grW;
            g.pitch     = grPit  / grW;
        }
        if (posW > 0.0f) g.readPos = grPos / posW;
        g.weight = grW / totalW;
    }
}
