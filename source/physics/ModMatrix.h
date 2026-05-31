#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ModMatrix.h — Modulation routing (Phase 6, v1)
//
// A connection routes a SOURCE (something whose value changes over time, e.g.
// a TrajectoryPath's current t parameter) to a DESTINATION (a continuous param
// on a Manifold object, e.g. a FieldObject's strength). Each connection has a
// `depth` controlling how strongly the source modulates the destination, and
// a `base` value which is what the destination would be in the source's
// neutral (0.5) state.
//
// Per-tick semantics, evaluated in the physics tick before integration:
//     value = base + (sourceValue − 0.5) × 2 × depth
// where sourceValue is normalised to [0, 1]. So depth = 0 leaves the dest at
// its base; depth = +1 sweeps the dest by ±1.0 of its natural unit; depth = −1
// inverts that sweep. Multiple connections can target the same dest; the
// offsets sum, the last connection's `base` wins (in v1 the UI prevents
// duplicate routings — see PluginEditor add-row logic — so this corner case
// shouldn't arise yet).
//
// V1 source / dest set is small but the data model is meant to scale —
// adding a new source means a new enum value plus a read in
// PhysicsEngine::evaluateModMatrix; adding a new destination is a new enum
// value plus a write. See project_morphos.md for the deferred source/dest
// list earmarked for v2.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_MOD_CONNECTIONS = 16;

enum class ModSourceType : uint8_t
{
    None = 0,
    TrajectoryT,     // trajectoryPaths_[index].currentT     [0,1]
    TrajectoryX,     // sampled x at trajectory's currentT   [0,1]
    TrajectoryY,     // sampled y at trajectory's currentT   [0,1]
    // MIDI sources — global (not per-voice) for v1. The last received value
    // across all channels is what the source returns.
    MidiCC,          // midiCC_[index] / 127.0f              [0,1]; index = CC #
    Keytrack,        // last note number / 127.0f            [0,1]
    Velocity,        // last note-on velocity / 127.0f       [0,1]
};

// Destinations. Object-position writes (X/Y) cover every canvas type so the
// user can drive any object's location from any source. NOTE: an object that
// is *also* attached to a trajectory via trajectoryPathIndex has its position
// rewritten by the attachment before the mod matrix runs, so the mod write
// wins — meaning explicit mod-on-X/Y supersedes trajectory attachment for
// the same object. Pick one or the other.
enum class ModDestType : uint8_t
{
    None = 0,
    // Object-position destinations
    FieldObjectX,            // fieldObjects_[index].x         (invalidates grid)
    FieldObjectY,            // fieldObjects_[index].y         (invalidates grid)
    EmitterX,                // emitters_[index].x
    EmitterY,                // emitters_[index].y
    AnchorX,                 // timbralAnchors_[index].x
    AnchorY,                 // timbralAnchors_[index].y
    EffectZoneX,             // effectZones_[index].x
    EffectZoneY,             // effectZones_[index].y
    FluxGateX,               // fluxGates_[index].x
    FluxGateY,               // fluxGates_[index].y
    PathObjectX,             // pathObjects_[index].x
    PathObjectY,             // pathObjects_[index].y
    TrajectoryPathX,         // trajectoryPaths_[index].x   (curve centre)
    TrajectoryPathY,         // trajectoryPaths_[index].y   (curve centre)
    TangentPathX,            // tangentPaths_[index].x
    TangentPathY,            // tangentPaths_[index].y
    // Non-position destinations
    FieldObjectStrength,     // fieldObjects_[index].strength
    FieldObjectRadius,       // fieldObjects_[index].radius     (invalidates grid)
    EffectZoneDepth,         // effectZones_[index].depth
    EffectZoneRadius,        // effectZones_[index].radius
    TrajectoryCurrentT,      // trajectoryPaths_[index].currentT (Manual mode)
    AnchorTimbreX,           // timbralAnchors_[index].timbreX
    AnchorTimbreY,           // timbralAnchors_[index].timbreY
    GlobalFriction,          // globalFriction_   (index ignored)
};

struct ModConnection
{
    ModSourceType srcType  = ModSourceType::None;
    int           srcIndex = 0;
    ModDestType   dstType  = ModDestType::None;
    int           dstIndex = 0;
    float         depth    = 0.0f;   // [-1, +1]
    float         base     = 0.0f;   // Captured at creation; user-edits update this
    bool          active   = false;
};
