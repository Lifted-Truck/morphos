#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// FluxGate.h — Crossing-triggered Manifold object
//
// A FluxGate is a line segment on the Manifold. When a Morphon's per-tick
// trajectory (its segment from (prevX, prevY) to (x, y)) crosses the gate
// segment, an event fires. Phase 5 fires a single hard-coded event:
//   • Envelope re-trigger (snap envStage to Attack on held voices).
//
// Phase 7 will broaden the action set as Transient Objects come online
// (percussive event firing on crossing). Phase 6 may add MIDI/CV-style
// crossing outputs once the mod matrix exists.
//
// Geometry: stored as center + length + angle rather than two endpoints. This
// makes click-to-place trivial (one position) and lets the panel expose Length
// and Angle as independent edits. Endpoints are derived per use.
//
// Crossing semantics for envelope re-trigger:
//   • Voice must be held (noteReleased == false). Released voices ignore
//     crossings, otherwise the immediate re-application of Release inside
//     updateEnvelopes would undo the snap-to-Attack and the gate would have
//     no audible effect.
//   • On crossing: m.envStage = Attack; clear terminusArmed/terminusReached.
//   • amplitude is NOT zeroed — the Attack ramp continues from the current
//     amplitude up to 1.0, which gives a snappy swell rather than a click.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_FLUX_GATES = 16;

struct FluxGate
{
    float x        = 0.5f;     // Centre on Manifold, [0,1]
    float y        = 0.5f;
    float length   = 0.20f;    // Endpoint distance along the gate line
    float angleRad = 0.0f;     // 0 = horizontal; +π/2 = vertical
    int   trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    bool  active   = false;
};

// Derive endpoint A (one end of the segment).
inline void fluxGateEndpointA(const FluxGate& g, float& ax, float& ay) noexcept
{
    const float half = g.length * 0.5f;
    ax = g.x - std::cos(g.angleRad) * half;
    ay = g.y - std::sin(g.angleRad) * half;
}

// Derive endpoint B (the other end).
inline void fluxGateEndpointB(const FluxGate& g, float& bx, float& by) noexcept
{
    const float half = g.length * 0.5f;
    bx = g.x + std::cos(g.angleRad) * half;
    by = g.y + std::sin(g.angleRad) * half;
}

// Segment-segment intersection test using orientation triples.
// Returns true if segment AB crosses segment CD (strictly — collinear overlap
// is not treated as a crossing, which is the right semantics for "did the
// Morphon traverse the gate this tick").
inline bool segmentsCross(float ax, float ay, float bx, float by,
                          float cx, float cy, float dx, float dy) noexcept
{
    auto orient = [](float px, float py, float qx, float qy, float rx, float ry) -> int
    {
        const float v = (qx - px) * (ry - py) - (qy - py) * (rx - px);
        if (v >  0.0f) return  1;
        if (v <  0.0f) return -1;
        return 0;
    };

    const int o1 = orient(ax, ay, bx, by, cx, cy);
    const int o2 = orient(ax, ay, bx, by, dx, dy);
    const int o3 = orient(cx, cy, dx, dy, ax, ay);
    const int o4 = orient(cx, cy, dx, dy, bx, by);

    // Generic proper intersection: endpoints of each segment lie on opposite
    // sides of the other segment. Collinear cases return 0 and are excluded.
    return (o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0)
        && (o1 != o2) && (o3 != o4);
}
