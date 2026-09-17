#pragma once

#include <memory>
#include <string>
#include <vector>

namespace Ship {
class Console;
}

// Console command `tts_dump`: writes every textbox of the message table, decoded
// exactly as the speech hook would read it, to a manifest the offline baker turns
// into voice clips.
bool VoiceTextDumpHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                          std::string* output);
