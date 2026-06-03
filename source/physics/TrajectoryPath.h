#pragma once
#include <cmath>
#include <cstdint>

#include "PathObject.h"   // for PathShape

// ─────────────────────────────────────────────────────────────────────────────
// TrajectoryPath.h — Position-driver Manifold object
//
// A TrajectoryPath defines a curve along which an attached object's position
// is computed each tick. The curve itself does not interact with Morphons —
// it drives the (x, y) of whatever object has been attached to it (v1: only
// Emitters can attach; future versions extend to Anchors / FieldObjects /
// Effect Zones / Flux Gates).
//
// Modes:
//   • AutoPlay — `currentT` advances by `speed * dt` each tick. Closed shapes
//                wrap modulo 1.0; open shapes ping-pong between 0 and 1.
//   • Manual   — `currentT` is set externally (Phase 6: mod-matrix destination).
//                In Manual mode `speed` is ignored.
//
// `speed` units: parameter t per second. For a closed circle, this means the
// orbital period is 1.0 / speed seconds (speed = 0.5 → 2-second loop).
//
// v1 ships PathShape::Circle (closed) only. PathShape lives in PathObject.h
// and is shared with rail-constraint paths so adding new shape types is one
// closest-point + sample-position pair per shape, used by both object types.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_TRAJECTORY_PATHS = 8;

enum class TrajectoryMode : uint8_t
{
    AutoPlay,   // currentT = (currentT + speed * dt) mod 1.0
    Manual,     // currentT driven externally (Phase 6 mod matrix destination)
};

// Velocity-vs-t profile for PathShape::Line — controls how the position
// oscillates between the two endpoints as t advances 0 → 1.
//   Triangular: position is a linear ping-pong (constant velocity, hard
//               turnaround at endpoints).
//   Sinusoidal: position follows 0.5 + 0.5 sin(2πt − π/2) — smooth
//               back-and-forth, slower at endpoints.
// Both produce a full out-and-back cycle per t = 0 → 1.
enum class TrajectoryLineCurve : uint8_t
{
    Triangular,
    Sinusoidal,
};

struct TrajectoryPath
{
    PathShape           shape    = PathShape::Circle;
    float               x        = 0.5f;          // Centre on Manifold [0,1]
    float               y        = 0.5f;
    float               radius   = 0.15f;         // Circle: ring radius
    float               length   = 0.30f;         // Line: full segment length
    float               angleRad = 0.0f;          // Line: orientation (0 = horizontal)
    TrajectoryLineCurve curve    = TrajectoryLineCurve::Triangular;  // Line only
    TrajectoryMode      mode     = TrajectoryMode::AutoPlay;
    float               speed    = 0.5f;          // For AutoPlay: t per second
    float               currentT = 0.0f;          // Current parameter [0, 1)
    bool                active   = false;
};

// Sample (outX, outY) at parameter t along the path. t is wrapped to [0, 1).
inline void trajectoryPathSample(const TrajectoryPath& tp, float t,
                                 float& outX, float& outY) noexcept
{
    if (tp.shape == PathShape::Line)
    {
        // Map t to a [0, 1] position along the line via the chosen velocity
        // profile, then lerp between the two endpoints. Both profiles complete
        // one full out-and-back per t-cycle.
        float s;
        if (tp.curve == TrajectoryLineCurve::Sinusoidal)
        {
            constexpr float TWO_PI = 6.28318530717958647692f;
            // sin(2πt − π/2) sweeps −1 → +1 → −1 across t ∈ [0,1].
            s = 0.5f + 0.5f * std::sin(t * TWO_PI - 1.57079632679489661923f);
        }
        else   // Triangular
        {
            s = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
        }
        const float half = tp.length * 0.5f;
        const float ax = tp.x - std::cos(tp.angleRad) * half;
        const float ay = tp.y - std::sin(tp.angleRad) * half;
        const float bx = tp.x + std::cos(tp.angleRad) * half;
        const float by = tp.y + std::sin(tp.angleRad) * half;
        outX = ax + (bx - ax) * s;
        outY = ay + (by - ay) * s;
        return;
    }

    // Circle — closed; t maps directly to angle.
    constexpr float TWO_PI = 6.28318530717958647692f;
    const float angle = t * TWO_PI;
    outX = tp.x + std::cos(angle) * tp.radius;
    outY = tp.y + std::sin(angle) * tp.radius;
}
