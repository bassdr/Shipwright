#include "MidiTranslator.h"
#include "GmInstrumentMap.h"
#include "soh/cvar_prefixes.h"
#include <ship/audio/MidiSynthManager.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

// Define DEBUG_FONT_MAP at build time (e.g. -DDEBUG_FONT_MAP) to emit a CSV
// of every (fontId, instOrWave, semitone, freqScale) pair that fires.
// Play through each scene, then inspect the log to fill in kUnmapped slots
// in GmInstrumentMap.h or to identify instruments needing a transpose
// override (PR 4).
#ifdef DEBUG_FONT_MAP
#include <cstdio>
#include <mutex>
static FILE*      sDbgFile  = nullptr;
static std::mutex sDbgMutex;

static void DbgLogNote(uint8_t fontId, int16_t instOrWave, uint8_t semitone,
                       float freqScale, bool mapped, uint8_t gmBank, uint8_t gmProg) {
    std::lock_guard<std::mutex> lk(sDbgMutex);
    if (!sDbgFile) {
        sDbgFile = fopen("font_map_dump.csv", "w");
        if (sDbgFile)
            fprintf(sDbgFile,
                    "fontId,instOrWave,instOrWave_hex,semitone,expectedMidi,"
                    "freqScale,mapped,gmBank,gmProgram\n");
    }
    if (sDbgFile)
        fprintf(sDbgFile, "%u,%d,0x%02X,%u,%d,%.6f,%s,%u,%u\n",
                fontId, instOrWave, (uint8_t)instOrWave, semitone,
                static_cast<int>(semitone) + 21, freqScale,
                mapped ? "yes" : "no", gmBank, gmProg);
}
#define DBG_LOG(fontId, iow, semi, freq, mapped, bank, prog) \
    DbgLogNote((fontId), (iow), (semi), (freq), (mapped), (bank), (prog))
#else
#define DBG_LOG(...) (void)0
#endif

namespace SOH {

// Engine semitone → MIDI note offset for melodic instruments. The
// engine's gNoteFrequencies table labels semitone 0x27 (39) as
// "NOTE_C4 (Middle C)" with freqScale 1.0, which by table-label would
// suggest offset 60 - 39 = 21. In practice the sample banks are
// recorded an octave higher than the table label implies (semitone 39
// actually plays at MIDI C5), so the correct offset against a
// standard-tuning GM SF2 is 33. Confirmed by ear via the discovery
// slider in PR 3.1: +12 from the original +21 matched native pitch.
static constexpr int kEngineSemitoneToMidiOffset = 33;

// Held at near-max so the SF2's default velocity → initialAttenuation
// modulator doesn't silence quiet notes. Dynamics are driven by CC11
// (expression) instead. PR 2 will replace the default modulators with
// linear ones; until then, this constant keeps audibility predictable.
static constexpr uint8_t kFixedNoteOnVelocity = 100;

// FluidSynth::kPitchBendRangeSemitones is ±12. We clamp slightly below
// that to keep some headroom for the integer rounding on the FluidSynth
// side.
static constexpr float kMaxPitchBendSemitones = 12.0f;

MidiTranslator::MidiTranslator() {
    // mProgramOverride uses -1 as "no override"; zero-init would otherwise
    // collide with valid GM program 0 (Acoustic Grand Piano).
    for (auto& row : mProgramOverride)
        for (auto& cell : row)
            cell = -1;
    // PR 7 effect CCs use -1 = no override (0..127 are valid CC values).
    for (auto& row : mReverbByPair)       for (auto& cell : row) cell = -1;
    for (auto& row : mChorusByPair)       for (auto& cell : row) cell = -1;
    for (auto& row : mFilterCutoffByPair) for (auto& cell : row) cell = -1;
    for (auto& row : mFilterQByPair)      for (auto& cell : row) cell = -1;
    // PR 8 bank override: -1 = no override.
    for (auto& row : mBankOverride)       for (auto& cell : row) cell = -1;
    // Ship 1 pack pinning: -1 = unpinned (ProgramChange fallback path).
    for (auto& row : mPinnedSfontId)      for (auto& cell : row) cell = -1;
    // Channel allocator: 0xFF = unallocated.
    for (auto& row : mPairChannel)        for (auto& cell : row) cell = 0xFF;
}

MidiTranslator& MidiTranslator::Instance() {
    static MidiTranslator sInstance;
    return sInstance;
}

void MidiTranslator::Reset() {
    for (NoteTranslatorState& noteState : mNoteState)
        noteState = {};
    for (ChannelState& chState : mChannelState)
        chState = {};
    for (auto& row : mSynthActiveByPair)
        for (auto& cell : row)
            cell = 0;
    for (auto& row : mNativeActiveByPair)
        for (auto& cell : row)
            cell = 0;
    auto synth = Ship::MidiSynthManager::Instance().GetActiveSynth();
    if (!synth)
        return;
    for (uint8_t ch = 0; ch < kMaxMidiChannels; ch++) {
        synth->ControlChange(ch, 123, 0); // CC 123 = All Notes Off
    }
}

// ── Debug UI plumbing: bypass + discovery + per-pair gain/program ─────────

static inline bool BypassIndexValid(uint8_t fontId, int16_t instOrWave) {
    return fontId < MidiTranslator::kMaxFontId &&
           instOrWave >= 0 && instOrWave < MidiTranslator::kMaxInstOrWave;
}

BypassMode MidiTranslator::GetBypass(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return BypassMode::Auto;
    return static_cast<BypassMode>(mBypass[fontId][instOrWave]);
}

void MidiTranslator::SetBypass(uint8_t fontId, int16_t instOrWave, BypassMode mode) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    mBypass[fontId][instOrWave] = static_cast<uint8_t>(mode);
}

int MidiTranslator::DiscoveredSnapshot(DiscoveredPair* out, int outCap) const {
    int n = mDiscoveredCount.load(std::memory_order_acquire);
    if (n > outCap) n = outCap;
    for (int i = 0; i < n; i++) out[i] = mDiscovered[i];
    return n;
}

uint8_t MidiTranslator::GetSynthActiveCount(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0;
    return mSynthActiveByPair[fontId][instOrWave];
}

uint8_t MidiTranslator::GetNativeActiveCount(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0;
    return mNativeActiveByPair[fontId][instOrWave];
}

float MidiTranslator::GetPerInstrumentGain(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0.0f;
    return mGainByPair[fontId][instOrWave];
}

void MidiTranslator::SetPerInstrumentGain(uint8_t fontId, int16_t instOrWave, float gain) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    mGainByPair[fontId][instOrWave] = gain;
}

int16_t MidiTranslator::GetProgramOverride(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return -1;
    return mProgramOverride[fontId][instOrWave];
}

void MidiTranslator::SetProgramOverride(uint8_t fontId, int16_t instOrWave, int16_t program) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    if (program < -1)  program = -1;
    if (program > 127) program = 127;
    mProgramOverride[fontId][instOrWave] = program;
}

int8_t MidiTranslator::GetTransposeOverride(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0;
    return mTransposeByPair[fontId][instOrWave];
}

void MidiTranslator::SetTransposeOverride(uint8_t fontId, int16_t instOrWave, int8_t semis) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    mTransposeByPair[fontId][instOrWave] = semis;
}

// PR 7 effect getters/setters. -1 sentinel = no override; valid CC values
// are 0..127 (out-of-range writes are clamped down to the sentinel so a
// stray UI bug can't put the synth into a permanently broken state).
static inline int8_t ClampCcOrSentinel(int v) {
    if (v < 0)   return -1;
    if (v > 127) return 127;
    return static_cast<int8_t>(v);
}

int8_t MidiTranslator::GetReverbSend(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mReverbByPair[fontId][instOrWave];
}
void MidiTranslator::SetReverbSend(uint8_t fontId, int16_t instOrWave, int8_t cc) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mReverbByPair[fontId][instOrWave] = ClampCcOrSentinel(cc);
}
int8_t MidiTranslator::GetChorusSend(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mChorusByPair[fontId][instOrWave];
}
void MidiTranslator::SetChorusSend(uint8_t fontId, int16_t instOrWave, int8_t cc) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mChorusByPair[fontId][instOrWave] = ClampCcOrSentinel(cc);
}
int8_t MidiTranslator::GetFilterCutoff(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mFilterCutoffByPair[fontId][instOrWave];
}
void MidiTranslator::SetFilterCutoff(uint8_t fontId, int16_t instOrWave, int8_t cc) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mFilterCutoffByPair[fontId][instOrWave] = ClampCcOrSentinel(cc);
}
int8_t MidiTranslator::GetFilterResonance(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mFilterQByPair[fontId][instOrWave];
}
void MidiTranslator::SetFilterResonance(uint8_t fontId, int16_t instOrWave, int8_t cc) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mFilterQByPair[fontId][instOrWave] = ClampCcOrSentinel(cc);
}

int16_t MidiTranslator::GetBankOverride(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mBankOverride[fontId][instOrWave];
}
void MidiTranslator::SetBankOverride(uint8_t fontId, int16_t instOrWave, int16_t bank) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    if (bank < -1)   bank = -1;
    if (bank > 255)  bank = 255; // ProgramChange preset packs bank into the high byte
    mBankOverride[fontId][instOrWave] = bank;
}

int16_t MidiTranslator::GetPinnedSfontId(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave)) return -1;
    return mPinnedSfontId[fontId][instOrWave];
}
void MidiTranslator::SetPinnedSfontId(uint8_t fontId, int16_t instOrWave, int16_t sfontId) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mPinnedSfontId[fontId][instOrWave] = sfontId;
}

std::string MidiTranslator::GetPresetPackName(uint8_t fontId, int16_t instOrWave) const {
    auto it = mPresetMetadata.find({ fontId, instOrWave });
    return it == mPresetMetadata.end() ? std::string{} : it->second.packName;
}
std::string MidiTranslator::GetPresetName(uint8_t fontId, int16_t instOrWave) const {
    auto it = mPresetMetadata.find({ fontId, instOrWave });
    return it == mPresetMetadata.end() ? std::string{} : it->second.presetName;
}
bool MidiTranslator::HasPresetMetadata(uint8_t fontId, int16_t instOrWave) const {
    return mPresetMetadata.find({ fontId, instOrWave }) != mPresetMetadata.end();
}
void MidiTranslator::SetPresetMetadata(uint8_t fontId, int16_t instOrWave,
                                       const std::string& packName, const std::string& presetName) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    if (packName.empty() && presetName.empty()) {
        mPresetMetadata.erase({ fontId, instOrWave });
        return;
    }
    mPresetMetadata[{ fontId, instOrWave }] = { packName, presetName };
}
void MidiTranslator::ClearPresetMetadata(uint8_t fontId, int16_t instOrWave) {
    mPresetMetadata.erase({ fontId, instOrWave });
}

void MidiTranslator::MarkPairAsUserModified(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    mUserModified.insert({ fontId, instOrWave });
}
void MidiTranslator::UnmarkPairAsUserModified(uint8_t fontId, int16_t instOrWave) {
    mUserModified.erase({ fontId, instOrWave });
}
bool MidiTranslator::IsPairUserModified(uint8_t fontId, int16_t instOrWave) const {
    return mUserModified.count({ fontId, instOrWave }) > 0;
}

bool MidiTranslator::IsTemporarilyMuted(uint8_t fontId, int16_t instOrWave) const {
    return mTemporaryMute.count({ fontId, instOrWave }) > 0;
}
void MidiTranslator::SetTemporaryMute(uint8_t fontId, int16_t instOrWave, bool muted) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    if (muted) mTemporaryMute.insert({ fontId, instOrWave });
    else       mTemporaryMute.erase({ fontId, instOrWave });
}
void MidiTranslator::ClearAllTemporaryMutes() {
    mTemporaryMute.clear();
}

float MidiTranslator::GetTemporaryVolume(uint8_t fontId, int16_t instOrWave) const {
    auto it = mTemporaryVolume.find({ fontId, instOrWave });
    return it == mTemporaryVolume.end() ? 1.0f : it->second;
}
void MidiTranslator::SetTemporaryVolume(uint8_t fontId, int16_t instOrWave, float vol) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    if (vol == 1.0f) {
        mTemporaryVolume.erase({ fontId, instOrWave });
    } else {
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 4.0f) vol = 4.0f;
        mTemporaryVolume[{ fontId, instOrWave }] = vol;
    }
}
void MidiTranslator::ClearAllTemporaryVolumes() {
    mTemporaryVolume.clear();
}

std::string MidiTranslator::GetDisplayName(uint8_t fontId, int16_t instOrWave) const {
    auto it = mDisplayName.find({ fontId, instOrWave });
    return it == mDisplayName.end() ? std::string{} : it->second;
}
void MidiTranslator::SetDisplayName(uint8_t fontId, int16_t instOrWave, const std::string& name) {
    if (!BypassIndexValid(fontId, instOrWave)) return;
    if (name.empty()) {
        mDisplayName.erase({ fontId, instOrWave });
    } else {
        mDisplayName[{ fontId, instOrWave }] = name;
    }
}

uint8_t MidiTranslator::AllocateChannelForPair(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0;
    uint8_t& slot = mPairChannel[fontId][instOrWave];
    if (slot != 0xFF)
        return slot;
    if (mChannelsAllocated >= kMaxMidiChannels) {
        // Pool exhausted — overflow pairs all share channel 0. Per-pair
        // effect CCs on these pairs stomp each other, but the basic
        // play-through still works. Logged once so we know if it ever
        // happens in real usage.
        static bool sLoggedOverflow = false;
        if (!sLoggedOverflow) {
            SPDLOG_WARN("[MidiTranslator] Per-pair channel pool exhausted "
                        "({} unique pairs allocated); new pairs share ch 0",
                        (int)kMaxMidiChannels);
            sLoggedOverflow = true;
        }
        slot = 0;
    } else {
        slot = mChannelsAllocated++;
    }
    return slot;
}

void MidiTranslator::ClearDiscovered() {
    // Wipe the discovered list + seen bitmap. Bypass and gain overrides
    // are intentionally preserved so the user keeps their tunings when
    // they clear the list to discover what plays in a new area.
    mDiscoveredCount.store(0, std::memory_order_release);
    for (auto& word : mSeenBits) word = 0;
    for (auto& entry : mDiscovered) entry = {};
}

void MidiTranslator::ResetAllOverrides() {
    for (auto& row : mBypass)             for (auto& cell : row) cell = 0;
    for (auto& row : mGainByPair)         for (auto& cell : row) cell = 0.0f;
    for (auto& row : mProgramOverride)    for (auto& cell : row) cell = -1;
    for (auto& row : mTransposeByPair)    for (auto& cell : row) cell = 0;
    for (auto& row : mReverbByPair)       for (auto& cell : row) cell = -1;
    for (auto& row : mChorusByPair)       for (auto& cell : row) cell = -1;
    for (auto& row : mFilterCutoffByPair) for (auto& cell : row) cell = -1;
    for (auto& row : mFilterQByPair)      for (auto& cell : row) cell = -1;
    for (auto& row : mBankOverride)       for (auto& cell : row) cell = -1;
    for (auto& row : mPinnedSfontId)      for (auto& cell : row) cell = -1;
    mPresetMetadata.clear();
    mDisplayName.clear();
    mUserModified.clear();
    // Channel allocation is runtime state, not an override — leave
    // mPairChannel / mChannelsAllocated alone so pairs keep their stable
    // MIDI channel mapping across an overrides reset.
}

bool MidiTranslator::SaveOverridesToFile(const std::string& path) const {
    nlohmann::json j;
    j["version"] = 1;
    j["entries"] = nlohmann::json::array();

    for (int f = 0; f < kMaxFontId; f++) {
        for (int i = 0; i < kMaxInstOrWave; i++) {
            // Auto-save model (Ship 7): only persist pairs the user
            // explicitly authored via the UI. Pack-supplied mapping.json
            // entries get re-applied through the chain on every load, so
            // including them in the user JSON would just shadow whatever
            // future pack mappings the user might bring in.
            if (!mUserModified.count({ static_cast<uint8_t>(f),
                                       static_cast<int16_t>(i) })) continue;
            uint8_t bypass    = mBypass[f][i];
            float   gain      = mGainByPair[f][i];
            int16_t program   = mProgramOverride[f][i];
            int8_t  transpose = mTransposeByPair[f][i];
            int8_t  reverb    = mReverbByPair[f][i];
            int8_t  chorus    = mChorusByPair[f][i];
            int8_t  cutoff    = mFilterCutoffByPair[f][i];
            int8_t  q         = mFilterQByPair[f][i];
            int16_t bank      = mBankOverride[f][i];

            // display_name on its own is enough to keep the entry — modders
            // ship custom labels for engine pairs ("Hyrule Field strings")
            // independent of any routing override.
            auto dispIt = mDisplayName.find({ static_cast<uint8_t>(f),
                                              static_cast<int16_t>(i) });
            bool hasDisplayName = (dispIt != mDisplayName.end() && !dispIt->second.empty());

            bool hasOverride =
                (bypass != static_cast<uint8_t>(BypassMode::Auto)) ||
                (gain != 0.0f) ||
                (program >= 0) ||
                (transpose != 0) ||
                (reverb >= 0) || (chorus >= 0) ||
                (cutoff >= 0) || (q >= 0) ||
                (bank >= 0) ||
                hasDisplayName;
            if (!hasOverride) continue;

            nlohmann::json entry;
            entry["fontId"]     = f;
            entry["instOrWave"] = i;
            if (bypass != static_cast<uint8_t>(BypassMode::Auto)) {
                entry["bypass"] = (bypass == static_cast<uint8_t>(BypassMode::ForceNative))
                                      ? "native"
                                      : "synth";
            }
            if (gain != 0.0f)      entry["gain"]      = gain;
            if (program >= 0)      entry["program"]   = program;
            if (transpose != 0)    entry["transpose"] = transpose;
            if (reverb >= 0)       entry["reverb"]        = reverb;
            if (chorus >= 0)       entry["chorus"]        = chorus;
            if (cutoff >= 0)       entry["filter_cutoff"] = cutoff;
            if (q >= 0)            entry["filter_q"]      = q;
            // bank is only meaningful with a program override. Skip writing
            // when it'd be a no-op so user JSON stays compact.
            if (bank >= 0 && program >= 0) entry["bank"] = bank;
            // Preset-source metadata for recovery — only present when the
            // user actually picked from the Preset combo (vs typed an
            // override into the JSON by hand). Audible result is still
            // driven by (bank, program); these fields just let the UI
            // surface "authored against pack X preset 'Y'" later.
            auto metaIt = mPresetMetadata.find({ static_cast<uint8_t>(f),
                                                 static_cast<int16_t>(i) });
            if (metaIt != mPresetMetadata.end()) {
                if (!metaIt->second.packName.empty())   entry["pack"]        = metaIt->second.packName;
                if (!metaIt->second.presetName.empty()) entry["preset_name"] = metaIt->second.presetName;
            }
            if (hasDisplayName) {
                entry["display_name"] = dispIt->second;
            }
            j["entries"].push_back(entry);
        }
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        SPDLOG_WARN("[MidiTranslator] SaveOverridesToFile: cannot open {}", path);
        return false;
    }
    out << j.dump(2);
    return out.good();
}

// Shared per-entry application. Parses an already-loaded JSON document and
// overlays each entry onto the current state. Returns the count of entries
// applied so callers can log a sensible summary. Used by both the
// string and file public entry points.
static int ApplyOverridesFromJson(MidiTranslator& self, nlohmann::json& j,
                                  uint8_t (&mBypass)[64][256],
                                  float (&mGainByPair)[64][256],
                                  int16_t (&mProgramOverride)[64][256],
                                  int8_t (&mTransposeByPair)[64][256],
                                  int8_t (&mReverbByPair)[64][256],
                                  int8_t (&mChorusByPair)[64][256],
                                  int8_t (&mFilterCutoffByPair)[64][256],
                                  int8_t (&mFilterQByPair)[64][256],
                                  int16_t (&mBankOverride)[64][256],
                                  bool markUserAuthored) {
    auto entries = j.value("entries", nlohmann::json::array());
    int applied = 0;
    for (const auto& entry : entries) {
        int fontId     = entry.value("fontId", -1);
        int instOrWave = entry.value("instOrWave", -1);
        if (fontId < 0 || fontId >= 64 || instOrWave < 0 || instOrWave >= 256) {
            continue;
        }
        // Mark user-authored pairs so SaveOverridesToFile keeps writing
        // them on auto-save. Defaults + pack mapping.json overlays do NOT
        // mark — those layers belong to the chain, not the user diff.
        if (markUserAuthored) {
            self.MarkPairAsUserModified(static_cast<uint8_t>(fontId),
                                        static_cast<int16_t>(instOrWave));
        }

        std::string bypassStr = entry.value("bypass", std::string{});
        if (bypassStr == "native") {
            mBypass[fontId][instOrWave] = static_cast<uint8_t>(BypassMode::ForceNative);
        } else if (bypassStr == "synth") {
            mBypass[fontId][instOrWave] = static_cast<uint8_t>(BypassMode::ForceSynth);
        }
        // Missing "bypass" key → leave existing bypass value untouched
        // (Auto by default; user JSON may set ForceSynth/ForceNative on top
        //  of built-in defaults, etc.).

        if (entry.contains("gain")) {
            mGainByPair[fontId][instOrWave] = entry.value("gain", 0.0f);
        }
        if (entry.contains("program")) {
            int p = entry.value("program", -1);
            if (p >= 0 && p <= 127)
                mProgramOverride[fontId][instOrWave] = static_cast<int16_t>(p);
        }
        if (entry.contains("transpose")) {
            int t = entry.value("transpose", 0);
            t = std::clamp(t, -127, 127);
            mTransposeByPair[fontId][instOrWave] = static_cast<int8_t>(t);
        }
        // PR 7 effect sends. Each is an optional 0..127 7-bit MIDI CC value;
        // values outside that range are clamped to the sentinel so a typo
        // in a pack's mapping.json can't make the synth go silent.
        auto applyCc = [&](const char* key, int8_t (&arr)[64][256]) {
            if (!entry.contains(key)) return;
            int v = entry.value(key, -1);
            arr[fontId][instOrWave] = (v < 0 || v > 127) ? int8_t(-1) : static_cast<int8_t>(v);
        };
        applyCc("reverb",        mReverbByPair);
        applyCc("chorus",        mChorusByPair);
        applyCc("filter_cutoff", mFilterCutoffByPair);
        applyCc("filter_q",      mFilterQByPair);

        // PR 8 bank override. Default 0 (when missing) is the GM melodic
        // bank — but to keep this field truly optional we only write it
        // when present. Range 0..255 fits the ProgramChange preset byte.
        if (entry.contains("bank")) {
            int b = entry.value("bank", 0);
            if (b < 0)   b = 0;
            if (b > 255) b = 255;
            mBankOverride[fontId][instOrWave] = static_cast<int16_t>(b);
        }
        // PR 8 preset-source metadata: pack + preset_name. Either can be
        // present independently. Stored sparsely on the translator; missing
        // keys leave any prior metadata untouched (same overlay semantics
        // as the rest of the schema).
        if (entry.contains("pack") || entry.contains("preset_name")) {
            std::string pack       = entry.value("pack",        std::string{});
            std::string presetName = entry.value("preset_name", std::string{});
            // If both are empty (explicit `""`), treat that as a clear.
            self.SetPresetMetadata(static_cast<uint8_t>(fontId),
                                   static_cast<int16_t>(instOrWave),
                                   pack, presetName);
        }
        // Ship 3: custom display_name. Modders ship these in mapping.json
        // so the bypass table reads as a labeled list of engine pairs.
        if (entry.contains("display_name")) {
            std::string dn = entry.value("display_name", std::string{});
            self.SetDisplayName(static_cast<uint8_t>(fontId),
                                static_cast<int16_t>(instOrWave), dn);
        }
        applied++;
    }
    return applied;
}

bool MidiTranslator::ApplyOverridesFromString(const std::string& json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[MidiTranslator] ApplyOverridesFromString: parse error: {}", e.what());
        return false;
    }
    // String overlay = chain layer (defaults or pack mapping.json), NOT
    // user diff. Don't mark pairs as user-modified.
    const int applied = ApplyOverridesFromJson(*this, j, mBypass, mGainByPair,
                                               mProgramOverride, mTransposeByPair,
                                               mReverbByPair, mChorusByPair,
                                               mFilterCutoffByPair, mFilterQByPair,
                                               mBankOverride,
                                               /*markUserAuthored=*/false);
    SPDLOG_INFO("[MidiTranslator] ApplyOverridesFromString: applied {} entries", applied);
    return true;
}

bool MidiTranslator::ApplyOverridesFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        // Missing file is normal on first launch.
        SPDLOG_INFO("[MidiTranslator] ApplyOverridesFromFile: no file at {} (first run?)", path);
        return false;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[MidiTranslator] ApplyOverridesFromFile: parse error: {}", e.what());
        return false;
    }
    // File overlay = user JSON. Every entry parsed from disk is by
    // definition the user's diff, so mark each pair as user-modified to
    // keep them in the next auto-save.
    const int applied = ApplyOverridesFromJson(*this, j, mBypass, mGainByPair,
                                               mProgramOverride, mTransposeByPair,
                                               mReverbByPair, mChorusByPair,
                                               mFilterCutoffByPair, mFilterQByPair,
                                               mBankOverride,
                                               /*markUserAuthored=*/true);
    SPDLOG_INFO("[MidiTranslator] ApplyOverridesFromFile: applied {} entries from {}",
                applied, path);
    return true;
}

void MidiTranslator::RecordDiscovery(uint8_t fontId, int16_t instOrWave, bool mapped) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    const size_t bitIdx  = static_cast<size_t>(fontId) * kMaxInstOrWave + instOrWave;
    const size_t wordIdx = bitIdx / 64;
    const uint64_t mask  = uint64_t(1) << (bitIdx & 63);
    if (mSeenBits[wordIdx] & mask)
        return;
    mSeenBits[wordIdx] |= mask;

    int slot = mDiscoveredCount.load(std::memory_order_relaxed);
    if (slot >= kMaxDiscovered)
        return;
    mDiscovered[slot] = { fontId, instOrWave, mapped };
    mDiscoveredCount.store(slot + 1, std::memory_order_release);
}

// ── ProcessNote ───────────────────────────────────────────────────────────

bool MidiTranslator::ProcessNote(int noteIndex, float freqScale, float velocity,
                                 uint8_t pan, float channelVolume, uint8_t fontId,
                                 int16_t instOrWave, uint8_t semitone, bool isFinished,
                                 uint8_t channelIdx, float resampleRate) {
    (void)resampleRate; // no longer used for the integer note; pitch bend uses ratios

    if (noteIndex < 0 || noteIndex >= kMaxNotes)
        return false;

    auto synth = Ship::MidiSynthManager::Instance().GetActiveSynth();
    if (!synth)
        return false;

    NoteTranslatorState& state = mNoteState[noteIndex];

    // ── Resolve GM preset + bypass ────────────────────────────────────────
    GmPreset gm = GetGmPreset(fontId, instOrWave);
    BypassMode bypass = GetBypass(fontId, instOrWave);

    RecordDiscovery(fontId, instOrWave, gm.program != kUnmapped);

    DBG_LOG(fontId, instOrWave, semitone, freqScale,
            gm.program != kUnmapped, gm.bank, gm.program);

    // Drop this slot's current routing (Synth or Native) and decrement the
    // matching per-pair counter. Used on transitions and on NoteOff.
    auto retireSlot = [&]() {
        if (state.kind == SlotKind::Synth) {
            synth->NoteOff(state.channel, state.midiNote);
            if (BypassIndexValid(state.pairFontId, state.pairInstOrWave) &&
                mSynthActiveByPair[state.pairFontId][state.pairInstOrWave] > 0) {
                mSynthActiveByPair[state.pairFontId][state.pairInstOrWave]--;
            }
        } else if (state.kind == SlotKind::Native) {
            if (BypassIndexValid(state.pairFontId, state.pairInstOrWave) &&
                mNativeActiveByPair[state.pairFontId][state.pairInstOrWave] > 0) {
                mNativeActiveByPair[state.pairFontId][state.pairInstOrWave]--;
            }
        }
        state.kind = SlotKind::Idle;
        state.pairInstOrWave = -1;
    };

    // Mark this slot as routing the given pair through the engine's native
    // synth. Retires any previous routing first. Used by the early-return
    // paths below where ProcessNote returns false.
    auto adoptNative = [&]() {
        if (state.kind != SlotKind::Native ||
            state.pairFontId != fontId || state.pairInstOrWave != instOrWave) {
            retireSlot();
            state.kind           = SlotKind::Native;
            state.pairFontId     = fontId;
            state.pairInstOrWave = instOrWave;
            if (BypassIndexValid(fontId, instOrWave) &&
                mNativeActiveByPair[fontId][instOrWave] < 255) {
                mNativeActiveByPair[fontId][instOrWave]++;
            }
        }
    };

    // ── Transient mute (Solo button) ──────────────────────────────────────
    // Pairs in the mute set are silent on BOTH paths: we return true so the
    // C hook flips bitField0.enabled = false (suppresses native), and we
    // also skip the synth NoteOn / channel-state updates below. retireSlot
    // first to clean up any prior synth/native bookkeeping; the set is
    // session-only (never persisted, cleared on Reset/scene transitions).
    if (mTemporaryMute.count({ fontId, instOrWave })) {
        retireSlot();
        return true;
    }

    // Bypass: ForceNative drops the note from FluidSynth entirely.
    if (bypass == BypassMode::ForceNative) {
        if (isFinished || velocity <= 0.0f) {
            retireSlot();
        } else {
            adoptNative();
        }
        return false;
    }

    // Unmapped (and not forced to synth) → let native render it.
    if (gm.program == kUnmapped && bypass != BypassMode::ForceSynth) {
        static int sLogCount = 0;
        if (sLogCount < 32) {
            SPDLOG_INFO("[MidiTranslator] UNMAPPED fontId={} instOrWave={} (0x{:02X}) semitone={}",
                        fontId, instOrWave, (uint8_t)instOrWave, semitone);
            sLogCount++;
        }
        if (isFinished || velocity <= 0.0f) {
            retireSlot();
        } else {
            adoptNative();
        }
        return false;
    }

    // If ForceSynth on an unmapped pair, fall back to GM piano so the user
    // hears *something* and can confirm which instrument they're testing.
    if (gm.program == kUnmapped) {
        gm.bank    = 0;
        gm.program = 0;
    }

    // Per-pair GM program override beats the mapping table. Bank defaults
    // to 0 (GM melodic); the PR 8 bank override lets a pack route into
    // arbitrary SF2 banks (Xadra uses 10/15/20/25 for SFX/ambience; 128
    // is GM percussion).
    //
    // Ship 1 (pack pinning): if the pair has preset metadata
    // (pack-bound entry) and AudioEditor's apply path could NOT resolve
    // the pack/bank/program to a loaded preset, the entry is "dead" — we
    // skip the override entirely so native plays. Dead entries are
    // detected by metadata existing but mPinnedSfontId being -1.
    if (BypassIndexValid(fontId, instOrWave)) {
        int16_t ovr = mProgramOverride[fontId][instOrWave];
        if (ovr >= 0 && ovr <= 127) {
            bool packBound  = HasPresetMetadata(fontId, instOrWave);
            bool deadEntry  = packBound && mPinnedSfontId[fontId][instOrWave] < 0;
            if (deadEntry) {
                // Treat as if there were no override — fall through to the
                // mapping-table resolution / native-play path below.
            } else {
                int16_t bankOvr = mBankOverride[fontId][instOrWave];
                gm.bank     = (bankOvr >= 0) ? static_cast<uint8_t>(bankOvr) : 0;
                gm.program  = static_cast<uint8_t>(ovr);
                gm.drumNote = 0;
            }
        }
    }

    // After the dead-entry filter, if gm.program is still kUnmapped and
    // we don't have a ForceSynth bypass, the pair plays native — same
    // as the un-overridden mapping-table path above.
    if (gm.program == kUnmapped && bypass != BypassMode::ForceSynth) {
        if (isFinished || velocity <= 0.0f) retireSlot();
        else                                adoptNative();
        return false;
    }

    const bool isDrum = (gm.bank == 128);

    // Per-pair channel allocation. Each unique (fontId, instOrWave) keeps
    // its own MIDI channel for the session so per-pair effect CCs don't
    // step on each other. FluidSynth flips channel type (melodic / drum)
    // on every ProgramChange so we don't need to reserve a channel for
    // drums up front.
    (void)channelIdx;
    uint8_t targetChannel = AllocateChannelForPair(fontId, instOrWave);

    // Compute the integer MIDI note.
    //   - Melodic: engine semitone + 21 (engine ref C4 = MIDI 60).
    //   - Drums:   prefer gm.drumNote (the GM percussion note the mapping
    //              table assigned to this engine instrument). When 0
    //              (no specific mapping — typical for instOrWave==0 true
    //              drums), distribute the engine semitone across the GM
    //              percussion range so each drum slot at least hits a
    //              different drum sound.
    uint8_t midiNote;
    if (isDrum) {
        if (gm.drumNote != 0) {
            midiNote = gm.drumNote;
        } else {
            int n = static_cast<int>(semitone) + 35; // 35 = GM Acoustic Bass Drum
            midiNote = static_cast<uint8_t>(std::clamp(n, 35, 81));
        }
    } else {
        int transpose = BypassIndexValid(fontId, instOrWave)
                            ? static_cast<int>(mTransposeByPair[fontId][instOrWave])
                            : 0;
        int midiRaw = static_cast<int>(semitone) + kEngineSemitoneToMidiOffset + transpose;
        midiNote = static_cast<uint8_t>(std::clamp(midiRaw, 0, 127));
    }

    uint16_t      preset  = (static_cast<uint16_t>(gm.bank) << 8) | gm.program;
    ChannelState& chState = mChannelState[targetChannel];

    // ── NoteOff ───────────────────────────────────────────────────────────
    if (isFinished || velocity <= 0.0f) {
        retireSlot();
        return true;
    }

    // ── Per-channel CCs (gated against the channel-state cache) ───────────
    // Pack pinning (Ship 1): when the apply path resolved this pair's
    // pack metadata to a specific sfont, route through ProgramSelect
    // so FluidSynth's reverse-load-order priority can't shadow the
    // user's intent. Falls back to ProgramChange when there's no pin
    // (legacy pack-agnostic entries; ForceSynth fallback for unmapped
    // pairs without metadata). The cache compares BOTH the (bank,
    // program) and the pin sfontId so a re-pin to a different SF2
    // sharing the same (bank, program) still triggers the resend.
    int16_t pinSfont = BypassIndexValid(fontId, instOrWave)
                           ? mPinnedSfontId[fontId][instOrWave]
                           : int16_t(-1);
    if (preset != chState.lastPreset || pinSfont != chState.lastPinSfont) {
        if (pinSfont >= 0) {
            bool ok = synth->ProgramSelect(targetChannel, pinSfont, gm.bank, gm.program);
            if (!ok) {
                // The pin failed mid-session — the synth lost the sfont
                // (shouldn't normally happen since the apply path
                // validates against sLoadedPresets). Fall back to native
                // for this NoteOn so we don't play a wrong preset.
                retireSlot();
                adoptNative();
                return false;
            }
        } else {
            synth->ProgramChange(targetChannel, preset);
        }
        chState.lastPreset   = preset;
        chState.lastPinSfont = pinSfont;
    }

    uint8_t cc7val = static_cast<uint8_t>(std::clamp(channelVolume * 127.0f, 0.0f, 127.0f));
    if (cc7val != chState.lastVolumeCC) {
        synth->ControlChange(targetChannel, 7, static_cast<uint16_t>(cc7val) << 7);
        chState.lastVolumeCC = cc7val;
    }

    uint8_t cc10val = static_cast<uint8_t>(pan & 0x7F);
    if (cc10val != chState.lastPanCC) {
        synth->ControlChange(targetChannel, 10, static_cast<uint16_t>(cc10val) << 7);
        chState.lastPanCC = cc10val;
    }

    // PR 7 per-pair effect sends + filter. -1 sentinel resolves to neutral
    // defaults that match FluidSynth's startup CC table (0 for the send
    // levels, 64 for the filter pair) so clearing an override actively
    // restores the synth's "no effect" baseline rather than leaving the
    // last-set value stuck on the channel. Sends are 7-bit, shifted into
    // the 14-bit CC value the IMidiSynth interface takes.
    auto resolveEffectCc = [](int8_t ovr, uint8_t fallback) -> uint8_t {
        return (ovr >= 0) ? static_cast<uint8_t>(ovr) : fallback;
    };
    uint8_t reverbVal = resolveEffectCc(mReverbByPair[fontId][instOrWave], 0);
    uint8_t chorusVal = resolveEffectCc(mChorusByPair[fontId][instOrWave], 0);
    uint8_t cutoffVal = resolveEffectCc(mFilterCutoffByPair[fontId][instOrWave], 64);
    uint8_t qVal      = resolveEffectCc(mFilterQByPair[fontId][instOrWave], 64);
    if (reverbVal != chState.lastReverbCC) {
        synth->ControlChange(targetChannel, 91, static_cast<uint16_t>(reverbVal) << 7);
        chState.lastReverbCC = reverbVal;
    }
    if (chorusVal != chState.lastChorusCC) {
        synth->ControlChange(targetChannel, 93, static_cast<uint16_t>(chorusVal) << 7);
        chState.lastChorusCC = chorusVal;
    }
    if (cutoffVal != chState.lastCutoffCC) {
        synth->ControlChange(targetChannel, 74, static_cast<uint16_t>(cutoffVal) << 7);
        chState.lastCutoffCC = cutoffVal;
    }
    if (qVal != chState.lastQCC) {
        synth->ControlChange(targetChannel, 71, static_cast<uint16_t>(qVal) << 7);
        chState.lastQCC = qVal;
    }

    chState.inited = true;

    // Per-mode loudness routing. Both modes apply a perceptual sqrt(velocity)
    // curve to lift quiet voices into a usable range — engine velocities
    // cluster in the lower half of [0,1] and a raw linear mapping pegs them
    // into the heavily-attenuated zone of either modulator curve. The mode
    // difference is which controller carries dynamics:
    //   Authentic  — NoteOn velocity held at kFixedNoteOnVelocity, sqrt-driven
    //                value routed through CC11; Graham-Smith modulator at
    //                halved attenuation amount processes it.
    //   Enhanced   — sqrt-driven value sent as NoteOn velocity, CC11 parked
    //                at 127 so the SF2's stock concave vel→attenuation
    //                modulator shapes dynamics.
    float gain      = CVarGetFloat(CVAR_AUDIO("FluidSynthGain"), 1.0f);
    float pairGain  = BypassIndexValid(fontId, instOrWave) ? mGainByPair[fontId][instOrWave] : 0.0f;
    if (pairGain == 0.0f) pairGain = 1.0f;
    // Transient session-only volume multiplier from the Override column.
    // Stacks on top of the persisted pairGain so the user can A/B levels
    // without committing them to disk.
    float tempVol = GetTemporaryVolume(fontId, instOrWave);
    float shaped  = std::clamp(sqrtf(std::max(0.0f, velocity)) * gain * pairGain * tempVol,
                               0.0f, 1.0f);

    uint8_t noteOnVel;
    uint8_t cc11val;
    if (mSynthMode == SynthMode::Enhanced) {
        noteOnVel = static_cast<uint8_t>(shaped * 127.0f);
        cc11val   = 127;
    } else {
        noteOnVel = kFixedNoteOnVelocity;
        cc11val   = static_cast<uint8_t>(shaped * 127.0f);
    }
    synth->ControlChange(targetChannel, 11, static_cast<uint16_t>(cc11val) << 7);

    // ── NoteOn ────────────────────────────────────────────────────────────
    // Trigger a fresh NoteOn when the slot is idle / on the native side, the
    // pair has changed, or the integer midiNote has moved (engine reassigned
    // the slot to a different semitone without an explicit NoteOff first).
    if (state.kind != SlotKind::Synth ||
        state.pairFontId != fontId || state.pairInstOrWave != instOrWave ||
        midiNote != state.midiNote) {
        retireSlot();
        synth->PitchBend(targetChannel, 0.0f);
        synth->NoteOn(targetChannel, midiNote, noteOnVel);

        state.kind           = SlotKind::Synth;
        state.channel        = targetChannel;
        state.midiNote       = midiNote;
        state.pairFontId     = fontId;
        state.pairInstOrWave = instOrWave;
        state.baseFreqScale  = (freqScale > 0.0f) ? freqScale : 1.0f;
        state.lastFreqScale  = freqScale;
        if (BypassIndexValid(fontId, instOrWave) &&
            mSynthActiveByPair[fontId][instOrWave] < 255) {
            mSynthActiveByPair[fontId][instOrWave]++;
        }
        return true;
    }

    // ── Continuous pitch update (vibrato / portamento) ────────────────────
    // Skip drums — they don't pitch-bend in GM percussion semantics, and
    // tuned drum samples in OoT are rare enough to defer.
    if (!isDrum && fabsf(freqScale - state.lastFreqScale) > 1e-6f) {
        float ratio = (state.baseFreqScale > 0.0f) ? (freqScale / state.baseFreqScale) : 1.0f;
        if (ratio <= 0.0f) ratio = 1e-6f;
        float bend = 12.0f * log2f(ratio);
        bend = std::clamp(bend, -kMaxPitchBendSemitones, kMaxPitchBendSemitones);
        synth->PitchBend(state.channel, bend);
        state.lastFreqScale = freqScale;
    }
    return true;
}

void MidiTranslator::NoteDisabled(int noteIndex) {
    if (noteIndex < 0 || noteIndex >= kMaxNotes)
        return;
    NoteTranslatorState& state = mNoteState[noteIndex];
    if (state.kind == SlotKind::Synth) {
        auto synth = Ship::MidiSynthManager::Instance().GetActiveSynth();
        if (synth)
            synth->NoteOff(state.channel, state.midiNote);
        if (BypassIndexValid(state.pairFontId, state.pairInstOrWave) &&
            mSynthActiveByPair[state.pairFontId][state.pairInstOrWave] > 0) {
            mSynthActiveByPair[state.pairFontId][state.pairInstOrWave]--;
        }
        state.kind           = SlotKind::Idle;
        state.pairInstOrWave = -1;
    } else if (state.kind == SlotKind::Native) {
        if (BypassIndexValid(state.pairFontId, state.pairInstOrWave) &&
            mNativeActiveByPair[state.pairFontId][state.pairInstOrWave] > 0) {
            mNativeActiveByPair[state.pairFontId][state.pairInstOrWave]--;
        }
        state.kind           = SlotKind::Idle;
        state.pairInstOrWave = -1;
    }
}

} // namespace SOH

// ─── C-linkage entry points called from audio_playback.c ───────────────────

extern "C" {

bool SOH_MidiTranslator_ProcessNote(int noteIndex, float freqScale, float velocity, uint8_t pan, float channelVolume,
                                    uint8_t fontId, int16_t instOrWave, uint8_t semitone, bool isFinished,
                                    uint8_t channelIdx, float resampleRate) {
    return SOH::MidiTranslator::Instance().ProcessNote(noteIndex, freqScale, velocity, pan, channelVolume, fontId,
                                                       instOrWave, semitone, isFinished, channelIdx, resampleRate);
}

void SOH_MidiTranslator_NoteDisabled(int noteIndex) {
    SOH::MidiTranslator::Instance().NoteDisabled(noteIndex);
}

void SOH_MidiTranslator_Reset() {
    SOH::MidiTranslator::Instance().Reset();
}

} // extern "C"
