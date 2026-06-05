# Physics engine — local conventions

All mutable Manifold state lives here and is mutated **only on the physics
thread**. External threads communicate via lock-free FIFOs: `ManifoldEdit` in
(from UI), `PhysicsStateSnapshot` out (to audio + UI, triple-buffered).

## Realtime safety

- **No heap allocation, no locks, no blocking syscalls** on the physics thread or
  in the audio `processBlock`. Fixed-size arrays only.
- New per-tick work slots into `tick(dt)` and must stay allocation-free.

## Tick order — `PhysicsEngine::tick(dt)`

Memorise this; new features almost always slot into a specific step:

1. `drainEditCommands` — apply UI edits first so the rest of the tick sees them.
2. `advanceTrajectoryPaths` — autoplay trajectories advance `currentT`.
3. `updateAttachedEmitters` — every object with `trajectoryPathIndex >= 0` gets
   `(x,y)` sampled from its trajectory; FieldObject moves flag `fieldGrid_.dirty`.
4. `drainNoteEvents` (Pass 1) — refresh MIDI mod-source state (CC, last-note,
   velocity); buffer note-on/off into `pendingNoteEvents_`.
5. `evaluateModMatrix` — global mod write using fresh trajectory + MIDI state.
6. `applyPendingNoteEvents` (Pass 2) — for each note-on: re-update keytrack/velocity,
   **re-run the mod matrix**, then `handleNoteOn`. This per-note re-eval is what
   makes simultaneous-key chords spawn with correct per-note modulation. **Do not
   move the mod-matrix call back before `drainNoteEvents`.**
7. `rebuildFieldGridIfDirty`.
8. `integrateMorphons` — semi-implicit Euler; `globalFriction_` is the only damping.
   Anchor blending (additive timbre + dominant granular source/scrub/weight) is
   written per Morphon here via `blendAnchorsGranular`.
9. `applyPathConstraints` — Rail snap + tangent projection + escape check.
10. `applyFluxGates` — crossing detection (Line: segment-segment; Circle: signed-
    distance flip) → envelope re-trigger on held voices.
11. `updateEnvelopes`.
12. `applyEffectZones`.
13. `writeSnapshot` + `bridge_.publish()`.

## Object lifecycle

- Most types use **first-inactive-slot** allocation; Remove sets `active = false`
  and indices stay stable.
- **TimbralAnchors are slot-compacted**: active anchors occupy `[0, activeAnchorCount_)`;
  Remove swaps the removed slot with the last active one. Anything that adds or
  copies anchors must preserve this invariant.
- New objects must reset **all** reused fields (e.g. `trajectoryPathIndex`,
  granular `sourceId` / `readPosition`) — slots are recycled, so stale state leaks
  in otherwise.
- Any object Add / Remove (and patch load) bumps `configVersion_` so the editor
  refreshes its mod-matrix dropdowns.
