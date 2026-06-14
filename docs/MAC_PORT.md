# Morphos — macOS port guide

Resume-on-Mac handoff. The project is CMake + JUCE (FetchContent), so the port is
small: the build system and code are already cross-platform. This doc is what the
Mac session needs that isn't obvious from the code. (The build command in the
root `CLAUDE.md` is the Windows MSBuild flow — on macOS use the CMake commands
below instead.)

## Status going in

- Builds cleanly on Windows (VST3, x64 Debug, VS 2026 / MSBuild).
- `CMakeLists.txt` is portable: JUCE 8.0.13 via FetchContent, no hardcoded paths,
  no Win32 code anywhere in `source/`.
- **AU is already wired**: `FORMATS` is `VST3` everywhere, plus `AU` on Apple
  (gated by `if(APPLE)`), and `BUNDLE_ID = com.liftedtruck.morphos` is set.
- So on the Mac you should get **VST3 + AU** out of the box once it compiles.

## Prerequisites

- macOS with **command-line tools** (`xcode-select --install`). Full Xcode is
  *not* required — the validated flow uses the Unix Makefiles generator. (The
  dev Mac, Apple Silicon, has CLT only: no Xcode, no Ninja.)
- **CMake ≥ 3.22** (`brew install cmake`).
- Network access (FetchContent clones JUCE 8.0.13 on first configure).

## Build (validated flow)

From the **repository root** (the clone, e.g. `morphos/` — the directory holding
`CMakeLists.txt`; there is no nested `plugin/` subfolder):

```sh
cmake --preset mac-debug                # = -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
```

(`-G "Xcode"` / `-G "Ninja"` also work if installed, but Unix Makefiles is the
flow that's been validated end-to-end on the dev machine. Note Makefiles are
single-config: the build type is fixed at configure time, so use the
`mac-release` preset / a separate build dir for Release.)

**Claude Code sessions on the dev Mac** — two environment gotchas:
- The shell sandbox resets cwd between Bash calls, so a relative
  `cmake --build build` silently no-ops (empty bundle skeletons, exit 0). Always
  pass the **absolute** build path.
- Writes to `~/Library/...` and `auval`/`codesign`/`killall` hit a sandbox
  overlay, not the real disk — an install can look successful while Ableton sees
  nothing. Run install/validate/sign commands with `dangerouslyDisableSandbox: true`.

Artifacts:
- VST3 → `build/Morphos_artefacts/Debug/VST3/Morphos.vst3`
- AU   → `build/Morphos_artefacts/Debug/AU/Morphos.component`

Install (or set `COPY_PLUGIN_AFTER_BUILD TRUE` in CMakeLists to automate):
- VST3 → `~/Library/Audio/Plug-Ins/VST3/`
- AU   → `~/Library/Audio/Plug-Ins/Components/`

**Code-signature seal (don't skip).** JUCE regenerates the VST3's
`Contents/Resources/moduleinfo.json` *after* ad-hoc-signing the bundle, which
breaks the signature seal (`codesign --verify` → "a sealed resource is missing or
invalid"). A bundle with a broken seal is **silently skipped by Ableton's scanner**
— it never appears in the browser and leaves no entry in Live's `Log.txt`, even
with "Use VST3 System Folders" on and after a Rescan. CMakeLists now re-signs both
bundles as a `POST_BUILD` step on Apple, so a normal build is loadable as-is. If
you ever sign by hand, do it *after* the build: `codesign --force -s -
~/Library/Audio/Plug-Ins/VST3/Morphos.vst3`. (Tip: when a plugin won't appear,
check `~/Library/Preferences/Ableton/Live <ver>/Log.txt` — a beta uses its own
versioned folder.)

## Validate the AU

Logic/GarageBand won't load an AU until it passes `auval`. Codes: type `aumu`
(music device / instrument, since `IS_SYNTH`), subtype = `PLUGIN_CODE` (`Mph1`),
manufacturer = `PLUGIN_MANUFACTURER_CODE` (`Mrph`):

```sh
auval -v aumu Mph1 Mrph
```

Must print "AU VALIDATION SUCCEEDED". If a host caches a stale AU, reset with
`killall -9 AudioComponentRegistrar` and rescan.

## Test

- **VST3**: Ableton Live (mac), Bitwig, Reaper.
- **AU**: Logic, GarageBand, Live (after `auval` passes).
- Re-verify the granular **file drag-and-drop from the DAW** here — whether
  Ableton/Logic expose a real file path on drag-out can differ from Windows
  (the file-picker path via `FileChooser` is unaffected and cross-platform).

## Likely first-compile fixes (clang/libc++ is stricter than MSVC)

None are known to exist — `source/` has no Win32 code — but these are the usual
suspects when a MSVC-clean project first meets clang:
- **Missing `#include`s** MSVC provided transitively: `<cmath>`, `<algorithm>`,
  `<cstdint>`, `<memory>`, `<limits>`. Add them where the error points.
- **Unqualified math**: a few physics headers use `exp(` / `sin(` without `std::`
  (`physics/EffectZone.h`, `physics/TrajectoryPath.h`). Resolve via `<cmath>`'s
  global names on both compilers; qualify with `std::` if clang objects.
- **Two-phase template lookup**: qualify dependent base-class members with
  `this->` if clang can't find them.
- **`char` signedness / narrowing** in brace-init — clang is stricter.
- **Case-sensitive include paths** — match header case exactly (APFS can be
  case-sensitive).

Fix forward, rebuild, repeat — this is normally a handful of one-line edits.

## Signing & notarization (only for distribution, not local dev)

Local builds run under Gatekeeper without signing. For sharing/release:
1. `codesign` each bundle with a **Developer ID Application** cert; enable
   **Hardened Runtime** (add `HARDENED_RUNTIME TRUE` to `juce_add_plugin`, or sign
   manually with `--options runtime`).
2. Zip and submit with `xcrun notarytool submit … --wait`.
3. `xcrun stapler staple` the bundles.
Defer all of this until the plugin builds, validates, and runs.

## Reference

- Manufacturer code: `Mrph` · Plugin code: `Mph1` · Bundle ID: `com.liftedtruck.morphos`
- Repo: `Lifted-Truck/morphos`, branch `master`. Git is the only bridge between
  machines — pull latest before starting.
- Rich design/roadmap context lives in Claude's project memory on the Windows
  machine and does **not** travel with the repo; the committed `CLAUDE.md` files
  (root + `source/physics/`) are the in-repo context. Copy the memory files over,
  or ask for the load-bearing parts to be distilled into the repo, if needed.
