#pragma once
#include <cmath>
#include <cstdint>

#include "PathObject.h"   // for PathShape + pathClosestPoint (shared geometry)

// ─────────────────────────────────────────────────────────────────────────────
// TangentPath.h — Tangent-force ("Flow") Manifold object
//
// A TangentPath defines a curve that exerts a *force* on nearby Morphons rather
// than rigidly pinning them (the rail-constraint PathObject does the latter).
// Within an influence band of half-width `width` around the curve, the force
// has two components:
//
//   • Tangential push — drives the Morphon along the curve. Direction follows
//     CCW traversal scaled by `chirality` (+1 = CCW, −1 = CW), magnitude scaled
//     by `strength`.
//   • Centering pull — a gentle spring toward the nearest point on the curve so
//     Morphons get drawn into the stream and carried along instead of being
//     flung off. A fixed fraction of `strength`.
//
// Both components fall off linearly to zero at `width`. The force is added into
// the field-force accumulator before the F = ma step, so Morphon mass scales the
// response exactly like Attractor / Repeller / Vortex forces. Because it's a
// soft force (not a constraint), a strong field elsewhere can still pull a
// Morphon out of the stream — the defining difference from a rail.
//
// v1 ships PathShape::Circle only. PathShape and pathClosestPoint live in
// PathObject.h and are shared with rail-constraint paths, so adding a new shape
// is a single closest-point implementation reused by both object types.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_TANGENT_PATHS = 8;

struct TangentPath
{
    PathShape shape     = PathShape::Circle;
    float     x         = 0.5f;     // Centre on Manifold [0,1]
    float     y         = 0.5f;
    float     radius    = 0.15f;    // Curve geometry (circle radius for v1)
    float     width     = 0.08f;    // Influence band half-width around the curve
    float     strength  = 0.40f;    // Tangential force magnitude (Manifold units/s²)
    float     chirality = 1.0f;     // +1 = CCW flow, −1 = CW flow
    int       trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    bool      active    = false;
};

// Accumulate the flow force at (mx, my) into (fx, fy). No-op when the Morphon is
// outside the influence band. Safe to call for every active TangentPath.
inline void tangentPathForce(const TangentPath& tp,
                             float mx, float my,
                             float& fx, float& fy) noexcept
{
    // Bounding pre-filter: a Morphon can only be within the band if it is within
    // (radius + width) of the centre. Reject in squared distance to skip both the
    // pathClosestPoint sqrt and the band-distance sqrt below for the far common
    // case. Exact for the Circle geometry v1 ships; widen for open shapes.
    {
        const float cdx   = mx - tp.x;
        const float cdy   = my - tp.y;
        const float bound = tp.radius + tp.width;
        if (cdx * cdx + cdy * cdy > bound * bound) return;
    }

    // Reuse the rail closest-point helper via a temporary PathObject view —
    // both object types share the same circle geometry + CCW tangent.
    PathObject geo;
    geo.shape  = tp.shape;
    geo.x      = tp.x;
    geo.y      = tp.y;
    geo.radius = tp.radius;

    float px, py, tx, ty;
    pathClosestPoint(geo, mx, my, px, py, tx, ty);

    const float dx = mx - px;
    const float dy = my - py;
    const float d  = std::sqrt(dx * dx + dy * dy);
    if (d >= tp.width) return;              // Outside the influence band

    const float w = 1.0f - d / tp.width;    // Linear falloff [0,1]

    // Tangential push along the curve (tx,ty is the CCW unit tangent).
    fx += tp.strength * w * tp.chirality * tx;
    fy += tp.strength * w * tp.chirality * ty;

    // Centering pull toward the nearest point on the curve.
    if (d > 1e-6f)
    {
        constexpr float CENTERING = 0.6f;
        const float nx = -dx / d;           // Unit vector toward the curve
        const float ny = -dy / d;
        fx += tp.strength * w * CENTERING * nx;
        fy += tp.strength * w * CENTERING * ny;
    }
}
