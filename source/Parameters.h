#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Parameters.h — Stable parameter ID registry
//
// RULE: Never rename or remove a string in this file once a version ships.
// Ableton and other DAWs key automation and session state to these exact
// strings. A rename silently breaks existing sessions.
//
// CONVENTION: IDs are lowercase_snake_case. One constant per parameter.
// Group by object type. Add new parameters at the bottom of their group.
// ─────────────────────────────────────────────────────────────────────────────

namespace ParamID
{
    // ── Global ────────────────────────────────────────────────────────────────
    inline constexpr const char* MASTER_GAIN       = "master_gain";
    inline constexpr const char* GLOBAL_TIME_SCALE = "global_time_scale";

    // ── Morphon defaults (Emitter-level; per-voice overrides come later) ──────
    // (reserved for Phase 4+)

    // ── Field objects (reserved for Phase 5+) ─────────────────────────────────

    // ── Mod matrix (reserved for Phase 6+) ───────────────────────────────────

} // namespace ParamID
