#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#include "EffectZone.h"
#include "FieldObject.h"
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
    bool        active  = false;
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

    int              activeMorphonCount       = 0;
    int              activeFieldObjCount      = 0;
    int              activeTimbralAnchorCount = 0;
    int              activeEffectZoneCount    = 0;
    BoundaryBehavior globalBoundary           = BoundaryBehavior::Wrap;
    PolyMode         globalPolyMode           = PolyMode::Polyphonic;
    float            globalGlideTime          = 0.0f;   // Portamento seconds [0, 5]
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
        SetPolyMode,           // x = (float)cast of PolyMode uint8_t; index unused
        SetTimbralAnchorTimbreX,
        SetTimbralAnchorTimbreY,
        SetGlideTime,          // x = portamento time in seconds [0, 5]; index unused

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
    enum class Type : uint8_t { NoteOn, NoteOff };
    Type type     = Type::NoteOff;
    int  channel  = 1;
    int  note     = 60;
    int  velocity = 0;
};
