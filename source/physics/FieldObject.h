#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// FieldObject.h — Field source types and precomputed vector field
//
// The FieldGrid bakes all active FieldObjects into a 128×128 force array.
// This makes per-Morphon force lookup O(1) bilinear interpolation regardless
// of how many field objects are in the patch — a necessary optimisation before
// per-tick O(morphons × objects) cost becomes audible.
//
// Grid is marked dirty whenever any FieldObject changes. Rebuilt at the start
// of the next physics tick. At patch-edit time (< 1 Hz typically), the ~0.5ms
// rebuild cost is imperceptible.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_FIELD_OBJECTS = 16;

enum class FieldObjectType : uint8_t { Attractor, Repeller, Vortex };

struct FieldObject
{
    FieldObjectType type        = FieldObjectType::Attractor;
    float           x           = 0.5f;   // Manifold position [0,1]
    float           y           = 0.5f;
    float           strength    = 0.3f;   // Force magnitude scale
    float           radius      = 0.4f;   // Influence radius, normalised to [0,1] space
    float           chirality   = 1.0f;   // Vortex: +1 = CCW, -1 = CW
    bool            active      = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldGrid
// ─────────────────────────────────────────────────────────────────────────────
class FieldGrid
{
public:
    static constexpr int RES = 128;

    bool dirty = true;

    // Rebuild from a flat array of FieldObjects. Call when dirty == true.
    // Not real-time safe (uses sqrt in a loop) — call from physics thread only,
    // and only when field objects have changed.
    void rebuild(const FieldObject* objects, int count) noexcept
    {
        const float invRes = 1.0f / (RES - 1);

        for (int iy = 0; iy < RES; ++iy)
        {
            for (int ix = 0; ix < RES; ++ix)
            {
                const float wx = ix * invRes;   // World x in [0,1]
                const float wy = iy * invRes;   // World y in [0,1]

                float sumFx = 0.0f;
                float sumFy = 0.0f;

                for (int k = 0; k < count; ++k)
                {
                    const auto& obj = objects[k];
                    if (!obj.active) continue;

                    const float dx   = wx - obj.x;   // Points from object → grid cell
                    const float dy   = wy - obj.y;
                    const float dist = std::sqrtf(dx * dx + dy * dy);

                    if (dist < 1e-6f) continue;

                    // Linear falloff within radius, zero outside
                    const float t = 1.0f - dist / obj.radius;
                    if (t <= 0.0f) continue;

                    const float falloff = t;   // Phase 5+: expose falloff curve choice
                    const float ndx = dx / dist;
                    const float ndy = dy / dist;

                    switch (obj.type)
                    {
                        case FieldObjectType::Attractor:
                            // Force toward object: negate the outward direction
                            sumFx -= obj.strength * falloff * ndx;
                            sumFy -= obj.strength * falloff * ndy;
                            break;

                        case FieldObjectType::Repeller:
                            // Force away from object
                            sumFx += obj.strength * falloff * ndx;
                            sumFy += obj.strength * falloff * ndy;
                            break;

                        case FieldObjectType::Vortex:
                            // Tangential force: perpendicular to radial direction.
                            // CCW (+1): tangent = (-ndy, ndx)
                            // CW  (-1): tangent = ( ndy, -ndx)
                            sumFx += obj.strength * falloff * (-ndy) * obj.chirality;
                            sumFy += obj.strength * falloff * ( ndx) * obj.chirality;
                            break;
                    }
                }

                fx_[iy][ix] = sumFx;
                fy_[iy][ix] = sumFy;
            }
        }

        dirty = false;
    }

    // Bilinear sample. x, y in [0,1]. Safe to call on any thread once built.
    void sample(float x, float y, float& outFx, float& outFy) const noexcept
    {
        x = std::max(0.0f, std::min(1.0f, x));
        y = std::max(0.0f, std::min(1.0f, y));

        const float gx = x * (RES - 1);
        const float gy = y * (RES - 1);

        const int ix0 = static_cast<int>(gx);
        const int iy0 = static_cast<int>(gy);
        const int ix1 = std::min(ix0 + 1, RES - 1);
        const int iy1 = std::min(iy0 + 1, RES - 1);

        const float tx = gx - ix0;
        const float ty = gy - iy0;

        // Bilinear interpolation
        outFx = (fx_[iy0][ix0] * (1.0f - tx) + fx_[iy0][ix1] * tx) * (1.0f - ty)
              + (fx_[iy1][ix0] * (1.0f - tx) + fx_[iy1][ix1] * tx) * ty;

        outFy = (fy_[iy0][ix0] * (1.0f - tx) + fy_[iy0][ix1] * tx) * (1.0f - ty)
              + (fy_[iy1][ix0] * (1.0f - tx) + fy_[iy1][ix1] * tx) * ty;
    }

private:
    float fx_[RES][RES]{};
    float fy_[RES][RES]{};
};
