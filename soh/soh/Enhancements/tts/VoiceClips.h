#pragma once

#include <cstdint>
#include <string>

namespace SOH {

// The name control code has to bake to one fixed name: clips are shared between
// players, and a player called anything else still hears this one.
inline constexpr const char* kBakedPlayerName = "Link";

// Voice clips are content-addressed on the decoded line, so the baker that writes
// them and the lookup that plays them have to agree byte for byte. Both go
// through here rather than each spelling the hash out.
[[nodiscard("the hash is the clip's identity, not a side effect")]] uint64_t
VoiceClipHash(const std::string& text) noexcept;

// Zero-padded to 16 digits: the hash is a file name, and a short one would not
// find the clip the baker wrote.
[[nodiscard("the hash is the clip's identity, not a side effect")]] std::string
VoiceClipHashHex(const std::string& text);

[[nodiscard("returns the path to probe; it does not test for the file")]] std::string
VoiceClipDirectory(const std::string& language);

[[nodiscard("returns the path to probe; it does not test for the file")]] std::string
VoiceClipPath(const std::string& text, const std::string& language);

} // namespace SOH
