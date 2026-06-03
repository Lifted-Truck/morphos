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

    // ── Mod matrix (Phase 6) ─────────────────────────────────────────────────
    // Eight host-automatable macro knobs. Each is a normalised [0, 1] float
    // exposed as a VST3/AU parameter so the DAW (Ableton) can map it onto
    // its own macro knobs / hardware. The mod matrix reads them as a source
    // (ModSourceType::Macro, index = 0..7) which can fan out to any number
    // of destinations with independent depths.
    inline constexpr const char* MACRO[8] = {
        "macro_1", "macro_2", "macro_3", "macro_4",
        "macro_5", "macro_6", "macro_7", "macro_8"
    };

} // namespace ParamID
