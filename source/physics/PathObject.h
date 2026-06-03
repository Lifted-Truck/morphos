#pragma once
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// PathObject.h — Rail-constraint Manifold object
//
// A PathObject defines a curve on the Manifold. Morphons that enter the path's
// snap radius are pinned to the curve and slide along it: each tick after
// integration, a pinned Morphon's position snaps to the closest point on the
// curve and its velocity is projected onto the local tangent. Field forces
// still apply through the integrator — only their tangential component
// survives the projection, so the Morphon moves along the rail driven by the
// field's tangential pull.
//
// Pin lifecycle:
//   • Unpinned Morphon enters snap zone → pin (pathIndex = i)
//   • Pinned Morphon stays pinned until either (a) the path is removed or
//     (b) the path's escapeForce > 0 and the perpendicular field force at
//     the contact point exceeds escapeForce — at which point the Morphon
//     unpins and resumes free motion (escape).
//   • Path removed → pinned Morphons unpin and resume free motion
//   • Morphon released and Release envelope completes → m.active = false;
//     next spawn into this slot starts fresh with pathIndex = -1.
//
// v1 ships Circle only (closed loop). The shape enum and per-shape sampling
// function leave room for Line / Arc / Polyline / Bezier without changing
// the storage layout or call sites.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_PATH_OBJECTS = 8;

enum class PathShape : uint8_t
{
    Circle,    // Closed loop; radius drives both geometry and traversal length
    Line,      // Open segment defined by centre + length + angleRad (currently
               // honoured by TrajectoryPath; rails/flow still treat as Circle).
    // Future: Arc, Polyline, Bezier
};

struct PathObject
{
    PathShape shape      = PathShape::Circle;
    float     x          = 0.5f;     // Centre on Manifold [0,1]
    float     y          = 0.5f;
    float     radius     = 0.15f;    // Shape size (circle radius for v1)
    float     snapRadius = 0.04f;    // Pin threshold — Morphons within this distance pin
    float     escapeForce = 0.0f;    // 0 = sticky (no escape); >0 = perpendicular force
                                     // magnitude at which pinned Morphons unpin and fly free
    int       trajectoryPathIndex = -1;  // -1 = stationary; else attached to traj[index]
    bool      active     = false;
};

// Find the closest point on the path to (mx, my) and the unit tangent vector
// at that point. Tangent direction follows CCW traversal (positive arc-length).
inline void pathClosestPoint(const PathObject& p,
                             float mx, float my,
                             float& outX, float& outY,
                             float& outTx, float& outTy) noexcept
{
    // Circle (v1): closest point lies along the radial from centre through (mx,my)
    const float dx = mx - p.x;
    const float dy = my - p.y;
    const float d  = std::sqrt(dx * dx + dy * dy);

    if (d < 1e-6f)
    {
        // Morphon coincides with centre — radial direction is undefined.
        // Pick an arbitrary contact point (right of centre) and a tangent
        // perpendicular to it (up). Any consistent choice works; the Morphon
        // will start at this contact point and integrate from there.
        outX  = p.x + p.radius;
        outY  = p.y;
        outTx = 0.0f;
        outTy = 1.0f;
        return;
    }

    const float invD = 1.0f / d;
    outX  = p.x + dx * (p.radius * invD);
    outY  = p.y + dy * (p.radius * invD);
    // Tangent = radial rotated +90° (CCW): (-dy, dx) / |r|
    outTx = -dy * invD;
    outTy =  dx * invD;
}
