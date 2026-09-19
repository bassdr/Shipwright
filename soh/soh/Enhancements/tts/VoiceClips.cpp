#include "VoiceClips.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>

#include <ship/core/Context.h>

#include "soh/OTRGlobals.h"

namespace SOH {

uint64_t VoiceClipHash(const std::string& text) noexcept {
    uint64_t hash = 0xcbf29ce484222325ULL ^ kVoiceRecipeVersion;
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

std::string VoiceLanguageDirectory(const std::string& language) {
    return Ship::Context::GetPathRelativeToAppDirectory("voice/" + language, appShortName);
}

std::string VoiceClipDirectory(const std::string& language, const std::string& profile) {
    return VoiceLanguageDirectory(language) + "/" + profile;
}

std::string VoiceClipPath(const std::string& text, const std::string& language, const std::string& profile) {
    return VoiceClipDirectory(language, profile) + "/" + VoiceClipHashHex(text) + ".opus";
}

bool VoiceClipIsCurrent(const std::string& path) {
    // Read once: the configs themselves are read once per launch, so a clip cannot
    // be shaped by an edit this run has not seen either.
    static const std::filesystem::file_time_type tuned = []() {
        std::filesystem::file_time_type newest{};
        for (const char* name : { "voice-profiles.json", "voice-speech.json" }) {
            std::error_code ec;
            const auto stamp =
                std::filesystem::last_write_time(Ship::Context::GetPathRelativeToAppDirectory(name, appShortName), ec);
            if (!ec && stamp > newest) {
                newest = stamp;
            }
        }
        return newest;
    }();

    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    return !ec && written >= tuned;
}

} // namespace SOH
