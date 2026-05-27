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

```bat
# From the plugin/ directory:
cmake -B build
cmake --build build --config Debug
```

First configure downloads JUCE via FetchContent (~300 MB, one time only).

**Plugin output:** `build\Morphos_artefacts\Debug\VST3\Morphos.vst3`

**Load in Ableton:** Preferences → Plug-Ins → VST3 Custom Folder → add the path above → Rescan.

For Release builds:
```bat
cmake --build build --config Release
```

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
2. **Manifold objects** — custom `ManifoldObjects` XML child (Anchors, Emitters, field objects)
   Added in Phase 3+; slot is present in the format from Phase 0 for forward compatibility.

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
- [ ] **Phase 1** — Physics core: Morphon integration, Attractor/Repeller/Vortex, precomputed field grid
- [ ] **Phase 2** — First sound: additive engine, two Timbral Anchors, RBF blending, ADSR
- [ ] **Phase 3** — Manifold authoring: drag-and-drop Anchors, Emitter placement, parameter panels
- [ ] **Phase 4** — Polyphony & key-tracking: voice management, Terminus, MIDI mapping
- [ ] **Phase 5** — Full field model: Effect Zones, Flux Gates, Path Objects
- [ ] **Phase 6** — Modulation: mod matrix, MIDI sources, Morphon state sources, MPE
- [ ] **Phase 7** — Additional engines: FM, wavetable, Transient Objects, heterogeneous blending
- [ ] **Phase 8** — Scaling: spatial hash, SIMD, engine LOD, physics quality settings
- [ ] **Phase 9** — Advanced: granular, physical model, spectral engines, full mod matrix
- [ ] **Phase 10** — Product: preset system, GUI polish, factory patches, code signing

---

## Future concepts (not in current scope)

- **Morph Surface** — 2D wavetable Anchor engine: interpolation at waveform data level rather than audio output level. Tabled until core system is audible.
- **Acceleration saturation** — Morphon acceleration vector drives a waveshaper (magnitude → drive depth, angle → character blend). Tabled until angular character map can be designed by ear.
- **Sheaf Synthesis** — A separate future synthesizer paradigm. Sound as constraint satisfaction over time-frequency space; cohomological obstructions as sonic events. Flagged for post-Morphos development.
