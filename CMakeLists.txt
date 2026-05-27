cmake_minimum_required(VERSION 3.22)

project(Morphos VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── JUCE ─────────────────────────────────────────────────────────────────────
# Pinned to a specific tag for reproducible builds.
# Check https://github.com/juce-framework/JUCE/releases for the latest stable.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.13          # <-- update to latest stable before each new phase
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ── Plugin target ─────────────────────────────────────────────────────────────
juce_add_plugin(Morphos
    VERSION                     0.1.0
    COMPANY_NAME                "Morphos"
    PLUGIN_MANUFACTURER_CODE    Mrph        # 4 chars, identifies manufacturer
    PLUGIN_CODE                 Mph1        # 4 chars, unique per plugin
    FORMATS                     VST3
    PRODUCT_NAME                "Morphos"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    # Set COPY_PLUGIN_AFTER_BUILD TRUE and set VST3_COPY_DIR if you want
    # automatic install after each build (requires admin on Windows).
    # For now: add the build output path to Ableton's VST3 custom folder instead.
    # Build output: build/Morphos_artefacts/Debug/VST3/Morphos.vst3
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

# ── Sources ───────────────────────────────────────────────────────────────────
target_sources(Morphos
    PRIVATE
        source/PluginProcessor.cpp
        source/PluginEditor.cpp
        source/physics/PhysicsEngine.cpp
)

target_include_directories(Morphos
    PRIVATE
        source/
)

# ── Compile definitions ───────────────────────────────────────────────────────
target_compile_definitions(Morphos
    PUBLIC
        JUCE_WEB_BROWSER=0          # not needed
        JUCE_USE_CURL=0             # not needed
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=1    # free tier: splash screen required
        JUCE_REPORT_APP_USAGE=0
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(Morphos
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
