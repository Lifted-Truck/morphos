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
