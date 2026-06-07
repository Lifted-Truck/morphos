# Morphos — design context

Background and design history distilled from the project's working notes, so it
travels with the repo. `CLAUDE.md` (root + `source/physics/`) holds the lean,
load-bearing conventions; this file holds the fuller "why" — concept, taxonomy,
the mod matrix, the granular design model, and the roadmap. Read it once when
picking up the project; trust the code over this doc where they disagree.

## What Morphos is

A VST3 synthesizer built on **Morphogenetic Synthesis**, a novel paradigm: sound
particles (**Morphons**) spawn from Emitters and travel a 2D vector field (the
**Manifold**) shaped by user-placed objects. Timbre evolves from a particle's
**geometric trajectory**, not from amplitude envelopes — the Manifold *is* the
modulation source.

**The central conceit (and its tripwire):** no envelopes shape timbre over time;
the particle's path through timbral space IS the sound's evolution. Any feature
that re-introduces envelope-as-timbre logic dilutes the concept — flag it rather
than build it silently. (Amplitude envelopes for loudness are fine; it's
*timbre*-from-envelope that's off-limits.)

## Architecture

Three threads, lock-free communication:
- **Physics** (`PhysicsEngine : juce::Thread`, 500 Hz wall-clock) — owns all
  Manifold state, mutated only here. During DAW bounce (`isNonRealtime()`) it
  parks and `processBlock` drives `advance(seconds)` synchronously.
- **Audio** (`processBlock`) — MIDI → physics via an SPSC FIFO of `NoteEvent`
  (note on/off + CC), reads a triple-buffered `PhysicsStateSnapshot`, renders the
  additive + granular voices, pushes APVTS macros into physics each block.
- **UI** (`MorphosEditor`, 30 Hz) — sends `ManifoldEdit` commands to physics via a
  second SPSC FIFO; reads the latest snapshot for rendering.

The command/snapshot pair (`ManifoldEdit` in, `PhysicsStateSnapshot` out) is the
whole UI↔engine contract. The **13-step tick order** lives in
`source/physics/CLAUDE.md` — memorise it; new features slot into a specific step.

## Canvas object taxonomy

Eight types, each a struct in `source/physics/` (+ `synthesis/TimbralAnchor.h`),
mirrored into the snapshot. All carry `int trajectoryPathIndex = -1` so any object
can be driven along a `TrajectoryPath`.
- **FieldObject** — Attractor / Repeller / Vortex; defines the force field.
- **Emitter** — spawns Morphons on note-on; owns envelope, transpose, key range,
  polyMode.
- **TimbralAnchor** — RBF (inverse-square IDW) interpolation source for timbre;
  slot-compacted (active anchors fill `[0, activeAnchorCount_)`). Now also the
  host for granular sources (see below).
- **EffectZone** — proximity-scaled modulation of a target (TimbreX/Y/Amp/Pan/
  Pitch); Linear or Gaussian falloff.
- **FluxGate** — crossing-triggered envelope re-trigger; Line or Circle shape.
- **PathObject ("Rail")** — hard constraint; pinned Morphons follow the curve,
  with per-rail escape force.
- **TrajectoryPath** — position driver (Circle or Line; AutoPlay or Manual).
- **TangentPath ("Flow")** — soft tangential force + centring pull, with chirality.

## Mod matrix (v1)

`source/physics/ModMatrix.h`. 16 connection slots; per tick:
`dst = base + (src − 0.5) × 2 × depth × perDestSwing(dst)`, clamped to each dest's
range. `base` is captured from the dest's current value at hookup.
- **Sources**: TrajectoryT/X/Y, MidiCC (0..127), Keytrack, Velocity, Macro 1..8
  (host-automatable APVTS params).
- **Destinations**: object X/Y, full Emitter param set, FieldObject strength/
  radius/chirality, EffectZone depth/radius, Trajectory params, Anchor timbreX/Y,
  Flow/Rail/Gate params, GlobalFriction.
- Dropdowns refresh off `state.configVersion` (bumped on any object add/remove or
  patch load). Sliders whose dest is under modulation reroute their edits to
  `SetModConnectionBase` so the user edit moves the pivot, not the live value.

## Granular synthesis (active feature)

Full design notes were developed with Julian; the essentials:

**Model.** Morphons become **grain-voices** scrubbing audio files attached to
TimbralAnchors. Pitch is decoupled — MIDI note → grain playback rate (note 60 =
source's natural pitch), manifold geometry → scrub read-position. On-concept:
geometry drives timbre/position, not an envelope.

**Source-grouped blend (the core rule).** Group a Morphon's RBF-weighted anchors
by source:
- **Within a source group → interpolate parameters, render once.** Same-file
  granular anchors → read-position = weighted average of their positions (a true
  *scrub*). Additive anchors → blend timbreX/Y (the existing additive engine is
  now just the degenerate case).
- **Across source groups → crossfade rendered outputs**, gain = `sqrt(weight
  share)` (equal-power, generalised to N groups; groups not rendered contribute no
  power → no additive bleed). Granular↔additive and granular↔different-file are
  both crossfades; you only *scrub* within a shared buffer.
- N same-file anchors → read-position is a weighted centroid in buffer-time, so a
  swirling Morphon time-warps for free.

**Per-anchor grain fields** (each a spatial RBF field): readPosition, density,
jitter, spray, grainSize, pitchSemis, a positionEnabled toggle (off = "texture
waypoint": contributes texture but not read-position), and a per-anchor **volume**
(a loudness field blended over *all* anchors, not just granular). Plus a global
grain-level trim.

**Level & gain staging.** Loaded samples are peak-normalised to ~−1 dBFS (so grain
level doesn't ride on the file's recording level), and the grain path has a makeup
gain (`GRAIN_MAKEUP`, compensates the Hann window's ~0.5 average) bringing it to
additive parity. Voice gain chain: `envelope × VOICE_SCALE(0.12) × emitter Level ×
anchor Volume field` → grain makeup on the grain portion → **Master** at the bus.
Three volume controls exist: Master (bound to the `MASTER_GAIN` APVTS param,
host-automatable), per-Emitter **Level** (baked at note-on, like pan/mass — affects
new notes, not held ones), per-anchor **Volume** (live each tick).

**Current state.** Shipped: foundation + grain cloud + per-anchor knobs + the
level/volume layer + the **source picker and waveform scrub**. Each anchor has a
source dropdown (Additive + every loaded source) and a draggable waveform marker —
so two anchors can share a `sourceId` and a Morphon **scrubs** between their read
positions (N same-file anchors → time-warp). Still open: a **single granular group
wins** (no cross-source crossfade — other sources fade to silence rather than
crossfading), mono only, sample data not yet embedded in patches, and the grain
**levels are pending ear-confirmation** (tune `GRAIN_MAKEUP` / normalize target if
hot or shy).

Key files: `synthesis/SampleSource.h` (mono source, atomic-published to audio),
`synthesis/GrainEngine.h` (per-voice Hann grain pool), `blendAnchorsGranular` in
`synthesis/TimbralAnchor.h`, grain render in `PluginProcessor.cpp::processBlock`,
`WaveformDisplay.h` + the source picker in `PluginEditor.cpp`.

## Conventions (summary — full list in CLAUDE.md)

Ask on ambiguity; build after every change; C++17; never positional-init the
object structs; `Parameters.h` ID strings are forever; `globalFriction_` is a
decay rate (1/s); never commit/push without an explicit request; commit messages
are descriptive (no Conventional-Commits prefix) with a `Co-Authored-By` footer.

## Roadmap (condensed; reprioritise with Julian)

- **Granular next**: cross-source crossfade (render multiple groups + additive
  instead of single-dominant-group), then stereo grains, DAW drag-and-drop, patch
  embed. (Source picker, waveform scrub, level/volume layer are done.)
- **Undo/redo + Ctrl/Cmd+Z** — state-memento approach (snapshot `PatchState`),
  with edit coalescing.
- **Object groups + modulatable group + Ticker** — group-scoped Morphon
  interaction; a crossing-advanced step-sequencer mod source.
- **Shift+click multi-select**; object-distance mod source; friction-curve tweak.
- **Manifold size/aspect as parameters**; real-unit grid + tempo sync + isometric
  + snap-to-grid.
- **macOS port** — prep done (see `MAC_PORT.md`); remaining work is Mac-side.
- **Future**: React/webview editor (parallel-branch experiment); Sheaf Synthesis
  (Julian's next-paradigm idea, post-Morphos).

## Collaborator

Julian originated the Morphos concept and has a strong, specific vision —
clarifying questions are cheaper than wrong guesses. He likes being offered a
short menu of options rather than having a path picked for him.
