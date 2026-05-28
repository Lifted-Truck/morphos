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
- [x] **Phase 5 (partial)** — Effect Zones: circular spatial modulators for TimbreX, TimbreY, Amplitude, Pan, Pitch; Linear/Gaussian falloff; additive accumulation per tick; depth range auto-switches to ±24 semitones for Pitch target
  - ⏳ *Remaining:* Flux Gates, Path Objects
- [ ] **Phase 6** — Modulation: mod matrix, MIDI sources, Morphon state sources, MPE. Priority destinations: Emitter position XY, launch angle, launch speed (keytracking these to MIDI pitch is the canonical Morphos expressive relationship). All field object XY positions must be both sources and destinations.
- [ ] **Phase 7** — Additional engines: FM, wavetable, heterogeneous blending. **Transient Objects**: percussive event synthesis layer triggered by Emitter generation, Terminus arrival, Event Horizon absorption, and Flux Gate crossings. **Emitter unison/detune mode**: N Morphons per note with per-parameter spread (angle, speed, detune, pan, mass); spread values are mod destinations.
- [ ] **Phase 8** — Scaling: spatial hash, SIMD, engine LOD, physics quality settings
- [ ] **Phase 9** — Advanced: granular, physical model, spectral engines, full mod matrix
- [ ] **Phase 10** — Product: timbral visualizer (partial bar graph / FFT waterfall for latest Morphon), object staging area (configure defaults before placement), patch randomizer, per-Morphon visual identity (note labels + per-note colour), preset browser, factory patches, GUI polish, code signing

**Known deferred issues:**
- **Topology-aware anchor blending** — Timbral Anchor IDW uses raw Euclidean distance; crossing a wrap boundary causes a sharp timbral discontinuity. Fix: use `min(|Δx|, 1−|Δx|)` per axis conditioned on `globalBoundary`. The discontinuity has its own character and should be a toggle once the continuous version exists. Target: Phase 5 remainder.
- **Tuning modes** — current pitch calculation is equal temperament only. Harmonic series and ratio-based mapping queued for a later phase (no fixed target).
- **Manifold aspect-ratio handling** — currently the Manifold stretches to fill the canvas, so resizing the window non-uniformly changes the relative dynamics of objects and the apparent scale of radii. This is interesting as an authoring tool but should not be the only mode. Options to design: lock 1:1 (letterbox), free-stretch (current), or global rescale (uniform scaling that preserves dynamics). Likely surfaced as a startup-menu preference.

**UI wishlist (Phase 10+):**
- **Object layer panel** — list of all existing objects on the canvas with selectable rows; alt-click on canvas cycles through objects beneath the cursor (the panel surfaces what's covered). Layer order should be reorderable to control z-stack and selection priority.
- **Timbral visualizer** — partial amplitude bar graph is the lowest-cost option given the existing additive engine; oscilloscope or FFT waterfall are alternatives. Shows timbre of the most recently launched Morphon.
- **Object staging area** — configure Emitter/Anchor/Zone defaults in the panel before clicking to place, so the first instance lands already tuned rather than requiring post-placement edits.
- **Per-Morphon visual identity** — note number or pitch-class label on each Morphon dot; per-note colour generation (toggleable). Makes polyphonic patches legible at a glance.
- **Patch randomizer** — randomise parameters with configurable mutation depth and optional seeded anchoring ("randomise around current patch").
- Piano keyboard strip at the bottom — lights active MIDI notes, clickable for in-VST testing.

---

## Far-future concepts

- **Morphogenetic MIDI Generator** — a sibling panel / plugin that runs the same Manifold physics engine as a *generative MIDI source*: Morphon positions trigger notes, Flux Gate crossings fire rhythmic events, Trigger Anchors define pitch mappings. The most powerful form shares geometry with the synthesis Manifold so field objects simultaneously shape timbres and generate melodies. Closed-loop routing (generator output → Morphos input) creates self-sustaining generative systems. Full spec in `morphos_orientation.md`.

## Future concepts (not in current scope)

- **Morph Surface** — 2D wavetable Anchor engine: interpolation at waveform data level rather than audio output level. Tabled until core system is audible.
- **Acceleration saturation** — Morphon acceleration vector drives a waveshaper (magnitude → drive depth, angle → character blend). Tabled until angular character map can be designed by ear.
- **Sheaf Synthesis** — A separate future synthesizer paradigm. Sound as constraint satisfaction over time-frequency space; cohomological obstructions as sonic events. Flagged for post-Morphos development.
