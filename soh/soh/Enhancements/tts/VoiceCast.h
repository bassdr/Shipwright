#pragma once

#include <cstdint>

namespace SOH {

// Which voice a line is spoken in, or nullptr when the speaker is not known.
//
// There is deliberately no default voice. A line nobody has cast stays silent:
// the game is voiced by the characters we are sure of and by nobody else, and
// putting some stand-in voice on an unattributed line would be the same fault as
// playing the wrong character. Silence is also not a cue to speak the line some
// other way - the accessibility reader is its own feature, driven by its own
// setting, and voice acting never falls back into it.
//
// Keyed only on the text id, because the baker walks the message table offline
// where no actor exists; anything the runtime knew and the baker did not could
// only name a clip that was never baked.
[[nodiscard("returns the profile to load with; it changes nothing")]] const char*
VoiceProfileForText(uint16_t textId) noexcept;

} // namespace SOH
