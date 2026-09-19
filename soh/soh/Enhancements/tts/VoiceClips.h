#pragma once

#include <cstdint>
#include <string>

namespace SOH {

// The name control code has to bake to one fixed name: clips are shared between
// players, and a player called anything else still hears this one.
inline constexpr const char* kBakedPlayerName = "Link";

// The clip name is the whole cache key, so a change to how the audio is made would
// otherwise replay the old file forever. Bump this whenever that happens; existing
// clips fall out of use on the next launch.
// 2: espeak's clause punctuation is fed to the model, so lines carry intonation.
// 3: per-mark pauses and emphasis, and espeak's language markers no longer read out.
// 4: a run of marks is tuned as a run, and the built-in pauses changed with it.
// 5: voice-speech.json is read at all - every clip before this ignored the file.
// 6: laughs are built from a repeated burst rather than spoken.
// 7: laughs are not voiced at all; the rest of the line still is.
constexpr uint64_t kVoiceRecipeVersion = 7;

// Voice clips are content-addressed on the decoded line, so the baker that writes
// them and the lookup that plays them have to agree byte for byte. Both go
// through here rather than each spelling the hash out.
[[nodiscard("the hash is the clip's identity, not a side effect")]] uint64_t
VoiceClipHash(const std::string& text) noexcept;

// Zero-padded to 16 digits: the hash is a file name, and a short one would not
// find the clip the baker wrote.
[[nodiscard("the hash is the clip's identity, not a side effect")]] std::string
VoiceClipHashHex(const std::string& text);

// Holds the manifest and, under it, one directory per voice.
[[nodiscard("returns the path to probe; it does not test for the file")]] std::string
VoiceLanguageDirectory(const std::string& language);

// Clips are filed under the voice they were baked in, so one character can be
// re-cast and re-baked without invalidating everyone else, and two characters can
// speak the same line without colliding on the hash.
[[nodiscard("returns the path to probe; it does not test for the file")]] std::string
VoiceClipDirectory(const std::string& language, const std::string& profile);

[[nodiscard("returns the path to probe; it does not test for the file")]] std::string
VoiceClipPath(const std::string& text, const std::string& language, const std::string& profile);

// A clip is only as current as the tuning that produced it, so one written before
// the last edit to voice-profiles.json or voice-speech.json counts as a miss and
// is synthesised again. Saves remembering to empty the folder after every edit.
[[nodiscard("the answer decides whether the clip is used")]] bool VoiceClipIsCurrent(const std::string& path);

} // namespace SOH
