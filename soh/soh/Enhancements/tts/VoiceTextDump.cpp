#include "VoiceTextDump.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "soh/OTRGlobals.h"
#include "soh/SaveManager.h"
#include "soh/Enhancements/tts/VoiceClips.h"
#include "soh/Enhancements/tts/VoiceCast.h"
#include "soh/Enhancements/tts/VoiceBaker.h"

extern "C" {
#include <z64.h>
#include "functions.h"
#include "macros.h"
#include "message_data_fmt.h"
#include "message_data_static.h"

extern PlayState* gPlayState;
extern SaveContext gSaveContext;
extern MessageTableEntry* sNesMessageEntryTablePtr;
extern MessageTableEntry* sGerMessageEntryTablePtr;
extern MessageTableEntry* sFraMessageEntryTablePtr;
extern size_t sNesMessageEntryCount;
extern size_t sGerMessageEntryCount;
extern size_t sFraMessageEntryCount;
}

// Defined in tts.cpp, which is what the speech hook itself calls. Going through
// the same function is the whole point: a reimplementation would drift and every
// drifted line would silently miss its clip at runtime.
std::string Message_TTS_Decode(uint8_t* sourceBuf, uint16_t startOfset, uint16_t size);

// Also tts.cpp. The button and stick names the decoder splices in come from text
// banks keyed on the interface language rather than on the message table, so a
// dump has to move both together or English lines come out with French buttons.
void InitTTSBank();

// Also tts.cpp.
const char* GetLanguageCode();

namespace {

// A message that never terminates would otherwise spin forever; the longest
// vanilla message is well under this.
constexpr uint16_t kMaxBoxesPerMessage = 64;

// Message_Decode emits one cache-invalidate per font glyph and the dump runs
// thousands of decodes in a single frame, so the write head is rewound before
// each one. It is rewound to where the frame already was rather than to a list
// of our own: something downstream keeps the pointer, and pointing it at memory
// outside the arena is what used to take the renderer down.
constexpr ptrdiff_t kGfxPerDecode = 256;

struct LanguageTarget {
    uint8_t id;
    const char* code;
};

constexpr std::array<LanguageTarget, 3> kLanguages = { LanguageTarget{ LANGUAGE_ENG, "en-US" },
                                                       LanguageTarget{ LANGUAGE_GER, "de-DE" },
                                                       LanguageTarget{ LANGUAGE_FRA, "fr-FR" } };

struct MessageTable {
    MessageTableEntry* entries;
    size_t count;
};

// An NTSC extraction carries no PAL message tables, and Message_FindMessage then
// reads the English one whatever the interface language says. Dumping the same
// fallback is what makes the clips findable in that setup.
[[nodiscard("the table and its count are only meaningful together")]] MessageTable
TableForLanguage(uint8_t language) noexcept {
    if (language == LANGUAGE_GER && sGerMessageEntryTablePtr != nullptr) {
        return { sGerMessageEntryTablePtr, sGerMessageEntryCount };
    }
    if (language == LANGUAGE_FRA && sFraMessageEntryTablePtr != nullptr) {
        return { sFraMessageEntryTablePtr, sFraMessageEntryCount };
    }
    return { sNesMessageEntryTablePtr, sNesMessageEntryCount };
}

// The file-name alphabet: digits, then A-Z, then a-z, with 0x3E for a blank.
[[nodiscard("returns the encoded byte rather than writing it")]] constexpr uint8_t EncodeNameChar(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<uint8_t>(c - 'A' + 0x0A);
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<uint8_t>(c - 'a' + 0x24);
    }
    return 0x3E;
}

[[nodiscard("returns the escaped copy; the argument is untouched")]] std::string
EscapeForManifest(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\t') {
            escaped += ' ';
        } else if (c == '\\') {
            escaped += "\\\\";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

// The dump runs mid-frame against the live save and the live message context, so
// it puts back what it borrowed. The message buffers are left dirty on purpose:
// Message_OpenText refills them, along with the language bookkeeping, before the
// next real textbox reads a byte.
class BorrowedState {
  public:
    explicit BorrowedState(PlayState* play)
        : mPlay(play), mLanguage(gSaveContext.language), mFilenameLanguage(gSaveContext.ship.filenameLanguage),
          mMsgMode(play->msgCtx.msgMode), mInterfaceLanguage(CVarGetInteger(CVAR_SETTING("Languages"), 0)),
          mPolyOpa(play->state.gfxCtx->polyOpa.p) {
        std::memcpy(mPlayerName.data(), gSaveContext.playerName, mPlayerName.size());

        gSaveContext.ship.filenameLanguage = NAME_LANGUAGE_PAL;
        const size_t nameLength = std::strlen(SOH::kBakedPlayerName);
        for (size_t i = 0; i < mPlayerName.size(); i++) {
            gSaveContext.playerName[i] = i < nameLength ? EncodeNameChar(SOH::kBakedPlayerName[i]) : 0x3E;
        }
    }

    ~BorrowedState() {
        mPlay->state.gfxCtx->polyOpa.p = mPolyOpa;
        mPlay->msgCtx.msgMode = mMsgMode;
        gSaveContext.ship.filenameLanguage = mFilenameLanguage;
        gSaveContext.language = mLanguage;
        std::memcpy(gSaveContext.playerName, mPlayerName.data(), mPlayerName.size());
        CVarSetInteger(CVAR_SETTING("Languages"), mInterfaceLanguage);
        InitTTSBank();
    }

    BorrowedState(const BorrowedState&) = delete;
    BorrowedState& operator=(const BorrowedState&) = delete;

  private:
    PlayState* mPlay;
    uint8_t mLanguage;
    uint8_t mFilenameLanguage;
    uint8_t mMsgMode;
    int32_t mInterfaceLanguage;
    std::array<uint8_t, ARRAY_COUNT(gSaveContext.playerName)> mPlayerName{};
    Gfx* mPolyOpa;
};

size_t DumpLanguage(PlayState* play, const LanguageTarget& language, size_t limit, std::ofstream& manifest) {
    MessageContext* msgCtx = &play->msgCtx;
    Font* font = &msgCtx->font;
    Gfx* const writeHead = play->state.gfxCtx->polyOpa.p;
    size_t written = 0;

    gSaveContext.language = language.id;
    CVarSetInteger(CVAR_SETTING("Languages"), language.id);
    InitTTSBank();

    const MessageTable table = TableForLanguage(language.id);
    for (size_t index = 0; index < std::min(table.count, limit); index++) {
        const MessageTableEntry* entry = &table.entries[index];
        const size_t length = std::min<size_t>(entry->msgSize, sizeof(font->msgBuf));
        std::memcpy(font->msgBuf, entry->segment, length);
        // Message_Decode walks to a terminator and bounds nothing on the way, so
        // a message that does not carry one - a truncated copy, or a table entry
        // whose size is wrong - decodes until it finds a random matching byte and
        // writes the result straight through msgBufDecoded into the rest of
        // PlayState. One of those overwrote view.gfxCtx.
        const size_t terminator = std::min(length, sizeof(font->msgBuf) - 1);
        font->msgBuf[terminator] = MESSAGE_END;

        font->charTexBuf[0] = entry->typePos;
        font->msgLength = static_cast<uint32_t>(length);
        msgCtx->msgLength = static_cast<int32_t>(length);
        msgCtx->textId = entry->textId;
        msgCtx->textBoxProperties = entry->typePos;
        msgCtx->textBoxType = msgCtx->textBoxProperties >> 4;
        msgCtx->textBoxPos = msgCtx->textBoxProperties & 0xF;
        msgCtx->msgBufPos = 0;
        msgCtx->choiceNum = 0;
        msgCtx->textboxEndType = 0;
        msgCtx->textUnskippable = 0;
        msgCtx->textDrawPos = 0;

        for (uint16_t box = 0; box < kMaxBoxesPerMessage; box++) {
            play->state.gfxCtx->polyOpa.p = writeHead;
            Message_Decode(play);
            // Leaves what the decode wrote as a well-formed list rather than as
            // commands followed by whatever the arena held.
            gSPEndDisplayList(play->state.gfxCtx->polyOpa.p);

            const uint16_t decoded = std::min<uint16_t>(msgCtx->decodedTextLen, sizeof(msgCtx->msgBufDecoded));
            const std::string text = Message_TTS_Decode(msgCtx->msgBufDecoded, 0, decoded);
            // An uncast line is left out entirely rather than baked in some
            // stand-in voice, so the corpus only ever holds lines we are sure of.
            const char* const profile = SOH::VoiceProfileForText(entry->textId);
            if (!text.empty() && profile != nullptr) {
                manifest << SOH::VoiceClipHashHex(text) << '\t' << profile << '\t' << std::hex << entry->textId
                         << std::dec << '\t' << box << '\t' << EscapeForManifest(text) << '\n';
                written++;
            }

            if (msgCtx->msgBufPos >= length) {
                break;
            }

            // Message_Decode stops on the control code that ends the textbox and,
            // except for a delayed break, leaves the read position on it.
            const uint8_t boxEnd = static_cast<uint8_t>(font->msgBuf[msgCtx->msgBufPos]);
            if (boxEnd == MESSAGE_END || boxEnd == MESSAGE_TEXTID || boxEnd == MESSAGE_EVENT) {
                break;
            }
            if (boxEnd == MESSAGE_BOX_BREAK) {
                msgCtx->msgBufPos++;
            }
        }
    }

    return written;
}

} // namespace

bool VoiceTextDumpHandler([[maybe_unused]] std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                          std::string* output) {
    const size_t limit = args.size() > 1 ? std::stoul(args[1]) : std::numeric_limits<size_t>::max();

    PlayState* play = gPlayState;
    if (play == nullptr || play->state.gfxCtx == nullptr) {
        *output = "tts_dump needs to run in-game.";
        return 1;
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE) {
        *output = "tts_dump cannot run while a textbox is open.";
        return 1;
    }
    if (play->state.gfxCtx->polyOpa.d - play->state.gfxCtx->polyOpa.p < kGfxPerDecode) {
        *output = "tts_dump needs room left in the frame's display list; try again next frame.";
        return 1;
    }

    const BorrowedState borrowed(play);

    for (const LanguageTarget& language : kLanguages) {
        const std::string directory = SOH::VoiceLanguageDirectory(language.code);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        const std::string path = directory + "/manifest.tsv";
        std::ofstream manifest(path, std::ios::trunc);
        if (!manifest.is_open()) {
            SPDLOG_ERROR("Could not write the voice manifest to {}", path);
            assert(false);
            continue;
        }

        const size_t written = DumpLanguage(play, language, limit, manifest);
        SPDLOG_INFO("Wrote {} lines to {}", written, path);
        *output += (output->empty() ? "" : "  ") + std::string(language.code) + ": " + std::to_string(written);
    }

    return 0;
}

bool VoiceSayHandler([[maybe_unused]] std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                     std::string* output) {
    if (args.size() < 3) {
        *output = "usage: tts_say <voice> <line to speak>";
        return 1;
    }

    std::string text;
    for (size_t i = 2; i < args.size(); i++) {
        if (i > 2) {
            text += ' ';
        }
        text += args[i];
    }

    SOH::VoiceBaker::Instance().Request(text, GetLanguageCode(), args[1], 1.0f);
    *output = "queued for " + args[1];
    return 0;
}
