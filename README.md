# Morphos

A VST3 synthesizer built on **Morphogenetic Synthesis** — a novel paradigm where sound
particles (Morphons) travel through a 2D vector field (the Manifold), and timbral
evolution is a consequence of geometry rather than envelopes. There are no LFOs routing
into filter cutoff. The path a Morphon traces *is* the sound's timbral biography.

Full design specification: [`../morphos_orientation.md`](../morphos_orientation.md)

---

## Prerequisites

| Tool | Version | Install |
|---|---|---|
| Visual Studio | 2026 (MSVC) | See [VS setup](#visual-studio-setup) below |
| CMake | 3.22+ | `winget install Kitware.CMake` |
| Git | any | already present |
| GitHub CLI | any | `winget install GitHub.cli` |

---

## Visual Studio Setup

Open the **Visual Studio Installer** → **Modify** on your VS 2026 installation.

**Workload to install:**
- ✅ **Desktop development with C++**

**Individual components to verify are checked** (within that workload):
- ✅ MSVC v1xx – C++ x64/x86 build tools (latest)
- ✅ Windows 11 SDK (latest available)
- ✅ C++ CMake tools for Windows
- ✅ C++ core features

**Recommended additions:**
- ✅ C++ Clang tools for Windows *(catches warnings MSVC misses; useful for keeping code clean)*
- ✅ C++ AddressSanitizer *(runtime memory error detection; invaluable during physics development)*

**Leave unchecked** (not needed): .NET, Python, Azure, UWP, Game Development with C++.

> **CMake PATH note:** VS installs its own cmake, but only in the Developer Command Prompt.
> Install `Kitware.CMake` via winget separately so `cmake` works in any terminal.

---

## Build

```powershell
# From the plugin/ directory — first time or after wiping build/:
cmake --preset debug

# Subsequent builds:
cmake --build --preset debug

# Release:
cmake --preset release
cmake --build --preset release
```

First configure downloads JUCE via FetchContent (~300 MB, one time only).
Generator is locked to `Visual Studio 18 2026 / x64` via `CMakePresets.json` — no need to specify `-G` manually.

**Plugin output:** `build\Morphos_artefacts\Debug\VST3\Morphos.vst3`

**Load in Ableton:** Preferences → Plug-Ins → VST3 Custom Folder → add the path above → Rescan.

---

## Architecture

### Threading model

```
┌─────────────────┐   lock-free triple buffer   ┌──────────────────────┐
│  Physics thread │ ──────────────────────────► │   Audio thread       │
│  500 Hz tick    │                             │   processBlock()     │
│  PhysicsEngine  │ ◄───────────────────────── │   note events (SPSC) │
└─────────────────┘                             └──────────────────────┘
         │                                                │
         │  relaxed copy (~30 Hz)                        │
         ▼                                               ▼
┌─────────────────┐                             ┌──────────────────────┐
│   UI thread     │                             │  APVTS parameters    │
│  Editor / Timer │                             │  (atomic reads)      │
└─────────────────┘                             └──────────────────────┘
```

**Rules enforced in `processBlock()`:** no heap allocation, no mutex, no blocking OS calls.
All physics↔audio communication is wait-free. Parameter reads use cached `std::atomic<float>*`.

### Key files

| File | Role |
|---|---|
| `source/Parameters.h` | Stable parameter ID strings — never rename after shipping |
| `source/physics/PhysicsState.h` | `MorphonState`, `PhysicsStateSnapshot`, triple-buffer `PhysicsAudioBridge`, `NoteEvent` |
| `source/physics/PhysicsEngine.h/.cpp` | Physics thread (500 Hz), SPSC note queue, global time scale |
| `source/PluginProcessor.h/.cpp` | Audio callback, MIDI → physics, APVTS, state serialisation |
| `source/PluginEditor.h/.cpp` | UI thread, 30 Hz physics poll, placeholder canvas |

### State serialisation

Two layers in `getStateInformation` / `setStateInformation`:
1. **APVTS parameters** — automatically serialised by JUCE ValueTree
2. **Manifold objects** — custom `ManifoldObjects` XML child appended to the APVTS state tree.
   Format: `version=2` attribute, one child per active object (`FieldObject`, `Emitter`, `Anchor`, `Zone`),
   plus global topology attributes (`boundary`, `glideTime`) and editor window dimensions (`editorW`, `editorH`).
   Each Emitter carries its own `polyMode`. Saves from the `latestSnapshotForUI_` copy; restores by
   building a `PatchState` and calling `PhysicsEngine::applyPatch()` (stops/replaces/restarts the
   simulation thread). Version 1 saves (global `polyMode`) are upgraded transparently: the old value
   is broadcast to every Emitter. Saves without `ManifoldObjects` restore APVTS params only.

### Parameter IDs

Stable string constants in `Parameters.h`. **Never rename or remove** an ID after a session
is shipped — DAW automation is keyed to these strings.

---

## Decisions log

| Decision | Choice | Rationale |
|---|---|---|
| Language / framework | C++ / JUCE 8.x | Industry standard for VST3; strong CMake support |
| Build system | CMake + FetchContent | Reproducible, IDE-agnostic, no Projucer dependency |
| Platform | Windows (VST3) | Primary target; Mac/AU can be added later |
| GUI approach | JUCE native components → OpenGL (Phase 3+) | Software renderer for early phases; switch to GPU when Morphon animation demands it |
| Physics tick rate | 500 Hz (configurable) | Perceptually indistinguishable from 1000 Hz for macro movement; 5–10× CPU reduction |
| Physics → audio | Wait-free triple buffer | No blocking on audio thread; audio always reads latest complete tick |
| Audio → physics | SPSC ring buffer (128 events) | Lock-free note event delivery |
| Parameter system | JUCE AudioProcessorValueTreeState | Stable IDs, automation, undo, preset serialisation |
| Max polyphony target | 64 simultaneous Morphons | Mid-range CPU; spatial hash added before this threshold |
| Electronegativity scaling | Spatial hash (Phase 8) | Naïve O(n²) adequate below ~64 Morphons |
| SIMD | AVX2 on FM/additive engines (Phase 8) | 4–8× throughput on homogeneous engine groups |
| JUCE license | Personal / free tier | Splash screen present; upgrade to Indie/Pro for commercial release |

---

## Phase progress

- [x] **Phase 0** — Plugin skeleton: CMake, JUCE, threading model, parameter system, state hooks
- [x] **Phase 1** — Physics core: Morphon integration, Attractor/Repeller/Vortex, precomputed field grid
- [x] **Phase 2** — First sound: additive engine (20 partials), Timbral Anchors, IDW blending, full ADSR
- [x] **Phase 3** — Manifold authoring: drag-and-drop for all object types, parameter panel (strength/radius/chirality/ADSR/key range/transpose/pan/terminus), per-Morphon trails, amplitude→opacity rendering, click-to-place spawn mode (arm type, click canvas), Delete/Backspace shortcut, patch save/load (DAW session persistence)
- [x] **Phase 4** — Polyphony & key-tracking: 256-voice pool, per-Emitter key ranges, per-Emitter transpose (Oct/Semi/Cents), per-Emitter pan, per-Emitter mass (Morphon resistance to field forces), per-Emitter Poly/Mono/Legato/Slur (each Emitter routes its own notes — mix mono bass + poly lead on the same key range), Terminus (key-off attractor with arrival detection), pitch glide/portamento (log-space interpolation), spectral crossfade to suppress clicks at Manifold boundary crossings
- [x] **Phase 5** — Field-model extensions:
  - **Effect Zones** — circular spatial modulators for TimbreX, TimbreY, Amplitude, Pan, Pitch; Linear/Gaussian falloff; additive accumulation per tick; depth range auto-switches to ±24 semitones for Pitch target.
  - **Flux Gates** — line-segment objects on the Manifold. A Morphon's per-tick trajectory (prev→cur) is intersection-tested against each gate; on crossing, the Morphon's envelope snaps back to Attack (terminus state cleared so a post-noteOff re-trigger doesn't drag toward terminus). Only held voices re-trigger — released voices skip the gate so updateEnvelopes doesn't immediately undo the snap. Geometry is centre + length + angle for clean drag-to-move and panel sliders.
  - **Path Objects (rail constraint)** — closed-circle curves that pin nearby Morphons. Each tick after integration: unpinned Morphons within `snapRadius` pin to the closest point on the curve; pinned Morphons have their position snapped to the curve and velocity projected onto the local tangent. Field forces still apply through the integrator — only their tangential component survives the projection, so the Morphon walks along the rail driven by the field's tangential pull. Path removal unpins all attached Morphons. v1 ships Circle only; the `PathShape` enum and `pathClosestPoint()` indirection leave room for Line / Arc / Polyline / Bezier without changing storage or call sites.
  - **Trajectory Paths (position driver, AutoPlay)** — curves whose `currentT` advances by `speed * dt` each tick (closed shapes wrap; open shapes will ping-pong when Line/Arc shapes land). An Emitter with `trajectoryPathIndex >= 0` has its `(x, y)` sampled from the path each tick, before notes drain — so MIDI notes spawned this tick read the updated Emitter position. The dashed peach ring with a moving head dot reads as "this object cycles". v1 ships circle + AutoPlay only; Manual mode (currentT driven by mod matrix) lands with Phase 6.
- [ ] **Phase 6** — Modulation: mod matrix, MIDI sources, Morphon state sources, MPE.
  - **Priority destinations:** Emitter position XY, launch angle, launch speed (keytracking these to MIDI pitch is the canonical Morphos expressive relationship). All field-object XY positions must be both sources and destinations. Per-Morphon mass.
  - **Path Objects as modulation sources** — Path Objects exist (v1: circle). For Phase 6 they should expose pinned-Morphon arc-length / tangent-angle / current-XY as mod-matrix sources, so any parameter can be driven by where a Morphon is on the rail. Adding richer shapes (Line, Arc, Polyline, Bezier) feeds the same source interface unchanged.
  - **Tempo-synced sources** — host-tempo LFOs (BPM ratios, divisions, dotted/triplet) and tempo-locked triggers ("fire every 1/16"). Wires through `juce::AudioPlayHead::CurrentPositionInfo` for BPM + transport position so anything in the matrix (Emitter retrigger, field-object position, gate position, zone depth) can lock to the song grid. Critical for using Morphos as a rhythmic synthesizer; without it, geometry-driven motion is free-running only.
- [ ] **Phase 7** — Additional engines: FM, wavetable, heterogeneous blending. **Transient Objects**: percussive event synthesis layer triggered by Emitter generation, Terminus arrival, Event Horizon absorption, and Flux Gate crossings. **Emitter unison/detune mode**: N Morphons per note with per-parameter spread (angle, speed, detune, pan, mass); spread values are mod destinations.
- [ ] **Phase 8** — Scaling: spatial hash, SIMD, engine LOD, physics quality settings, **offline-rendering correctness** (switch physics from wall-clock to buffer-clock advance under `isNonRealtime()`)
- [ ] **Phase 9** — Advanced: granular, physical model, spectral engines, full mod matrix, **tuning modes** (harmonic series, ratio-based pitch mapping, alternate temperaments)
- [ ] **Phase 10** — Product:
  - **Preset / patch system** — internal preset browser independent of DAW session save: save to/load from disk, named slots, categories, factory patches, A/B compare. Distinct from the existing `getStateInformation` path (which serialises into the DAW session).
  - **Right-click context menus** — grow over time. On empty canvas: spawn each object type + "Generate Random" (creates a random object with randomised parameters). On an object: motion-path drawing (bezier curves, click-to-add waypoints) and pre-formed motion shapes (circle / Lissajous / spiral) placed via a stretchable bounding box. Right-click on a slider: copy/paste value, "copy from Emitter N" etc.
  - **Object copy / paste** — Ctrl+C / Ctrl+V on the canvas; also "copy parameters from Emitter N" via the right-click menu so a new Emitter can mirror an existing one's settings.
  - **Typed X/Y position input** — numeric entry for every selected object's Manifold position (in addition to drag), so positions can be set precisely.
  - **Panel scrolling / `juce::Viewport`** — Phase-5 work shipped a section-overlap fix that collapsed ~500px of stacked invisible sliders. The Emitter section still runs ~600px tall on its own, so wrapping the panel content in a `Viewport` for vertical scroll is the next step once any further controls land. Compact mode (smaller fonts / shorter sliders) is a fallback if scrolling proves awkward.
  - **Gate endpoint drag** — currently a Flux Gate drags as a rigid segment from any point along its line (centre + length + angle stays bound together). Endpoints should be individually grabbable so rotation/length can be set on-canvas without falling back to the Length / Angle sliders. Likely uses a small hit zone at each endpoint distinct from the main line.
  - **Timbral visualizer** — partial amplitude bar graph (cheapest given the additive engine); oscilloscope or FFT waterfall are alternatives. Shows the timbre of the most recently launched Morphon.
  - **Object layer panel** — list of all objects on the canvas with selectable rows; alt-click on canvas cycles through objects beneath the cursor (the panel surfaces what's covered). Layer order reorderable to control z-stack and selection priority.
  - **Object staging area** — configure Emitter/Anchor/Zone defaults in the panel before clicking to place, so the first instance lands already tuned rather than requiring post-placement edits.
  - **Per-Morphon visual identity** — note number or pitch-class label on each Morphon dot; per-note colour generation (toggleable).
  - **Patch randomizer** — randomise parameters with configurable mutation depth and optional seeded anchoring ("randomise around current patch").
  - **Piano keyboard strip** at the bottom — lights active MIDI notes, clickable for in-VST testing.
  - GUI polish, code signing.
- [ ] **Phase 11** — Cross-platform: macOS port (AU + VST3 universal binary, code signing & notarisation, Retina-aware rendering). Build system already CMake-based; the JUCE side is portable, but anything Windows-specific (paths, file dialogs, font assumptions) gets audited here.

**Known deferred issues:**
- **Offline-rendering / Bounce** — the physics thread is wall-clock-driven (a `juce::Thread` ticking at 500 Hz of real time), so during Ableton's faster-than-realtime render (Freeze, Bounce in Place, Export Audio) the audio thread renders many buffers per wall second while physics still advances at only 500 ticks/sec. The bounced audio will not match live playback. Fix: detect `isNonRealtime()` in `processBlock` and advance physics inline with `dt = numSamples / sampleRate`. Threaded path stays for live use. Target: Phase 8.
- **Topology-aware anchor blending** — Timbral Anchor IDW uses raw Euclidean distance; crossing a wrap boundary causes a sharp timbral discontinuity. Fix: use `min(|Δx|, 1−|Δx|)` per axis conditioned on `globalBoundary`. The discontinuity has its own character and should be a toggle once the continuous version exists.
- **Manifold aspect-ratio handling** — currently the Manifold stretches to fill the canvas, so resizing the window non-uniformly changes the relative dynamics of objects and the apparent scale of radii. This is interesting as an authoring tool but should not be the only mode. Options to design: lock 1:1 (letterbox), free-stretch (current), or global rescale (uniform scaling that preserves dynamics). Likely surfaced as a startup-menu preference.

**Phase 5 follow-ups (queued for a later pass through the field-model chapter):**
- **Open-path shapes** — Line / Arc / Polyline / Bezier shape variants for both rail and trajectory paths. Open paths ping-pong rather than wrap; the `PathShape` enum and `pathClosestPoint()` / `trajectoryPathSample()` indirection is already in place, so each shape is one closest-point + one sample-position implementation.
- **Tangent-force paths (guide-field bias)** — third path-object variant. Instead of constraining Morphons (rail) or driving objects (trajectory), the path emits a tangential vector-field bias that nudges nearby free Morphons along its tangent. Soft constraint; composes with Attractors/Repellers/Vortices.
- **Curved Flux Gates** — Flux Gates that use a path shape (line / arc / bezier) instead of a single line segment. Same crossing semantics (re-trigger envelope on prev→cur intersection); just a richer geometry that can wrap around an Attractor or trace an irregular boundary.
- **Off-path escape** — currently a Morphon pinned to a rail stays pinned for its lifetime (path removal or voice death are the only escape paths). Add an escape condition (velocity threshold, MIDI source, explicit unpin event from a Flux Gate, or a panel-toggleable "no escape / escape on impulse / escape on note-off").
- **Manual-mode trajectory paths** — `currentT` is driven externally rather than by `speed * dt`. Lands with Phase 6 mod matrix: `currentT` becomes a destination, any mod source (LFO, MIDI CC, Morphon position) can drive it. Slider in the Trajectory panel selects mode.
- **Global friction slider** — a global multiplier on per-Morphon drag. At 0, Morphons hold velocity indefinitely (terminal velocity bounded only by field-force magnitudes); at max, motion decays to zero in a fraction of a second. Sits next to Glide Time in the always-visible top section.

---

## Far-future concepts

- **Morphogenetic MIDI Generator** — a sibling panel / plugin that runs the same Manifold physics engine as a *generative MIDI source*: Morphon positions trigger notes, Flux Gate crossings fire rhythmic events, Trigger Anchors define pitch mappings. The most powerful form shares geometry with the synthesis Manifold so field objects simultaneously shape timbres and generate melodies. Closed-loop routing (generator output → Morphos input) creates self-sustaining generative systems. Full spec in `morphos_orientation.md`.

## Future concepts (not in current scope)

- **Morph Surface** — 2D wavetable Anchor engine: interpolation at waveform data level rather than audio output level. Tabled until core system is audible.
- **Acceleration saturation** — Morphon acceleration vector drives a waveshaper (magnitude → drive depth, angle → character blend). Tabled until angular character map can be designed by ear.
- **Sheaf Synthesis** — A separate future synthesizer paradigm. Sound as constraint satisfaction over time-frequency space; cohomological obstructions as sonic events. Flagged for post-Morphos development.
