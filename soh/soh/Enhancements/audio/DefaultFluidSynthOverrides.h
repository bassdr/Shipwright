#pragma once

// ---------------------------------------------------------------------------
// Built-in FluidSynth per-pair overrides shipped with SoH.
//
// Currently EMPTY (no entries). All (fontId, instOrWave) pairs default to
// the engine's native synth, which always sounds the way the original game
// intended. Users who want to experiment with GM substitutions can do so
// via the bypass UI; their choices persist to fluidsynth_overrides.json
// in the SoH config directory.
//
// Why no defaults: the 2026-05-30 audit (FluidR3_GM.sf2 calibration target)
// found that a generic GM SoundFont fundamentally can't reproduce the
// original timbre for most pairs. The N64 audio engine uses heavily
// customised samples — per-key critical splits with their own ADSR, pan,
// and attenuation; baked-in reverb and filter character; sample data
// reused across slots with different intent per font. ANMP's
// "Reproducing N64 OSTs accurately" wiki documents the same conclusion
// for the broader N64 catalogue. For the few pairs where a GM
// substitution genuinely improves on the sample (rare — typically only
// when the native sample is muffled or low-resolution), the user opts
// in per pair. We do not pre-bake those decisions because they're
// soundfont-specific and subjective.
//
// PR 4.2 will move the substrate to mod-shipped JSON: a mod author can
// distribute a SoundFont *purpose-built* for OoT's instrument layout
// (one of the community HD packs) plus a mapping pack tuned for that
// font. That's the realistic path to "better-than-native"; generic GM
// substitution is a tinkering tool, not a default.
// ---------------------------------------------------------------------------

namespace SOH {

inline constexpr const char* kDefaultFluidSynthOverridesJson = R"({
  "version": 1,
  "entries": []
})";

} // namespace SOH
