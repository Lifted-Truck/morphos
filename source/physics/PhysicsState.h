#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#include "EffectZone.h"
#include "FieldObject.h"
#include "FluxGate.h"
#include "ModMatrix.h"
#include "PathObject.h"
#include "TangentPath.h"
#include "TrajectoryPath.h"
#include "synthesis/TimbralAnchor.h"   // for MAX_TIMBRAL_ANCHORS

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsState.h — Shared data structures for physics ↔ audio/UI communication
//
// Threading model (unchanged from Phase 0):
//   Physics thread  → writes PhysicsStateSnapshot, publishes via triple buffer
//   Audio thread    → reads latest snapshot at start of each processBlock()
//   UI thread       → reads a relaxed copy for display (~30 Hz)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_MORPHONS  = 256;
static constexpr int MAX_EMITTERS  = 8;

// Boundary behaviour when a Morphon exits the [0,1]×[0,1] Manifold.
//
// KleinBottle: Y axis wraps with X-flip (non-orientable surface).
//   Crossing y=1 from below → re-enters at y=0 with x mirrored to (1-x).
//   X axis wraps normally. A Morphon orbiting this topology will appear on
//   the "other side" of the canvas with its trajectory mirrored.
enum class BoundaryBehavior : uint8_t { Wrap, Reflect, Terminate, KleinBottle };

// Amplitude envelope state machine stages (ADSR)
enum class EnvelopeStage : uint8_t { Attack, Decay, Sustain, Release };

// Polyphony mode — governs how note-on events interact with existing Morphons.
// Polyphonic: each note spawns its own Morphon(s), unlimited voices.
// Mono:       all existing Morphons are released before a new one spawns;
//             at most one active Morphon at a time.
// Legato:     gap-sensitive. Retargets an existing held Morphon (keeps position,
//             updates pitch + envelope). If the previous note was released before
//             the new note arrives, falls through to a fresh spawn.
// LegatoSlur: always-retarget. Like Legato but retargets even across note gaps
//             (previous voice in Release is picked up rather than discarded).
enum class PolyMode : uint8_t { Polyphonic, Mono, Legato, LegatoSlur };

// ─────────────────────────────────────────────────────────────────────────────
// MorphonState — per-Morphon data shared between physics, audio, and UI
// ─────────────────────────────────────────────────────────────────────────────
struct MorphonState
{
    // ── Position & kinematics ─────────────────────────────────────────────────
    float x  = 0.5f;    // Manifold position, normalised [0,1]
    float y  = 0.5f;
    float vx = 0.0f;    // Velocity (Manifold units per second)
    float vy = 0.0f;

    // Position at the start of the current tick — used by Flux Gate crossing
    // detection (the Morphon's per-tick trajectory is the segment prev → cur).
    // Stored by integrateMorphons before the Euler step.
    float prevX = 0.5f;
    float prevY = 0.5f;

    // ── Physics parameters ────────────────────────────────────────────────────
    float mass = 1.0f;  // Resistance to field forces
    float drag = 0.02f; // Velocity dissipation per tick (0 = frictionless, 1 = instant stop)

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    float            age          = 0.0f;   // Seconds alive
    bool             active          = false;
    bool             noteReleased    = false;  // true after note-off; triggers Release stage
    bool             terminusArmed   = false;  // true if morphon was outside arrival zone at note-off
    bool             terminusReached = false;  // true once morphon enters arrival zone; speeds up release

    // ── Envelope state ────────────────────────────────────────────────────────
    EnvelopeStage envStage = EnvelopeStage::Attack;

    // ── MIDI identity ─────────────────────────────────────────────────────────
    int midiNote      = 60;
    int midiChannel   = 1;
    int emitterIndex  = -1;   // Which Emitter slot spawned this Morphon

    // ── Amplitude envelope ────────────────────────────────────────────────────
    // Computed by physics tick via ADSR state machine; read by audio engine each buffer.
    float amplitude = 0.0f;   // 0..1, authoritative value for synthesis scaling

    // ── Spatial ───────────────────────────────────────────────────────────────
    float pan     = 0.0f;   // Live stereo position [-1 L, 0 C, +1 R]; reset to basePan each tick
    float basePan = 0.0f;   // Spawn-time pan from Emitter; zones add on top each tick

    // ── Timbral parameters ────────────────────────────────────────────────────
    // Output of Timbral Anchor RBF blending at current Manifold position.
    // Phase 2: timbreX = spectral rolloff, timbreY = inharmonicity.
    // Extended in later phases.
    float timbreX = 0.5f;
    float timbreY = 0.5f;

    // Fundamental frequency derived from midiNote (Hz).
    // When glide time > 0, fundamentalHz interpolates toward targetFundamentalHz
    // each physics tick; otherwise they are always equal.
    float fundamentalHz       = 440.0f;
    float targetFundamentalHz = 440.0f;

    // ── Zone modulation (written each tick by applyEffectZones) ───────────────
    // pitchZoneSemitones is applied multiplicatively in processBlock so it does
    // not corrupt fundamentalHz (which is the glide target).
    float pitchZoneSemitones = 0.0f;

    // Path Object pin index — set by applyPathConstraints when the Morphon
    // enters a PathObject's snap zone. While pinned, the Morphon's position
    // snaps to the path each tick and its velocity is projected onto the
    // local tangent. -1 = not pinned (free motion).
    int pathIndex = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
// EmitterSnapshot — Emitter position and launch params for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct EmitterSnapshot
{
    float          x            = 0.5f;
    float          y            = 0.5f;
    float          launchAngle  = 0.0f;   // radians; used to draw direction arrow
    float          launchSpeed  = 0.0f;   // scales arrow length
    float          spawnMass    = 1.0f;
    float          spawnDrag    = 0.001f;
    float          attackTime   = 0.05f;
    float          decayTime    = 0.15f;
    float          sustainLevel = 0.70f;
    float          releaseTime  = 0.30f;
    int            keyLow        = 0;     // Lowest MIDI note this Emitter responds to
    int            keyHigh       = 127;   // Highest MIDI note this Emitter responds to
    int            transposeOct  = 0;
    int            transposeSemi = 0;
    float          transposeCents        = 0.0f;
    float          pan                   = 0.0f;
    bool           terminusEnabled       = false;
    float          terminusStrength      = 0.30f;
    float          terminusArrivalRadius = 0.04f;
    PolyMode       polyMode              = PolyMode::Polyphonic;
    int            trajectoryPathIndex   = -1;   // -1 = stationary; else attached
    bool           active                = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// TimbralAnchorSnapshot — Anchor position and timbre params for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct TimbralAnchorSnapshot
{
    float x       = 0.5f;
    float y       = 0.5f;
    float timbreX = 0.5f;   // spectral rolloff [0,1]
    float timbreY = 0.0f;   // inharmonicity    [0,1]
    int   trajectoryPathIndex = -1;
    bool  active  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// EffectZoneSnapshot — zone data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct EffectZoneSnapshot
{
    float       x       = 0.5f;
    float       y       = 0.5f;
    float       radius  = 0.15f;
    float       depth   = 0.5f;
    ZoneTarget  target  = ZoneTarget::TimbreX;
    ZoneFalloff falloff = ZoneFalloff::Gaussian;
    int         trajectoryPathIndex = -1;
    bool        active  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// FluxGateSnapshot — crossing-triggered gate data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct FluxGateSnapshot
{
    FluxGateShape shape = FluxGateShape::Line;
    float x        = 0.5f;
    float y        = 0.5f;
    float length   = 0.20f;
    float angleRad = 0.0f;
    float radius   = 0.15f;
    int   trajectoryPathIndex = -1;
    bool  active   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// PathObjectSnapshot — rail-constraint path data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct PathObjectSnapshot
{
    PathShape shape       = PathShape::Circle;
    float     x           = 0.5f;
    float     y           = 0.5f;
    float     radius      = 0.15f;
    float     snapRadius  = 0.04f;
    float     escapeForce = 0.0f;   // 0 = sticky; >0 = perpendicular force escape threshold
    int       trajectoryPathIndex = -1;
    bool      active      = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// TrajectoryPathSnapshot — position-driver path data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct TrajectoryPathSnapshot
{
    PathShape           shape    = PathShape::Circle;
    float               x        = 0.5f;
    float               y        = 0.5f;
    float               radius   = 0.15f;
    float               length   = 0.30f;
    float               angleRad = 0.0f;
    TrajectoryLineCurve curve    = TrajectoryLineCurve::Triangular;
    TrajectoryMode      mode     = TrajectoryMode::AutoPlay;
    float               speed    = 0.5f;
    float               currentT = 0.0f;
    bool                active   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// TangentPathSnapshot — tangent-force ("Flow") path data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct TangentPathSnapshot
{
    PathShape shape     = PathShape::Circle;
    float     x         = 0.5f;
    float     y         = 0.5f;
    float     radius    = 0.15f;
    float     width     = 0.08f;
    float     strength  = 0.40f;
    float     chirality = 1.0f;
    int       trajectoryPathIndex = -1;
    bool      active    = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldObjectSnapshot — lightweight field object data for UI rendering
// ─────────────────────────────────────────────────────────────────────────────
struct FieldObjectSnapshot
{
    FieldObjectType type      = FieldObjectType::Attractor;
    float           x         = 0.5f;
    float           y         = 0.5f;
    float           strength  = 0.0f;
    float           radius    = 0.0f;
    float           chirality = 1.0f;
    int             trajectoryPathIndex = -1;
    bool            active    = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsStateSnapshot — one complete physics tick output
// Triple-buffered between physics and audio threads.
// ─────────────────────────────────────────────────────────────────────────────
struct PhysicsStateSnapshot
{
    std::array<MorphonState,           MAX_MORPHONS>         morphons{};
    std::array<FieldObjectSnapshot,    MAX_FIELD_OBJECTS>    fieldObjects{};
    std::array<EmitterSnapshot,        MAX_EMITTERS>         emitters{};
    std::array<TimbralAnchorSnapshot,  MAX_TIMBRAL_ANCHORS>  timbralAnchors{};
    std::array<EffectZoneSnapshot,     MAX_EFFECT_ZONES>     effectZones{};
    std::array<FluxGateSnapshot,       MAX_FLUX_GATES>       fluxGates{};
    std::array<PathObjectSnapshot,     MAX_PATH_OBJECTS>     pathObjects{};
    std::array<TrajectoryPathSnapshot, MAX_TRAJECTORY_PATHS> trajectoryPaths{};
    std::array<TangentPathSnapshot,    MAX_TANGENT_PATHS>    tangentPaths{};
    // Mod-matrix connections — the same struct is used both physics-side and
    // in the snapshot since it's small and POD. UI reads it for the Mod-tab
    // list; physics evaluates active rows each tick.
    std::array<ModConnection,          MAX_MOD_CONNECTIONS>  modConnections{};

    int              activeMorphonCount       = 0;
    int              activeFieldObjCount      = 0;
    int              activeTimbralAnchorCount = 0;
    int              activeEffectZoneCount    = 0;
    int              activeFluxGateCount      = 0;
    int              activePathObjectCount    = 0;
    int              activeTrajectoryPathCount = 0;
    int              activeTangentPathCount   = 0;
    BoundaryBehavior globalBoundary           = BoundaryBehavior::Wrap;
    float            globalGlideTime          = 0.0f;   // Portamento seconds [0, 5]
    float            globalFriction           = 0.0f;   // Per-tick velocity damping [0, 0.1]
    // Bumped every time an object is added or removed (or a FieldObject's
    // type changes). UI watches this to know when to rebuild the mod-matrix
    // dropdowns without polling every per-slot active flag itself.
    uint64_t         configVersion            = 0;
    uint64_t         tickIndex                = 0;
    double           simulationTimeMs         = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ManifoldEdit — UI thread → physics thread edit command
//
// Sent via a lock-free SPSC queue; applied at the start of the next physics
// tick. Keeps the UI responsive (no waiting for physics) while guaranteeing
// that object state is mutated only on the physics thread.
//
// Scalar edits (non-Move types) carry the new value in `x`; `y` is unused.
// Move edits carry the new Manifold position in (x, y).
// ─────────────────────────────────────────────────────────────────────────────
struct ManifoldEdit
{
    enum class Type : uint8_t
    {
        // ── Position edits — x,y carry new Manifold coords [0,1] ─────────────
        MoveFieldObject,
        MoveEmitter,
        MoveTimbralAnchor,

        // ── Scalar property edits — x carries new value, y unused ────────────
        SetFieldObjectStrength,
        SetFieldObjectRadius,
        SetFieldObjectChirality,
        SetEmitterLaunchAngle,
        SetEmitterLaunchSpeed,
        SetEmitterAttack,
        SetEmitterDecay,
        SetEmitterSustain,
        SetEmitterRelease,
        SetEmitterKeyLow,       // x = (float)MIDI note number [0, 127]
        SetEmitterKeyHigh,      // x = (float)MIDI note number [0, 127]
        SetEmitterTransposeOct, // x = (float)octave offset [-4, +4]
        SetEmitterTransposeSemi,// x = (float)semitone offset [-12, +12]
        SetEmitterTransposeCents,// x = cents [-100, +100]
        SetEmitterPan,              // x = pan position [-1, +1]
        SetEmitterTerminusEnabled,  // x = 1.0f enabled, 0.0f disabled
        SetEmitterTerminusStrength, // x = pull force magnitude
        SetEmitterTerminusRadius,   // x = arrival radius
        SetGlobalBoundary,     // x = (float)cast of BoundaryBehavior uint8_t; index unused
        SetEmitterPolyMode,    // x = (float)cast of PolyMode uint8_t; index = emitter slot
        SetEmitterSpawnMass,   // x = mass [0.1, 4.0]; index = emitter slot
        SetTimbralAnchorTimbreX,
        SetTimbralAnchorTimbreY,
        SetGlideTime,          // x = portamento time in seconds [0, 5]; index unused
        SetGlobalFriction,     // x = decay rate per second (1/s) [0, 10]; index unused

        // ── Spawn / remove — x,y = initial position for Add types ────────────
        AddAttractor,          // Spawn a new Attractor at (x,y) with defaults
        AddRepeller,           // Spawn a new Repeller  at (x,y) with defaults
        AddVortex,             // Spawn a new Vortex    at (x,y) with defaults
        AddEmitter,            // Spawn a new Emitter   at (x,y) with defaults
        AddTimbralAnchor,      // Spawn a new TimbralAnchor at (x,y)
        AddEffectZone,         // Spawn a new EffectZone at (x,y) with defaults
        RemoveFieldObject,     // Deactivate field object[index]
        RemoveEmitter,         // Deactivate emitter[index]
        RemoveTimbralAnchor,   // Deactivate anchor[index] (physics re-compacts array)
        RemoveEffectZone,      // Deactivate effect zone[index]

        // ── Effect zone property edits ────────────────────────────────────────
        MoveEffectZone,           // x,y carry new Manifold coords [0,1]
        SetEffectZoneRadius,      // x = radius [0.01, 0.75]
        SetEffectZoneDepth,       // x = depth (units: [-1,+1] or semitones [-24,+24] for Pitch)
        SetEffectZoneTarget,      // x = (float)cast of ZoneTarget uint8_t
        SetEffectZoneFalloff,     // x = (float)cast of ZoneFalloff uint8_t

        // ── Flux gate spawn / remove / edits ──────────────────────────────────
        AddFluxGate,              // Spawn a new FluxGate at (x,y) with defaults
        RemoveFluxGate,           // Deactivate fluxGate[index]
        MoveFluxGate,             // x,y carry new Manifold coords [0,1] (centre)
        SetFluxGateLength,        // x = length [0.02, 0.9]
        SetFluxGateAngle,         // x = angleRad [-π, +π]
        SetFluxGateShape,         // x = (float)cast of FluxGateShape uint8_t
        SetFluxGateRadius,        // x = circle radius [0.02, 0.45]

        // ── Mod matrix (Phase 6) ──────────────────────────────────────────────
        // index = connection slot [0, MAX_MOD_CONNECTIONS-1]
        AddModConnection,         // Activate the first inactive slot — UI ignores `index`. Source and dest default
                                  //   to None until SetModConnectionSource / SetModConnectionDest follow.
        RemoveModConnection,      // Deactivate connection[index]
        SetModConnectionSource,   // x = (float)ModSourceType,  y = (float)srcIndex
        SetModConnectionDest,     // x = (float)ModDestType,    y = (float)dstIndex
                                  //   — also re-captures `base` from the dest's current value
        SetModConnectionDepth,    // x = depth [-1, +1]
        SetModConnectionBase,     // x = base — issued by sliders when their dest is under modulation,
                                  //   so the user-facing edit updates the connection's pivot value
                                  //   instead of being overwritten by the mod write next tick

        // ── Path object spawn / remove / edits ────────────────────────────────
        AddPathObject,            // Spawn a new PathObject at (x,y) with defaults
        RemovePathObject,         // Deactivate pathObject[index]
        MovePathObject,           // x,y carry new Manifold coords [0,1] (centre)
        SetPathObjectRadius,      // x = radius [0.02, 0.45]
        SetPathObjectSnapRadius,  // x = snap radius [0.005, 0.15]
        SetPathObjectEscapeForce, // x = escape force threshold [0.0, 5.0]; 0 = sticky

        // ── Trajectory path spawn / remove / edits ────────────────────────────
        AddTrajectoryPath,           // Spawn a new TrajectoryPath at (x,y)
        RemoveTrajectoryPath,        // Deactivate trajectoryPath[index]
        MoveTrajectoryPath,          // x,y carry new centre coords [0,1]
        SetTrajectoryPathRadius,     // x = radius [0.02, 0.45]
        SetTrajectoryPathSpeed,      // x = t-per-second [-4.0, 4.0]; negative reverses
        SetTrajectoryPathMode,       // x = (float)cast of TrajectoryMode uint8_t
        SetTrajectoryPathCurrentT,   // x = current t parameter [0, 1) — used in Manual mode
        SetTrajectoryPathShape,      // x = (float)cast of PathShape uint8_t (Circle | Line)
        SetTrajectoryPathLength,     // x = line length [0.02, 0.9]
        SetTrajectoryPathAngle,      // x = line orientation [-π, +π]
        SetTrajectoryPathCurve,      // x = (float)cast of TrajectoryLineCurve uint8_t
        SetEmitterTrajectoryPath,    // x = trajectory index [-1, MAX_TRAJECTORY_PATHS-1]
        // Attach any other object type to a trajectory path's position output.
        // Index parameter selects the object; x = trajectory index (-1 = detach).
        SetFieldObjectTrajectoryPath,
        SetTimbralAnchorTrajectoryPath,
        SetEffectZoneTrajectoryPath,
        SetFluxGateTrajectoryPath,
        SetPathObjectTrajectoryPath,
        SetTangentPathTrajectoryPath,

        // ── Tangent-force ("Flow") path spawn / remove / edits ────────────────
        AddTangentPath,              // Spawn a new TangentPath at (x,y)
        RemoveTangentPath,           // Deactivate tangentPath[index]
        MoveTangentPath,             // x,y carry new centre coords [0,1]
        SetTangentPathRadius,        // x = radius [0.02, 0.45]
        SetTangentPathWidth,         // x = influence band half-width [0.01, 0.30]
        SetTangentPathStrength,      // x = force magnitude [0.0, 2.0]
        SetTangentPathChirality,     // x = flow direction [-1, +1]
    };

    Type  type  = Type::MoveFieldObject;
    int   index = 0;
    float x     = 0.0f;
    float y     = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// PhysicsAudioBridge — wait-free triple buffer (unchanged from Phase 0)
// ─────────────────────────────────────────────────────────────────────────────
class PhysicsAudioBridge
{
public:
    PhysicsAudioBridge()
        : publishedIdx_(0), writeIdx_(1), readIdx_(2)
    {}

    PhysicsStateSnapshot& getWriteBuffer() noexcept
    {
        return buffers_[writeIdx_];
    }

    void publish() noexcept
    {
        writeIdx_ = publishedIdx_.exchange(writeIdx_, std::memory_order_acq_rel);
    }

    const PhysicsStateSnapshot& getLatestForAudio() noexcept
    {
        readIdx_ = publishedIdx_.exchange(readIdx_, std::memory_order_acq_rel);
        return buffers_[readIdx_];
    }

private:
    std::array<PhysicsStateSnapshot, 3> buffers_;
    std::atomic<int> publishedIdx_;
    int writeIdx_;
    int readIdx_;
};

// ─────────────────────────────────────────────────────────────────────────────
// NoteEvent — audio thread → physics thread (unchanged from Phase 0)
// ─────────────────────────────────────────────────────────────────────────────
struct NoteEvent
{
    // ControlChange repurposes the `note` field as the CC number (0-127) and
    // `velocity` as the CC value (0-127). Keeps the audio→physics FIFO single-
    // typed without adding a parallel queue just for mod-matrix MIDI sources.
    enum class Type : uint8_t { NoteOn, NoteOff, ControlChange };
    Type type     = Type::NoteOff;
    int  channel  = 1;
    int  note     = 60;   // For ControlChange: CC number (0-127)
    int  velocity = 0;    // For ControlChange: CC value (0-127)
};
