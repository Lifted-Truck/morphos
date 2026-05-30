#pragma once
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// FluxGate.h — Crossing-triggered Manifold object
//
// A FluxGate fires an event when a Morphon's per-tick trajectory (segment
// prev → cur) crosses its boundary. Phase 5 fires a single hard-coded event:
//   • Envelope re-trigger (snap envStage to Attack on held voices).
//
// Two shapes are supported (closes the path-family trilogy alongside Rail and
// Trajectory paths):
//   • Line   — a straight line segment defined by centre + length + angle.
//   • Circle — a closed ring of given centre + radius; a crossing is detected
//              when the signed distance (|p − centre| − radius) flips sign
//              between prev and cur (i.e. the Morphon traversed the ring
//              boundary, either inward or outward).
//
// Phase 7 will broaden the action set as Transient Objects come online
// (percussive event firing on crossing). Phase 6 may add MIDI/CV-style
// crossing outputs once the mod matrix exists.
//
// Geometry: Line stores centre + length + angle so click-to-place is one
// position and the panel can expose Length/Angle independently. Circle stores
// centre + radius. Endpoints are derived per use.
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

enum class FluxGateShape : uint8_t
{
    Line,    // Centre + length + angle define a segment
    Circle,  // Centre + radius define a ring; crossing = radial sign flip
};

struct FluxGate
{
    FluxGateShape shape    = FluxGateShape::Line;
    float x        = 0.5f;     // Centre on Manifold, [0,1]
    float y        = 0.5f;
    float length   = 0.20f;    // Line: endpoint distance along the gate line
    float angleRad = 0.0f;     // Line: 0 = horizontal; +π/2 = vertical
    float radius   = 0.15f;    // Circle: ring radius
    int   trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    bool  active   = false;
};

// Returns true if the segment (px,py)→(qx,qy) crosses the ring of radius r
// centred at (cx,cy) — i.e. one endpoint is inside the disk and the other
// outside. We don't distinguish inward vs outward crossings here; the gate
// fires on either traversal direction.
inline bool segmentCrossesCircle(float px, float py, float qx, float qy,
                                  float cx, float cy, float r) noexcept
{
    const float dpx = px - cx, dpy = py - cy;
    const float dqx = qx - cx, dqy = qy - cy;
    const float dp2 = dpx * dpx + dpy * dpy;
    const float dq2 = dqx * dqx + dqy * dqy;
    const float r2  = r * r;
    // sign(d² − r²) flips ⇔ Morphon crossed the ring boundary this tick.
    return (dp2 < r2 && dq2 > r2) || (dp2 > r2 && dq2 < r2);
}

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
