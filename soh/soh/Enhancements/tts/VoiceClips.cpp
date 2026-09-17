#include "VoiceClips.h"

#include <cinttypes>
#include <cstdio>

#include <ship/core/Context.h>

#include "soh/OTRGlobals.h"

namespace SOH {

uint64_t VoiceClipHash(const std::string& text) noexcept {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text) {
        hash = (hash ^ c) * 0x100000001b3ULL;
    }
    return hash;
}

std::string VoiceClipHashHex(const std::string& text) {
    char hex[17];
    snprintf(hex, sizeof(hex), "%016" PRIx64, VoiceClipHash(text));
    return std::string(hex);
}

std::string VoiceClipDirectory(const std::string& language) {
    return Ship::Context::GetPathRelativeToAppDirectory("voice/" + language, appShortName);
}

std::string VoiceClipPath(const std::string& text, const std::string& language) {
    return VoiceClipDirectory(language) + "/" + VoiceClipHashHex(text) + ".opus";
}

} // namespace SOH
