#include "MidiTranslator.h"
#include "GmInstrumentMap.h"
#include "soh/cvar_prefixes.h"
#include <ship/audio/MidiSynthManager.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#ifdef DEBUG_FONT_MAP
#include <cstdio>
#include <mutex>
static FILE* sDbgFile = nullptr;
static std::mutex sDbgMutex;

static void DbgLogNote(uint8_t fontId, int16_t instOrWave, uint8_t semitone, float freqScale, bool mapped,
                       uint8_t gmBank, uint8_t gmProg) {
    std::lock_guard<std::mutex> lk(sDbgMutex);
    if (!sDbgFile) {
        sDbgFile = fopen("font_map_dump.csv", "w");
        if (sDbgFile)
            fprintf(sDbgFile, "fontId,instOrWave,instOrWave_hex,semitone,expectedMidi,"
                              "freqScale,mapped,gmBank,gmProgram\n");
    }
    if (sDbgFile)
        fprintf(sDbgFile, "%u,%d,0x%02X,%u,%d,%.6f,%s,%u,%u\n", fontId, instOrWave, (uint8_t)instOrWave, semitone,
                static_cast<int>(semitone) + 21, freqScale, mapped ? "yes" : "no", gmBank, gmProg);
}
#define DBG_LOG(fontId, iow, semi, freq, mapped, bank, prog) \
    DbgLogNote((fontId), (iow), (semi), (freq), (mapped), (bank), (prog))
#else
#define DBG_LOG(...) (void)0
#endif

namespace SOH {

// Engine semitone -> MIDI note offset for melodic instruments.
//
// The engine's gNoteFrequencies table labels semitone 39 as "NOTE_C4
// (Middle C)" with freqScale 1.0, which gives the textbook offset of
// 60 - 39 = 21 against standard GM tuning.
static constexpr int kEngineSemitoneToMidiOffset = 21;

// Held near-max so the SF2's default velocity attenuation modulator
// doesn't silence quiet notes. Dynamics ride CC11 instead (Authentic
// mode) or NoteOn velocity (Enhanced mode).
static constexpr uint8_t kFixedNoteOnVelocity = 100;

MidiTranslator::MidiTranslator() {
    // Reserve mEntries capacity so push_back never reallocates. The audio
    // thread reads entry fields by index without locking; a reallocation
    // would invalidate those reads.
    mEntries.reserve(kMaxEntries);
    // -1 = "no active entry" sentinel; native plays for that pair.
    for (auto& row : mActiveEntryIdx)
        for (auto& cell : row)
            cell = -1;
    // 0xFF = unallocated MIDI channel slot.
    for (auto& row : mPairChannel)
        for (auto& cell : row)
            cell = 0xFF;
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

static inline bool BypassIndexValid(uint8_t fontId, int16_t instOrWave) {
    return fontId < MidiTranslator::kMaxFontId && instOrWave >= 0 && instOrWave < MidiTranslator::kMaxInstOrWave;
}

// ── Discovery + active-voice counters ─────────────────────────────────────

int MidiTranslator::DiscoveredSnapshot(DiscoveredPair* out, int outCap) const {
    int n = mDiscoveredCount.load(std::memory_order_acquire);
    if (n > outCap)
        n = outCap;
    for (int i = 0; i < n; i++)
        out[i] = mDiscovered[i];
    return n;
}

void MidiTranslator::ClearDiscovered() {
    mDiscoveredCount.store(0, std::memory_order_release);
    for (auto& word : mSeenBits)
        word = 0;
    for (auto& entry : mDiscovered)
        entry = {};
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

// ── DEBUG: per-pair stats accessors ──────────────────────────────────────

MidiTranslator::DebugPairStats MidiTranslator::GetDebugStats(uint8_t fontId, int16_t instOrWave) const {
    DebugPairStats out{};
    if (!BypassIndexValid(fontId, instOrWave))
        return out;
    const auto& s = mDebugStats[fontId][instOrWave];
    out.noteOns          = s.noteOns.load(std::memory_order_relaxed);
    out.routedSynth      = s.routedSynth.load(std::memory_order_relaxed);
    out.routedNative     = s.routedNative.load(std::memory_order_relaxed);
    out.routedMute       = s.routedMute.load(std::memory_order_relaxed);
    out.lastSemitone     = s.lastSemitone;
    return out;
}

int MidiTranslator::GetDrumSlotHistogram(uint8_t fontId, int16_t instOrWave, uint32_t out[128]) const {
    for (int s = 0; s < kDrumHistSlots; ++s)
        out[s] = 0;
    if (fontId >= kMaxFontId || instOrWave < 0 || instOrWave >= kDrumHistInst)
        return 0;
    int distinct = 0;
    for (int s = 0; s < kDrumHistSlots; ++s) {
        uint32_t c = mDrumSlotHits[fontId][instOrWave][s].count.load(std::memory_order_relaxed);
        out[s] = c;
        if (c > 0)
            ++distinct;
    }
    return distinct;
}

void MidiTranslator::ResetDebugStats() {
    for (int f = 0; f < kMaxFontId; ++f)
        for (int i = 0; i < kMaxInstOrWave; ++i)
            ResetDebugStatsForPair(static_cast<uint8_t>(f), static_cast<int16_t>(i));
}

void MidiTranslator::ResetDebugStatsForPair(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    auto& s = mDebugStats[fontId][instOrWave];
    s.noteOns.store(0, std::memory_order_relaxed);
    s.routedSynth.store(0, std::memory_order_relaxed);
    s.routedNative.store(0, std::memory_order_relaxed);
    s.routedMute.store(0, std::memory_order_relaxed);
    s.lastSemitone     = 0;
    if (instOrWave >= 0 && instOrWave < kDrumHistInst) {
        for (int slot = 0; slot < kDrumHistSlots; ++slot)
            mDrumSlotHits[fontId][instOrWave][slot].count.store(0, std::memory_order_relaxed);
    }
}

// ── Pack stack + sfontId resolution ───────────────────────────────────────

void MidiTranslator::SetPackLoadOrder(const std::vector<std::string>& order) {
    mPackLoadOrder = order;
}

int MidiTranslator::PackRank(const std::string& pack) const {
    // Higher index = later loaded = wins resolution. -1 if not loaded.
    for (size_t i = 0; i < mPackLoadOrder.size(); i++) {
        if (mPackLoadOrder[i] == pack)
            return static_cast<int>(i);
    }
    return -1;
}

int16_t MidiTranslator::ResolveSfontIdFromCache(const std::string& pack, int16_t bank, int16_t program) const {
    if (pack.empty() || program < 0) {
        // Placeholder entries — synthetic sfontId=0 so they resolve and
        // mute via the program<0 branch in ProcessNote.
        return 0;
    }
    for (const auto& lp : mLoadedPresets) {
        if (lp.packName == pack && lp.bank == bank && lp.program == program) {
            return static_cast<int16_t>(lp.sfontId);
        }
    }
    return -1;
}

void MidiTranslator::RefreshEntrySfontIds(const std::vector<LoadedPresetRef>& loadedPresets) {
    // Cache the list so the per-mutation entry creation path can resolve
    // sfontIds without waiting for the next pack-stack pass.
    mLoadedPresets = loadedPresets;
    // O(N*M) walk — both N (entries) and M (loaded presets) stay small in
    // practice (a few hundred each). Build a quick lookup if this ever
    // becomes hot.
    for (auto& e : mEntries) {
        e.sfontId = ResolveSfontIdFromCache(e.packName, e.bank, e.program);
    }
}

void MidiTranslator::RemoveModEntriesNotIn(const std::set<std::string>& packNamesLoaded) {
    // Erase ModSupplied entries whose pack isn't loaded anymore. We also
    // clear any active-cache slots that pointed into them; the caller is
    // expected to follow up with RecomputeAllActive() since indices shift.
    auto newEnd = std::remove_if(mEntries.begin(), mEntries.end(), [&](const ConfigEntry& e) {
        return e.source == EntrySource::ModSupplied && !packNamesLoaded.count(e.packName);
    });
    if (newEnd != mEntries.end()) {
        mEntries.erase(newEnd, mEntries.end());
        // Clear the active cache wholesale — indices may have shifted.
        for (auto& row : mActiveEntryIdx)
            for (auto& cell : row)
                cell = -1;
    }
}

void MidiTranslator::RecomputeAllActive() {
    for (auto& row : mActiveEntryIdx)
        for (auto& cell : row)
            cell = -1;
    // Recompute only pairs that actually have entries — RecomputeActive is
    // O(128 * entries-for-pair), so scanning all 64*256 cells would be
    // wasteful. The distinct-pair set is small (a few hundred at most).
    std::set<std::pair<uint8_t, int16_t>> pairs;
    for (const auto& e : mEntries)
        if (BypassIndexValid(e.fontId, e.instOrWave))
            pairs.insert({ e.fontId, e.instOrWave });
    for (const auto& p : pairs)
        RecomputeActive(p.first, p.second);
}

void MidiTranslator::RecomputeActive(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;

    // Per-semitone winner: the highest-pack-rank enabled+resolvable entry
    // whose [noteLow,noteHigh] covers that engine slot. For the common
    // unsplit pair every semitone resolves to the same single entry (or
    // none), so the chain below collapses to length 1 / head=-1 — bit-
    // identical to the pre-split single-winner behaviour. Disjoint splits
    // (the UI enforces non-overlap) yield one distinct winner per range.
    int winnerAt[128];
    for (int s = 0; s < 128; ++s) {
        int best = -1;
        int bestRank = -2;
        for (size_t idx = 0; idx < mEntries.size(); idx++) {
            const ConfigEntry& e = mEntries[idx];
            if (e.fontId != fontId || e.instOrWave != instOrWave)
                continue;
            if (!e.enabled || e.sfontId < 0)
                continue;
            if (s < e.noteLow || s > e.noteHigh)
                continue;
            int rank = PackRank(e.packName);
            // Strict '>' keeps the first-seen entry on a rank tie, matching
            // the pre-split single-winner resolver bit-for-bit. (Equal rank
            // == same pack == an overlap, which the UI disallows, so this
            // tiebreak only ever fires on the unsplit multi-enabled corner.)
            if (best < 0 || rank > bestRank) {
                best = static_cast<int>(idx);
                bestRank = rank;
            }
        }
        winnerAt[s] = best;
    }

    // Distinct winners in ascending-noteLow order, deduped so each entry is
    // linked at most once (this is what guarantees the chain is acyclic —
    // a cycle would hang the audio-thread walk).
    std::vector<int> winners;
    for (int s = 0; s < 128; ++s) {
        int w = winnerAt[s];
        if (w < 0)
            continue;
        if (std::find(winners.begin(), winners.end(), w) == winners.end())
            winners.push_back(w);
    }
    std::sort(winners.begin(), winners.end(),
              [&](int a, int b) { return mEntries[a].noteLow < mEntries[b].noteLow; });

    // Reset this pair's links, relink the winners, then publish the head
    // LAST so a concurrent audio-thread walk sees either the intact old
    // chain or the fully-built new one (single int16_t store is the publish
    // point; the brief reset->relink window can at worst play one note
    // native, the same benign-transient class as the old single-index flip).
    for (auto& e : mEntries)
        if (e.fontId == fontId && e.instOrWave == instOrWave)
            e.nextActiveSplit = -1;
    for (size_t k = 0; k + 1 < winners.size(); ++k)
        mEntries[winners[k]].nextActiveSplit = static_cast<int16_t>(winners[k + 1]);
    mActiveEntryIdx[fontId][instOrWave] = winners.empty() ? -1 : static_cast<int16_t>(winners[0]);
}

// ── Entry queries ─────────────────────────────────────────────────────────

const ConfigEntry* MidiTranslator::GetActiveEntry(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return nullptr;
    int idx = mActiveEntryIdx[fontId][instOrWave];
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return nullptr;
    return &mEntries[idx];
}
int MidiTranslator::GetActiveEntryIdx(uint8_t fontId, int16_t instOrWave) const {
    if (!BypassIndexValid(fontId, instOrWave))
        return -1;
    return mActiveEntryIdx[fontId][instOrWave];
}

void MidiTranslator::GetEntriesForPair(uint8_t fontId, int16_t instOrWave, std::vector<int>& outIdx) const {
    outIdx.clear();
    for (size_t idx = 0; idx < mEntries.size(); idx++) {
        const ConfigEntry& e = mEntries[idx];
        if (e.fontId == fontId && e.instOrWave == instOrWave) {
            outIdx.push_back(static_cast<int>(idx));
        }
    }
}

int MidiTranslator::FindEntry(uint8_t fontId, int16_t instOrWave, const std::string& pack, int16_t program,
                              uint8_t noteLow) const {
    for (size_t idx = 0; idx < mEntries.size(); idx++) {
        const ConfigEntry& e = mEntries[idx];
        if (e.fontId == fontId && e.instOrWave == instOrWave && e.packName == pack && e.program == program &&
            e.noteLow == noteLow) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

int MidiTranslator::CountSelectedEntries(uint8_t fontId, int16_t instOrWave) const {
    int n = 0;
    for (const auto& e : mEntries) {
        if (e.fontId == fontId && e.instOrWave == instOrWave && e.selected)
            n++;
    }
    return n;
}

int MidiTranslator::FindOrCreateEntry(uint8_t fontId, int16_t instOrWave, const std::string& pack, int16_t program,
                                      int16_t bank, const std::string& presetName, EntrySource source,
                                      uint8_t noteLow) {
    int idx = FindEntry(fontId, instOrWave, pack, program, noteLow);
    if (idx >= 0)
        return idx;
    if (mEntries.size() >= kMaxEntries) {
        SPDLOG_WARN("[MidiTranslator] Entry pool full ({}); dropping new entry "
                    "for f={}, i={}, pack='{}', program={}",
                    kMaxEntries, fontId, instOrWave, pack, program);
        return -1;
    }
    ConfigEntry e;
    e.fontId = fontId;
    e.instOrWave = instOrWave;
    e.packName = pack;
    e.program = program;
    e.bank = bank;
    e.presetName = presetName;
    e.source = source;
    e.noteLow = noteLow; // noteHigh stays 127 (full range) until a split sets it
    // Resolve sfontId immediately so the entry is eligible for
    // resolution on the next note — without this, a fresh PickPreset
    // creates an entry with sfontId=-1 and the resolution filter
    // (enabled && sfontId>=0) skips it. ResolveSfontIdFromCache returns
    // 0 for placeholder entries so they participate in resolution and
    // produce the silent NoteOn the user asked for.
    e.sfontId = ResolveSfontIdFromCache(pack, bank, program);
    mEntries.push_back(std::move(e));
    return static_cast<int>(mEntries.size()) - 1;
}

// ── UI row actions ────────────────────────────────────────────────────────

void MidiTranslator::PickPreset(uint8_t fontId, int16_t instOrWave, const std::string& pack, int16_t program,
                                int16_t bank, const std::string& presetName) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;

    // Capture the previous winner's pack so we know whether to clear its
    // selected flag (same SoundFont as the new pick → user moved within
    // the pack and the old entry is no longer "the one for this pack").
    std::string prevWinnerPack;
    int prevWinnerIdx = mActiveEntryIdx[fontId][instOrWave];
    if (prevWinnerIdx >= 0 && prevWinnerIdx < static_cast<int>(mEntries.size())) {
        prevWinnerPack = mEntries[prevWinnerIdx].packName;
    }

    // Disable every currently-enabled entry for this pair.
    for (auto& e : mEntries) {
        if (e.fontId == fontId && e.instOrWave == instOrWave && e.enabled) {
            e.enabled = false;
        }
    }

    // Same-SoundFont rule: if the previous winner was from the new
    // pick's pack, drop its selected flag (the user replaced it in-place).
    if (!prevWinnerPack.empty() && prevWinnerPack == pack && prevWinnerIdx >= 0) {
        ConfigEntry& prev = mEntries[prevWinnerIdx];
        if (prev.program != program) {
            prev.selected = false;
        }
    }

    int idx = FindOrCreateEntry(fontId, instOrWave, pack, program, bank, presetName, EntrySource::UserPicked);
    if (idx < 0)
        return;
    ConfigEntry& e = mEntries[idx];
    // Preserve gain/transpose/effects when reusing — only program/bank/
    // pack identify the entry. presetName updates so renamed presets
    // surface correctly. Re-resolve sfontId in case the bank changed on
    // a reused entry (FindEntry keys on pack+program so the existing
    // entry's bank may differ from the new pick's).
    e.bank = bank;
    e.presetName = presetName;
    e.sfontId = ResolveSfontIdFromCache(pack, bank, program);
    e.enabled = true;
    e.selected = true;
    e.lastEnabledSeq = mNextSeq++;
    // source stays whatever it was (might be ModSupplied if the user
    // clicked a mod-shipped entry; that's fine — picking promotes it to
    // a selected entry which will be persisted).
    if (e.source == EntrySource::ModSupplied) {
        // The act of picking promotes a mod entry to user-owned so we
        // persist customisations the user makes on top of it.
        e.source = EntrySource::UserPicked;
    }
    RecomputeActive(fontId, instOrWave);
}

void MidiTranslator::ClickNative(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    for (auto& e : mEntries) {
        if (e.fontId == fontId && e.instOrWave == instOrWave) {
            e.enabled = false;
        }
    }
    RecomputeActive(fontId, instOrWave);
}

void MidiTranslator::ClickSynth(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;

    // Primary search: user-selected entries.
    int bestIdx = -1;
    uint32_t bestSeq = 0;
    for (size_t idx = 0; idx < mEntries.size(); idx++) {
        const ConfigEntry& e = mEntries[idx];
        if (e.fontId != fontId || e.instOrWave != instOrWave)
            continue;
        if (!e.selected)
            continue;
        if (bestIdx < 0 || e.lastEnabledSeq >= bestSeq) {
            bestIdx = static_cast<int>(idx);
            bestSeq = e.lastEnabledSeq;
        }
    }
    // Fallback (option B): any disabled-but-resolvable entry for this
    // pair. Covers the "mod ships a preset, user never picked, clicked
    // Native, now wants it back" case.
    if (bestIdx < 0) {
        for (size_t idx = 0; idx < mEntries.size(); idx++) {
            const ConfigEntry& e = mEntries[idx];
            if (e.fontId != fontId || e.instOrWave != instOrWave)
                continue;
            if (e.sfontId < 0)
                continue;
            if (bestIdx < 0 || e.lastEnabledSeq >= bestSeq) {
                bestIdx = static_cast<int>(idx);
                bestSeq = e.lastEnabledSeq;
            }
        }
    }
    if (bestIdx >= 0) {
        ConfigEntry& e = mEntries[bestIdx];
        e.enabled = true;
        e.lastEnabledSeq = mNextSeq++;
        RecomputeActive(fontId, instOrWave);
        return;
    }
    // Last resort: muted placeholder entry. packName=="" tells the resolver
    // / UI that this is a synthetic "None" pick.
    int idx = FindOrCreateEntry(fontId, instOrWave, std::string(), -1, 0, std::string("None"), EntrySource::UserPicked);
    if (idx < 0)
        return;
    ConfigEntry& e = mEntries[idx];
    e.enabled = true;
    e.selected = true;
    e.lastEnabledSeq = mNextSeq++;
    // Placeholder entries get sfontId=0 in RefreshEntrySfontIds so they
    // participate in resolution; tag here as well in case Refresh hasn't
    // run since the create.
    e.sfontId = 0;
    RecomputeActive(fontId, instOrWave);
}

// ── Per-entry mutators ────────────────────────────────────────────────────

void MidiTranslator::SetEntryGain(int idx, float gain) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].gain = gain;
}
void MidiTranslator::SetEntryTranspose(int idx, int8_t semis) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].transpose = semis;
}
static inline int8_t ClampCcOrSentinel(int v) {
    if (v < 0)
        return -1;
    if (v > 127)
        return 127;
    return static_cast<int8_t>(v);
}
void MidiTranslator::SetEntryReverb(int idx, int8_t cc) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].reverb = ClampCcOrSentinel(cc);
}
void MidiTranslator::SetEntryChorus(int idx, int8_t cc) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].chorus = ClampCcOrSentinel(cc);
}
void MidiTranslator::SetEntryFilterCutoff(int idx, int8_t cc) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].cutoff = ClampCcOrSentinel(cc);
}
void MidiTranslator::SetEntryFilterResonance(int idx, int8_t cc) {
    if (idx < 0 || idx >= static_cast<int>(mEntries.size()))
        return;
    mEntries[idx].q = ClampCcOrSentinel(cc);
}
// ── Per-pair display name (row label) ─────────────────────────────────────

std::string MidiTranslator::GetDisplayName(uint8_t fontId, int16_t instOrWave) const {
    auto it = mDisplayName.find({ fontId, instOrWave });
    return it == mDisplayName.end() ? std::string{} : it->second;
}
void MidiTranslator::SetDisplayName(uint8_t fontId, int16_t instOrWave, const std::string& name) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    if (name.empty())
        mDisplayName.erase({ fontId, instOrWave });
    else
        mDisplayName[{ fontId, instOrWave }] = name;
}

// ── Session-only transient state ──────────────────────────────────────────

bool MidiTranslator::IsTemporarilyMuted(uint8_t fontId, int16_t instOrWave) const {
    return mTemporaryMute.count({ fontId, instOrWave }) > 0;
}
void MidiTranslator::SetTemporaryMute(uint8_t fontId, int16_t instOrWave, bool muted) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    if (muted)
        mTemporaryMute.insert({ fontId, instOrWave });
    else
        mTemporaryMute.erase({ fontId, instOrWave });
}
void MidiTranslator::ClearAllTemporaryMutes() {
    mTemporaryMute.clear();
}

float MidiTranslator::GetTemporaryVolume(uint8_t fontId, int16_t instOrWave) const {
    auto it = mTemporaryVolume.find({ fontId, instOrWave });
    return it == mTemporaryVolume.end() ? 1.0f : it->second;
}
void MidiTranslator::SetTemporaryVolume(uint8_t fontId, int16_t instOrWave, float vol) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    if (vol == 1.0f) {
        mTemporaryVolume.erase({ fontId, instOrWave });
    } else {
        if (vol < 0.0f)
            vol = 0.0f;
        if (vol > 4.0f)
            vol = 4.0f;
        mTemporaryVolume[{ fontId, instOrWave }] = vol;
    }
}
void MidiTranslator::ClearAllTemporaryVolumes() {
    mTemporaryVolume.clear();
}

// ── MIDI channel pool ─────────────────────────────────────────────────────

uint8_t MidiTranslator::GetChannelsInUse() const {
    return mChannelsAllocated;
}

uint32_t MidiTranslator::GetChannelReclaims() const {
    return mChannelReclaims;
}

uint8_t MidiTranslator::ReclaimIdleChannel(uint8_t exceptFontId, int16_t exceptInst) {
    for (uint8_t ch = 0; ch < mChannelsAllocated; ++ch) {
        ChannelOwner& o = mChannelOwner[ch];
        if (o.instOrWave < 0)
            continue; // unowned (shouldn't happen without eager release)
        if (o.fontId == exceptFontId && o.instOrWave == exceptInst)
            continue; // never evict the pair asking for a channel
        if (mSynthActiveByPair[o.fontId][o.instOrWave] != 0)
            continue; // still sounding -- leave it alone
        // Idle pair: drop its claim. The physical channel keeps its program
        // and CC state, and so does mChannelState[ch], so the new owner's
        // ProcessNote re-issues only what differs. No explicit reset needed.
        mPairChannel[o.fontId][o.instOrWave] = 0xFF;
        o.instOrWave = -1;
        return ch;
    }
    return 0xFF;
}

// 0xFF return = no channel available; caller routes the note to native.
uint8_t MidiTranslator::AllocateChannelForPair(uint8_t fontId, int16_t instOrWave) {
    if (!BypassIndexValid(fontId, instOrWave))
        return 0xFF;
    uint8_t& slot = mPairChannel[fontId][instOrWave];
    if (slot != 0xFF)
        return slot;

    // Room to grow the pool: hand out the next channel.
    if (mChannelsAllocated < kMaxMidiChannels) {
        slot = mChannelsAllocated++;
        mChannelOwner[slot] = { fontId, instOrWave };
        return slot;
    }

    // Pool full: reclaim a channel from a pair that has gone quiet (a prior
    // song's instruments). Active pairs keep theirs, so nothing sounding is
    // cut. This is what stops the long-session "instruments stop playing"
    // collapse -- new pairs get a real channel instead of sharing ch 0.
    uint8_t reclaimed = ReclaimIdleChannel(fontId, instOrWave);
    if (reclaimed != 0xFF) {
        mChannelOwner[reclaimed] = { fontId, instOrWave };
        ++mChannelReclaims;
        slot = reclaimed;
        return slot;
    }

    // Genuinely exhausted: all 64 channels are sounding distinct pairs right
    // now. Collapsing onto channel 0 would hijack whatever instrument owns it
    // and play this note with the wrong program, so signal failure instead and
    // let the caller fall back to native. mPairChannel stays unassigned, so a
    // later note retries once any pair goes quiet.
    static bool sLoggedOverflow = false;
    if (!sLoggedOverflow) {
        SPDLOG_WARN("[MidiTranslator] All {} channels sounding at once; "
                    "routing overflow pair to native",
                    (int)kMaxMidiChannels);
        sLoggedOverflow = true;
    }
    return 0xFF;
}

void MidiTranslator::RecordDiscovery(uint8_t fontId, int16_t instOrWave, bool mapped) {
    if (!BypassIndexValid(fontId, instOrWave))
        return;
    const size_t bitIdx = static_cast<size_t>(fontId) * kMaxInstOrWave + instOrWave;
    const size_t wordIdx = bitIdx / 64;
    const uint64_t mask = uint64_t(1) << (bitIdx & 63);
    if (mSeenBits[wordIdx] & mask)
        return;
    mSeenBits[wordIdx] |= mask;

    int slot = mDiscoveredCount.load(std::memory_order_relaxed);
    if (slot >= kMaxDiscovered)
        return;
    mDiscovered[slot] = { fontId, instOrWave, mapped };
    mDiscoveredCount.store(slot + 1, std::memory_order_release);
}

// ── ResetAllOverrides + Persistence ───────────────────────────────────────

void MidiTranslator::ResetAllOverrides() {
    mEntries.clear();
    for (auto& row : mActiveEntryIdx)
        for (auto& cell : row)
            cell = -1;
    mDisplayName.clear();
    // Channel allocation, discovery bits, and active-voice counters
    // intentionally survive — they're runtime state, not overrides.
}

// Route <-> JSON string. Synth is the default and is omitted on write.
static const char* RouteToString(EntryRoute r) {
    switch (r) {
        case EntryRoute::Native: return "native";
        case EntryRoute::Mute:   return "mute";
        case EntryRoute::Synth:  break;
    }
    return "synth";
}
static EntryRoute RouteFromString(const std::string& s) {
    if (s == "native") return EntryRoute::Native;
    if (s == "mute")   return EntryRoute::Mute;
    return EntryRoute::Synth;
}

// Append the note-range split fields to a serialised entry, each omitted at
// its default so unsplit entries stay byte-equivalent to the pre-split schema
// (v2 -> v2, additive). Shared by SaveOverridesToFile and ExportPackMapping
// so the two writers can't drift. (noteLow is the only split field that is
// part of the entry key; it is written unconditionally-when-nonzero here.)
static void WriteSplitFields(nlohmann::json& entry, const ConfigEntry& e) {
    if (e.noteLow != 0)
        entry["note_low"] = e.noteLow;
    if (e.noteHigh != 127)
        entry["note_high"] = e.noteHigh;
    if (e.fixedNote >= 0)
        entry["fixed_note"] = e.fixedNote;
    if (e.route != EntryRoute::Synth)
        entry["route"] = RouteToString(e.route);
}

// Read the non-key split fields (noteHigh / fixedNote / route) onto an entry.
// Missing keys keep the entry's current values, matching how gain/transpose/
// effects overlay. noteLow is applied separately via the FindOrCreateEntry
// key so distinct splits resolve to distinct entries.
static void ReadSplitFields(const nlohmann::json& entry, ConfigEntry& e) {
    if (entry.contains("note_high")) {
        int nh = entry.value("note_high", 127);
        e.noteHigh = static_cast<uint8_t>(std::clamp(nh, 0, 127));
    }
    if (entry.contains("fixed_note")) {
        int fn = entry.value("fixed_note", -1);
        e.fixedNote = static_cast<int16_t>(std::clamp(fn, -1, 127));
    }
    if (entry.contains("route"))
        e.route = RouteFromString(entry.value("route", std::string("synth")));
}

bool MidiTranslator::SaveOverridesToFile(const std::string& path) const {
    nlohmann::json j;
    j["version"] = 2;
    j["entries"] = nlohmann::json::array();

    // Save criteria: an entry is persisted when the user has expressed
    // intent in it (selected=true) OR the pair has a display name
    // override. Mod-shipped entries the user never touched are NOT
    // persisted — they reload from each pack's mapping.json next session.
    for (const auto& e : mEntries) {
        bool hasDisplayName = false;
        auto dispIt = mDisplayName.find({ e.fontId, e.instOrWave });
        if (dispIt != mDisplayName.end() && !dispIt->second.empty())
            hasDisplayName = true;

        if (!e.selected && !hasDisplayName)
            continue;
        if (e.source != EntrySource::UserPicked)
            continue; // safety

        nlohmann::json entry;
        entry["fontId"] = e.fontId;
        entry["instOrWave"] = e.instOrWave;
        entry["pack"] = e.packName;
        entry["program"] = e.program;
        entry["bank"] = e.bank;
        if (!e.presetName.empty())
            entry["preset_name"] = e.presetName;
        if (e.gain != 0.0f)
            entry["gain"] = e.gain;
        if (e.transpose != 0)
            entry["transpose"] = e.transpose;
        if (e.reverb >= 0)
            entry["reverb"] = e.reverb;
        if (e.chorus >= 0)
            entry["chorus"] = e.chorus;
        if (e.cutoff >= 0)
            entry["filter_cutoff"] = e.cutoff;
        if (e.q >= 0)
            entry["filter_q"] = e.q;
        WriteSplitFields(entry, e);
        entry["enabled"] = e.enabled;
        entry["selected"] = e.selected;
        if (hasDisplayName)
            entry["display_name"] = dispIt->second;
        j["entries"].push_back(std::move(entry));
    }

    // Display-name-only pairs (no matching selected entry) still need a
    // home in the JSON so the label survives a restart. Write a stub
    // entry with display_name set but no pack/program/etc.
    for (const auto& kv : mDisplayName) {
        if (kv.second.empty())
            continue;
        bool alreadyEmitted = false;
        for (const auto& e : mEntries) {
            if (e.fontId == kv.first.first && e.instOrWave == kv.first.second && e.selected &&
                e.source == EntrySource::UserPicked) {
                alreadyEmitted = true;
                break;
            }
        }
        if (alreadyEmitted)
            continue;
        nlohmann::json entry;
        entry["fontId"] = kv.first.first;
        entry["instOrWave"] = kv.first.second;
        entry["display_name"] = kv.second;
        j["entries"].push_back(std::move(entry));
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        SPDLOG_WARN("[MidiTranslator] SaveOverridesToFile: cannot open {}", path);
        return false;
    }
    out << j.dump(2);
    return out.good();
}

// Shared predicate for "ship this entry inside a pack mapping?". Same gate
// used by ExportPackMapping and CountExportableEntries so the previewed
// count always matches what gets written.
static bool ExportEntryMatches(const ConfigEntry& e, const std::string& packNameFilter) {
    if (!e.enabled || !e.selected) return false;
    if (e.program < 0) return false;          // None placeholder — not shippable
    if (e.packName.empty()) return false;
    if (!packNameFilter.empty() && e.packName != packNameFilter) return false;
    return true;
}

int MidiTranslator::CountExportableEntries(const std::string& packNameFilter) const {
    int n = 0;
    for (const auto& e : mEntries)
        if (ExportEntryMatches(e, packNameFilter))
            ++n;
    return n;
}

int MidiTranslator::ExportPackMapping(const std::string& path, const std::string& packNameFilter) const {
    nlohmann::json j;
    j["version"] = 2;
    if (!packNameFilter.empty())
        j["pack_name"] = packNameFilter;
    j["entries"] = nlohmann::json::array();

    int written = 0;
    for (const auto& e : mEntries) {
        if (!ExportEntryMatches(e, packNameFilter))
            continue;

        nlohmann::json entry;
        entry["fontId"] = e.fontId;
        entry["instOrWave"] = e.instOrWave;
        entry["pack"] = e.packName;
        entry["program"] = e.program;
        entry["bank"] = e.bank;
        if (!e.presetName.empty())
            entry["preset_name"] = e.presetName;
        if (e.gain != 0.0f)
            entry["gain"] = e.gain;
        if (e.transpose != 0)
            entry["transpose"] = e.transpose;
        if (e.reverb >= 0)
            entry["reverb"] = e.reverb;
        if (e.chorus >= 0)
            entry["chorus"] = e.chorus;
        if (e.cutoff >= 0)
            entry["filter_cutoff"] = e.cutoff;
        if (e.q >= 0)
            entry["filter_q"] = e.q;
        WriteSplitFields(entry, e);
        // Pack mapping consumers default enabled=true, selected=false at load
        // time (ApplyOverridesFromString), so we omit both flags here — the
        // file represents "this is the pack's recommended preset for the
        // pair", not "this is currently picked by the user".
        auto dispIt = mDisplayName.find({ e.fontId, e.instOrWave });
        if (dispIt != mDisplayName.end() && !dispIt->second.empty())
            entry["display_name"] = dispIt->second;
        j["entries"].push_back(std::move(entry));
        ++written;
    }

    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        SPDLOG_WARN("[MidiTranslator] ExportPackMapping: cannot open {}", path);
        return -1;
    }
    out << j.dump(2);
    if (!out.good()) {
        SPDLOG_WARN("[MidiTranslator] ExportPackMapping: write failed for {}", path);
        return -1;
    }
    SPDLOG_INFO("[MidiTranslator] ExportPackMapping: wrote {} entries to {}", written, path);
    return written;
}

bool MidiTranslator::ApplyOverridesFromString(const std::string& json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[MidiTranslator] ApplyOverridesFromString: parse error: {}", e.what());
        return false;
    }
    auto entries = j.value("entries", nlohmann::json::array());
    int applied = 0;
    for (const auto& entry : entries) {
        int fontId = entry.value("fontId", -1);
        int instOrWave = entry.value("instOrWave", -1);
        if (fontId < 0 || fontId >= kMaxFontId || instOrWave < 0 || instOrWave >= kMaxInstOrWave)
            continue;

        if (entry.contains("display_name")) {
            std::string dn = entry.value("display_name", std::string{});
            SetDisplayName(static_cast<uint8_t>(fontId), static_cast<int16_t>(instOrWave), dn);
        }
        if (!entry.contains("pack"))
            continue;
        std::string pack = entry.value("pack", std::string{});
        int program = entry.value("program", -1);
        int bank = entry.value("bank", 0);
        if (program < -1 || program > 127)
            continue;
        if (bank < 0)
            bank = 0;
        if (bank > 255)
            bank = 255;
        std::string presetName = entry.value("preset_name", std::string{});
        // note_low is part of the entry key, so it must be resolved before
        // find-or-create or distinct splits collapse into one entry.
        uint8_t noteLow = static_cast<uint8_t>(std::clamp(entry.value("note_low", 0), 0, 127));

        int idx = FindOrCreateEntry(static_cast<uint8_t>(fontId), static_cast<int16_t>(instOrWave), pack,
                                    static_cast<int16_t>(program), static_cast<int16_t>(bank), presetName,
                                    EntrySource::ModSupplied, noteLow);
        if (idx < 0)
            continue;
        ConfigEntry& e = mEntries[idx];
        // Overlay fields. Missing keys keep current values.
        e.bank = static_cast<int16_t>(bank);
        if (!presetName.empty())
            e.presetName = presetName;
        if (entry.contains("gain"))
            e.gain = entry.value("gain", 0.0f);
        if (entry.contains("transpose")) {
            int t = entry.value("transpose", 0);
            t = std::clamp(t, -127, 127);
            e.transpose = static_cast<int8_t>(t);
        }
        if (entry.contains("reverb"))
            e.reverb = ClampCcOrSentinel(entry.value("reverb", -1));
        if (entry.contains("chorus"))
            e.chorus = ClampCcOrSentinel(entry.value("chorus", -1));
        if (entry.contains("filter_cutoff"))
            e.cutoff = ClampCcOrSentinel(entry.value("filter_cutoff", -1));
        if (entry.contains("filter_q"))
            e.q = ClampCcOrSentinel(entry.value("filter_q", -1));
        ReadSplitFields(entry, e);
        // Pack mapping.json: enabled by default (mods publish active
        // presets), selected stays false (user hasn't picked).
        e.enabled = entry.value("enabled", true);
        e.selected = entry.value("selected", false);
        applied++;
    }
    SPDLOG_INFO("[MidiTranslator] ApplyOverridesFromString: applied {} entries", applied);
    return true;
}

bool MidiTranslator::ApplyOverridesFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
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
    int version = j.value("version", 1);
    auto entries = j.value("entries", nlohmann::json::array());
    int applied = 0;
    for (const auto& entry : entries) {
        int fontId = entry.value("fontId", -1);
        int instOrWave = entry.value("instOrWave", -1);
        if (fontId < 0 || fontId >= kMaxFontId || instOrWave < 0 || instOrWave >= kMaxInstOrWave)
            continue;

        if (entry.contains("display_name")) {
            std::string dn = entry.value("display_name", std::string{});
            SetDisplayName(static_cast<uint8_t>(fontId), static_cast<int16_t>(instOrWave), dn);
        }
        if (!entry.contains("pack"))
            continue;
        std::string pack = entry.value("pack", std::string{});
        int program = entry.value("program", -1);
        int bank = entry.value("bank", 0);
        if (program < -1 || program > 127)
            continue;
        if (bank < 0)
            bank = 0;
        if (bank > 255)
            bank = 255;
        std::string presetName = entry.value("preset_name", std::string{});
        // note_low is part of the entry key, so it must be resolved before
        // find-or-create or distinct splits collapse into one entry.
        uint8_t noteLow = static_cast<uint8_t>(std::clamp(entry.value("note_low", 0), 0, 127));

        int idx = FindOrCreateEntry(static_cast<uint8_t>(fontId), static_cast<int16_t>(instOrWave), pack,
                                    static_cast<int16_t>(program), static_cast<int16_t>(bank), presetName,
                                    EntrySource::UserPicked, noteLow);
        if (idx < 0)
            continue;
        ConfigEntry& e = mEntries[idx];
        e.bank = static_cast<int16_t>(bank);
        if (!presetName.empty())
            e.presetName = presetName;
        if (entry.contains("gain"))
            e.gain = entry.value("gain", 0.0f);
        if (entry.contains("transpose")) {
            int t = entry.value("transpose", 0);
            t = std::clamp(t, -127, 127);
            e.transpose = static_cast<int8_t>(t);
        }
        if (entry.contains("reverb"))
            e.reverb = ClampCcOrSentinel(entry.value("reverb", -1));
        if (entry.contains("chorus"))
            e.chorus = ClampCcOrSentinel(entry.value("chorus", -1));
        if (entry.contains("filter_cutoff"))
            e.cutoff = ClampCcOrSentinel(entry.value("filter_cutoff", -1));
        if (entry.contains("filter_q"))
            e.q = ClampCcOrSentinel(entry.value("filter_q", -1));
        ReadSplitFields(entry, e);
        // File overlay = user file. Defaults align with "this is a user
        // pick" — enabled+selected unless the file says otherwise. Old
        // (v1) files don't carry these keys, so the defaults give the
        // expected migration behaviour: every old entry comes back as
        // an enabled, selected user pick.
        e.enabled = entry.value("enabled", true);
        e.selected = entry.value("selected", true);
        // Promote to UserPicked even if a mod entry was created earlier
        // in the chain with the same key — the user file is the source
        // of truth for source attribution.
        e.source = EntrySource::UserPicked;
        applied++;
    }
    SPDLOG_INFO("[MidiTranslator] ApplyOverridesFromFile: applied {} entries from {} (v{})", applied, path, version);
    return true;
}

// ── ProcessNote ───────────────────────────────────────────────────────────

bool MidiTranslator::ProcessNote(int noteIndex, float freqScale, float velocity, uint8_t pan, float channelVolume,
                                 uint8_t fontId, int16_t instOrWave, uint8_t semitone, bool isFinished,
                                 uint8_t channelIdx, float resampleRate, float pitchBend) {
    (void)resampleRate;
    (void)channelIdx;

    if (noteIndex < 0 || noteIndex >= kMaxNotes)
        return false;

    auto synth = Ship::MidiSynthManager::Instance().GetActiveSynth();
    if (!synth)
        return false;

    NoteTranslatorState& state = mNoteState[noteIndex];

    // Discovery uses GetGmPreset purely as a "did the legacy mapping
    // table know about this pair?" hint for the UI rows. Resolution
    // itself runs off mActiveEntryIdx now.
    GmPreset legacyGm = GetGmPreset(fontId, instOrWave);
    RecordDiscovery(fontId, instOrWave, legacyGm.program != kUnmapped);

    // DEBUG: per-pair stats. Detect a fresh NoteOn (Idle → playing transition);
    // record the engine semitone. Route counters are incremented at the
    // dispatch decision below — guarded by `wasIdleStart` so continuation
    // frames of the same note don't double-count.
    const bool wasIdleStart = BypassIndexValid(fontId, instOrWave) &&
                              state.kind == SlotKind::Idle &&
                              velocity > 0.0f && !isFinished;
    if (wasIdleStart) {
        auto& dbg = mDebugStats[fontId][instOrWave];
        dbg.noteOns.fetch_add(1, std::memory_order_relaxed);
        dbg.lastSemitone = semitone;
        // Drum/SFX slot histogram: on these channels `semitone` is a slot
        // index, so this is the per-slot fire count the drum auto-split reads.
        if (instOrWave < kDrumHistInst && semitone < kDrumHistSlots) {
            mDrumSlotHits[fontId][instOrWave][semitone].count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Helper for the route-decision counters below. Only counts on a fresh
    // NoteOn so per-frame continuation calls don't inflate the totals.
    auto bumpRoute = [&](std::atomic<uint32_t> DebugSlot::*counter) {
        if (wasIdleStart && BypassIndexValid(fontId, instOrWave))
            (mDebugStats[fontId][instOrWave].*counter).fetch_add(1, std::memory_order_relaxed);
    };
    DBG_LOG(fontId, instOrWave, semitone, freqScale, legacyGm.program != kUnmapped, legacyGm.bank, legacyGm.program);

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

    auto adoptNative = [&]() {
        if (state.kind != SlotKind::Native || state.pairFontId != fontId || state.pairInstOrWave != instOrWave) {
            retireSlot();
            state.kind = SlotKind::Native;
            state.pairFontId = fontId;
            state.pairInstOrWave = instOrWave;
            if (BypassIndexValid(fontId, instOrWave) && mNativeActiveByPair[fontId][instOrWave] < 255) {
                mNativeActiveByPair[fontId][instOrWave]++;
            }
        }
    };

    // Transient mute (Solo button) silences BOTH paths.
    if (mTemporaryMute.count({ fontId, instOrWave })) {
        retireSlot();
        return true;
    }

    // Engine drum (instOrWave==0) and SFX (instOrWave==1): the `semitone`
    // byte is a slot index (Audio_GetDrum / Audio_GetSfx), not a chromatic
    // pitch. These flow through the same split resolution as melodic pairs:
    // with no active entry the chain resolves to native exactly as before,
    // but an authored per-slot split can claim a slot. The bank-128 guard
    // below still routes percussion entries to native until the drum-split
    // playback path (plan 4.5 phase 3) replaces it, so behaviour for current
    // mappings is unchanged.

    // ── Resolution: walk the active-split chain ─────────────────────────
    if (!BypassIndexValid(fontId, instOrWave)) {
        if (isFinished || velocity <= 0.0f)
            retireSlot();
        else
            adoptNative();
        // bumpRoute is a no-op outside the bypass range — fine.
        return false;
    }
    // Find the entry whose engine-semitone range covers this note. The chain
    // is sorted by noteLow and acyclic (RecomputeActive links each entry at
    // most once); an unsplit pair has a length-1 chain so this is the same
    // single lookup as before.
    int activeIdx = mActiveEntryIdx[fontId][instOrWave];
    while (activeIdx >= 0 && activeIdx < static_cast<int>(mEntries.size())) {
        const ConfigEntry& cand = mEntries[activeIdx];
        if (semitone >= cand.noteLow && semitone <= cand.noteHigh)
            break;
        activeIdx = cand.nextActiveSplit;
    }
    if (activeIdx < 0 || activeIdx >= static_cast<int>(mEntries.size())) {
        // No enabled entry covers this slot → native plays.
        if (isFinished || velocity <= 0.0f)
            retireSlot();
        else
            adoptNative();
        bumpRoute(&DebugSlot::routedNative);
        return false;
    }
    const ConfigEntry& e = mEntries[activeIdx];

    // Native-route split: this entry won its range but the audible path is
    // the engine sample (a range the author wants to keep native while a
    // sibling range synths). First-class winner, not the "no entry" fall-
    // through, so a wider synth split can't shadow it.
    if (e.route == EntryRoute::Native) {
        if (isFinished || velocity <= 0.0f)
            retireSlot();
        else
            adoptNative();
        bumpRoute(&DebugSlot::routedNative);
        return false;
    }

    // Placeholder ("None") / Mute entry: explicit user intent to mute the
    // synth path while not letting native sneak in. Same return value as the
    // Solo-button mute — true tells the C hook to suppress the engine.
    if (e.route == EntryRoute::Mute || e.program < 0) {
        retireSlot();
        bumpRoute(&DebugSlot::routedMute);
        return true;
    }

    const uint8_t gmBank = static_cast<uint8_t>(e.bank);
    const uint8_t gmProgram = static_cast<uint8_t>(e.program);
    const int16_t pinSfont = e.sfontId;

    // GM percussion (bank 128) needs note-range splits to map each engine
    // drum slot to its intended GM percussion note; until that lands the
    // engine-semitone-to-GM-note heuristic produced nothing usable. Route
    // to native and keep the authored entry around for the split-aware path.
    if (gmBank == 128) {
        if (isFinished || velocity <= 0.0f)
            retireSlot();
        else
            adoptNative();
        bumpRoute(&DebugSlot::routedNative);
        return false;
    }

    // Per-pair channel allocation — same channel for the life of a pair
    // so per-pair effect CCs survive across notes.
    uint8_t targetChannel = AllocateChannelForPair(fontId, instOrWave);
    if (targetChannel == 0xFF) {
        // Channel pool momentarily exhausted (all 64 sounding) -> native, so
        // the instrument still plays instead of corrupting another channel.
        if (isFinished || velocity <= 0.0f)
            retireSlot();
        else
            adoptNative();
        bumpRoute(&DebugSlot::routedNative);
        return false;
    }

    // Integer MIDI note from the engine semitone, with optional transpose.
    int transpose = static_cast<int>(e.transpose);
    int midiRaw = static_cast<int>(semitone) + kEngineSemitoneToMidiOffset + transpose;
    uint8_t midiNote = static_cast<uint8_t>(std::clamp(midiRaw, 0, 127));

    uint16_t preset = (static_cast<uint16_t>(gmBank) << 8) | gmProgram;
    ChannelState& chState = mChannelState[targetChannel];

    if (isFinished || velocity <= 0.0f) {
        retireSlot();
        return true;
    }

    // ── ProgramSelect (pinned to entry's sfontId) ────────────────────────
    if (preset != chState.lastPreset || pinSfont != chState.lastPinSfont) {
        bool ok = synth->ProgramSelect(targetChannel, pinSfont, gmBank, gmProgram);
        if (!ok) {
            // Pin failed mid-session — fall back to native for this note.
            retireSlot();
            adoptNative();
            bumpRoute(&DebugSlot::routedNative);
            return false;
        }
        chState.lastPreset = preset;
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

    auto resolveEffectCc = [](int8_t ovr, uint8_t fallback) -> uint8_t {
        return (ovr >= 0) ? static_cast<uint8_t>(ovr) : fallback;
    };
    uint8_t reverbVal = resolveEffectCc(e.reverb, 0);
    uint8_t chorusVal = resolveEffectCc(e.chorus, 0);
    uint8_t cutoffVal = resolveEffectCc(e.cutoff, 64);
    uint8_t qVal = resolveEffectCc(e.q, 64);
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

    // ── Velocity shaping (same as PR 3 — see comments preserved below) ──
    float gainGlobal = CVarGetFloat(CVAR_AUDIO("FluidSynthGain"), 1.0f);
    float entryGain = e.gain;
    if (entryGain == 0.0f)
        entryGain = 1.0f;
    float tempVol = GetTemporaryVolume(fontId, instOrWave);
    float shaped = std::clamp(sqrtf(std::max(0.0f, velocity)) * gainGlobal * entryGain * tempVol, 0.0f, 1.0f);

    uint8_t noteOnVel;
    uint8_t cc11val;
    if (mSynthMode == SynthMode::Enhanced) {
        noteOnVel = static_cast<uint8_t>(shaped * 127.0f);
        cc11val = 127;
    } else {
        noteOnVel = kFixedNoteOnVelocity;
        cc11val = static_cast<uint8_t>(shaped * 127.0f);
    }
    synth->ControlChange(targetChannel, 11, static_cast<uint16_t>(cc11val) << 7);

    // `pitchBend` is the engine's per-note pitch deviation from the nominal
    // note, expressed as a frequency ratio (1.0 = no bend). The engine adapter
    // (audio_playback.c) already stripped the non-pitch factors -- sample
    // tuning, global resampleRate, and the note itself, which FluidSynth
    // reproduces by playing midiNote -- so we hand the ratio straight to the
    // synth and let it own the semitone conversion and the wheel-range clamp.

    if (state.kind != SlotKind::Synth || state.pairFontId != fontId || state.pairInstOrWave != instOrWave ||
        midiNote != state.midiNote) {
        retireSlot();
        synth->NoteOnPitchFactor(targetChannel, midiNote, noteOnVel, pitchBend);
        state.kind = SlotKind::Synth;
        state.channel = targetChannel;
        state.midiNote = midiNote;
        state.pairFontId = fontId;
        state.pairInstOrWave = instOrWave;
        state.lastFreqScale = freqScale;
        if (mSynthActiveByPair[fontId][instOrWave] < 255) {
            mSynthActiveByPair[fontId][instOrWave]++;
        }
        bumpRoute(&DebugSlot::routedSynth);
        return true;
    }

    if (fabsf(freqScale - state.lastFreqScale) > 1e-6f) {
        synth->PitchBendFactor(state.channel, pitchBend);
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
    } else if (state.kind == SlotKind::Native) {
        if (BypassIndexValid(state.pairFontId, state.pairInstOrWave) &&
            mNativeActiveByPair[state.pairFontId][state.pairInstOrWave] > 0) {
            mNativeActiveByPair[state.pairFontId][state.pairInstOrWave]--;
        }
    }
    state.kind = SlotKind::Idle;
    state.pairInstOrWave = -1;
}

} // namespace SOH

// ─── C-linkage entry points called from audio_playback.c ───────────────────

extern "C" {

bool SOH_MidiTranslator_ProcessNote(int noteIndex, float freqScale, float velocity, uint8_t pan, float channelVolume,
                                    uint8_t fontId, int16_t instOrWave, uint8_t semitone, bool isFinished,
                                    uint8_t channelIdx, float resampleRate, float pitchBend) {
    return SOH::MidiTranslator::Instance().ProcessNote(noteIndex, freqScale, velocity, pan, channelVolume, fontId,
                                                       instOrWave, semitone, isFinished, channelIdx, resampleRate,
                                                       pitchBend);
}

void SOH_MidiTranslator_NoteDisabled(int noteIndex) {
    SOH::MidiTranslator::Instance().NoteDisabled(noteIndex);
}

void SOH_MidiTranslator_Reset() {
    SOH::MidiTranslator::Instance().Reset();
}

} // extern "C"
