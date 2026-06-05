# Morphos — VST3 morphogenetic synthesizer

Sound particles ("Morphons") spawn from Emitters and travel a 2D vector field
(the "Manifold") shaped by user-placed objects. Timbre evolves from a particle's
geometric trajectory, not from amplitude envelopes — **the Manifold IS the
modulation source.** Do not add envelope-as-timbre logic; it dilutes the core
concept and is a known tripwire.

## Build & test

Windows, Visual Studio 2026, x64 Debug. From `plugin/`:

```
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" \
  build/Morphos_All.vcxproj -p:Configuration=Debug -p:Platform=x64 -m -nologo -v:m
```

- Output VST3: `build/Morphos_artefacts/Debug/VST3/Morphos.vst3`. Tested by reloading in Ableton.
- **Build after every substantive change** — compile errors are cheap to catch now, expensive later.
- **C++17.** (e.g. no `= default` for `operator==`; write comparison ops by hand.)
- **macOS** (VST3 + AU via CMake/Xcode): see `docs/MAC_PORT.md` — the command above is Windows-only.

## Architecture (one screen)

Three threads, lock-free communication:
- **Physics** (`PhysicsEngine : juce::Thread`, 500 Hz) — owns all Manifold state.
  See `source/physics/CLAUDE.md` for the tick order and realtime rules.
- **Audio** (`processBlock`) — forwards MIDI to physics via an SPSC FIFO, reads a
  triple-buffered `PhysicsStateSnapshot`, runs the additive + granular voices.
- **UI** (`MorphosEditor`, 30 Hz) — sends `ManifoldEdit` commands to physics via a
  second SPSC FIFO.

Canvas object types live in `source/physics/` (+ `synthesis/TimbralAnchor.h`), each
mirrored into `PhysicsStateSnapshot`. Rich design context (object taxonomy, mod
matrix, roadmap) is maintained in Claude's project memory, not here — keep this
file lean.

## Gotchas that have caused real bugs

- **No positional aggregate initialisers** for the object structs
  (`timbralAnchors_[0] = { ... }`). Adding a field silently shifts values into the
  wrong slot. Assign field-by-field.
- **`Parameters.h` ID strings are forever** — DAW sessions key to them. Add new
  IDs; never rename or remove an existing one.
- **`globalFriction_` is a decay rate per second** (1/s), not a per-tick fraction;
  the integrator converts via `1 − exp(−rate·dt)`. It is the *only* damping —
  per-Morphon `m.drag` / `Emitter::spawnDrag` are ignored (kept for patch-format
  compat; don't reintroduce them in the math).
- **`PathShape::Line`** is honoured only by `TrajectoryPath`; Rails and Flow still
  treat it as Circle. Don't extend without flagging it.

## Working conventions

- **Ask, don't assume.** On ambiguous scope / UX / math / naming, ask one focused
  question before writing code rather than shipping a "reasonable" guess.
- **Git lives in `plugin/`** (`Lifted-Truck/morphos`, branch `master`). The parent
  workspace folder is not version-controlled — run git from here.
- **Never commit or push without an explicit request.** "Works" / "looks good" is a
  green-light to *propose* a commit; wait for "commit" / "ship it" before running it.
- **Commit style**: descriptive title (no Conventional Commits prefix) + body
  explaining intent and any subtle tradeoffs. Footer:
  `Co-Authored-By: Claude <noreply@anthropic.com>` (match your model name).
- **No destructive git** (`reset --hard`, `push --force`, `checkout --`, hook/signing
  bypasses) without explicit instruction. Don't touch git config.
