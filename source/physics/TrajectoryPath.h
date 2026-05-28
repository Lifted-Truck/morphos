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

struct TrajectoryPath
{
    PathShape      shape    = PathShape::Circle;
    float          x        = 0.5f;          // Centre on Manifold [0,1]
    float          y        = 0.5f;
    float          radius   = 0.15f;         // Shape size (circle radius for v1)
    TrajectoryMode mode     = TrajectoryMode::AutoPlay;
    float          speed    = 0.5f;          // For AutoPlay: t per second
    float          currentT = 0.0f;          // Current parameter [0, 1)
    bool           active   = false;
};

// Sample (outX, outY) at parameter t along the path. t is treated modulo 1
// for closed shapes.
inline void trajectoryPathSample(const TrajectoryPath& tp, float t,
                                 float& outX, float& outY) noexcept
{
    // Circle (v1) — closed; t maps directly to angle.
    constexpr float TWO_PI = 6.28318530717958647692f;
    const float angle = t * TWO_PI;
    outX = tp.x + std::cos(angle) * tp.radius;
    outY = tp.y + std::sin(angle) * tp.radius;
}
