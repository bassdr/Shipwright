#include "VoiceCast.h"
#include "VoiceCastGenerated.h"

#include <array>

namespace SOH {

namespace {

struct CastRange {
    uint16_t first;
    uint16_t last;
    const char* profile;
};

// Hand casting, consulted before the generated table so story dialogue can be
// attributed where the decompiled source names no actor - which is most of it,
// since cutscene text is triggered from scene data rather than from actor code.
constexpr std::array<CastRange, 9> kCastByText = { {
    // The decomp hands these to Navi's actor because it is what puts the box on
    // screen; the voice on the other end of the ocarina is Saria's.
    { 0x00E1, 0x00E3, "saria" },
    // Navi owns two whole bands rather than individual ids, which is why the
    // decomp scan finds none of her lines: Elf_Msg and the C-up hint walker both
    // return `byte2 | 0x100`, and z_player.c asks for `naviEnemyId + 0x600`.
    { 0x0100, 0x01FF, "navi" },
    { 0x0600, 0x06FF, "navi" },
    { 0x106C, 0x106C, "sheik" },
    { 0x5022, 0x5022, "impa" },
    { 0x7060, 0x7060, "zelda" },
    { 0x7077, 0x7077, "sheik" },
    { 0x7082, 0x7082, "zelda" },
    { 0x70FF, 0x70FF, "zelda" },
} };

} // namespace

const char* VoiceProfileForText(const uint16_t textId) noexcept {
    for (const CastRange& range : kCastByText) {
        if (textId >= range.first && textId <= range.last) {
            return range.profile;
        }
    }
    for (const GeneratedCastEntry& entry : kGeneratedCast) {
        if (entry.textId == textId) {
            return entry.profile;
        }
    }
    return nullptr;
}

} // namespace SOH
