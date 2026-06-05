#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SOH {

// Per-channel state tracked across notes on the same MIDI channel.
// 0xFF / 0xFFFF sentinels mean "never sent" so the first NoteOn always
// emits a fresh ControlChange/ProgramChange.
struct ChannelState {
    bool     inited       = false;
    uint16_t lastPreset   = 0xFFFF; // last ProgramChange value (bank<<8|prog)
    int16_t  lastPinSfont = -1;     // last ProgramSelect sfontId; -1 = unpinned
    uint8_t  lastVolumeCC = 0xFF;   // last CC 7 (channel volume) sent, 7-bit
    uint8_t  lastPanCC    = 0xFF;   // last CC 10 (pan) sent, 7-bit
    uint8_t  lastReverbCC = 0xFF;   // CC 91 (reverb send)
    uint8_t  lastChorusCC = 0xFF;   // CC 93 (chorus send)
    uint8_t  lastCutoffCC = 0xFF;   // CC 74 (low-pass filter cutoff)
    uint8_t  lastQCC      = 0xFF;   // CC 71 (low-pass filter resonance)
};

// Which path a translator slot is currently routing audio through. Slots
// transition Idle -> (Synth | Native) on first sight and back to Idle on
// NoteDisabled.
enum class SlotKind : uint8_t {
    Idle   = 0,
    Synth  = 1,
    Native = 2,
};

// Per-note state tracked between Audio_ProcessNotes calls.
struct NoteTranslatorState {
    SlotKind kind          = SlotKind::Idle;
    uint8_t  channel       = 0;
    uint8_t  midiNote      = 0;
    uint8_t  pairFontId    = 0;
    int16_t  pairInstOrWave = -1;
    float    lastFreqScale = 0.0f;
};

// Overall translator behaviour, set from AudioEditor and read by ProcessNote.
enum class SynthMode : uint8_t {
    Authentic = 0, // Graham-Smith modulators + console-era reverb
    Enhanced  = 1, // stock SF2 modulators + low default reverb
};

// One entry per (fontId, instOrWave) pair the translator has actually
// seen. mapped indicates the GM mapping table had a non-kUnmapped value
// (UI hint only since the new resolution model doesn't use the table).
struct DiscoveredPair {
    uint8_t fontId;
    int16_t instOrWave;
    bool    mapped;
};

// Where a ConfigEntry came from. Decides persistence eligibility and
// whether the entry is removed when its pack is unloaded.
enum class EntrySource : uint8_t {
    UserPicked  = 0, // From fluidsynth_overrides.json or a UI pick
    ModSupplied = 1, // From a pack mapping.json; not persisted, removed on
                     // pack unload
};

// One row in the configuration. Multiple entries can share
// (fontId, instOrWave); each represents a distinct candidate keyed by
// (pack, program). Resolution at play time picks the enabled+resolvable
// candidate whose pack is last in the load order.
//
// IMPORTANT: the audio thread reads primitive fields of this struct via
// mEntries[mActiveEntryIdx[f][i]] without locking. mEntries is reserved
// at construction so push_back never reallocates. Strings are NOT touched
// from the audio thread.
struct ConfigEntry {
    uint8_t     fontId        = 0;
    int16_t     instOrWave    = -1;
    std::string packName;            // "" = placeholder ("None") entry
    int16_t     program       = -1;  // -1 = placeholder (no audible output)
    int16_t     bank          = 0;
    std::string presetName;          // user-facing label / recovery hint
    float       gain          = 0.0f;// 0.0 = use default 1.0x
    int8_t      transpose     = 0;
    int8_t      reverb        = -1;  // CC 91; -1 = no override (default 0)
    int8_t      chorus        = -1;  // CC 93; -1 = no override (default 0)
    int8_t      cutoff        = -1;  // CC 74; -1 = no override (default 64)
    int8_t      q             = -1;  // CC 71; -1 = no override (default 64)
    bool        enabled       = false;
    bool        selected      = false;
    uint32_t    lastEnabledSeq = 0;  // process-lifetime; not persisted
    EntrySource source        = EntrySource::UserPicked;
    // Runtime: resolved sfontId for ProgramSelect. -1 when the pack isn't
    // currently loaded OR the SF2 doesn't have (bank, program).
    // Refreshed by AudioEditor's pack-apply path; never persisted.
    int16_t     sfontId       = -1;
};

class MidiTranslator {
  public:
    static MidiTranslator& Instance();

    // Audio-thread entry point (see audio_playback.c hook).
    bool ProcessNote(int noteIndex, float freqScale, float velocity, uint8_t pan, float channelVolume,
                     uint8_t fontId, int16_t instOrWave, uint8_t semitone, bool isFinished, uint8_t channelIdx,
                     float resampleRate, float pitchBend);
    void NoteDisabled(int noteIndex);
    void Reset();

    static constexpr int kMaxNotes        = 64;
    static constexpr int kMaxFontId       = 64;
    static constexpr int kMaxInstOrWave   = 256;
    static constexpr int kMaxDiscovered   = 128;
    static constexpr uint8_t kMaxMidiChannels = 64;
    // mEntries capacity reserved at construction so push_back never
    // reallocates within session bounds. The audio thread reads entry
    // fields by index without locking; reallocation would invalidate
    // those reads.
    static constexpr size_t kMaxEntries   = 4096;

    // ── Discovery snapshot for the UI ────────────────────────────────────
    int  DiscoveredSnapshot(DiscoveredPair* out, int outCap) const;
    void ClearDiscovered();
    uint8_t GetSynthActiveCount(uint8_t fontId, int16_t instOrWave) const;
    uint8_t GetNativeActiveCount(uint8_t fontId, int16_t instOrWave) const;

    // DEBUG: per-pair running stats accumulated by ProcessNote. Used by the
    // AudioEditor's per-row "[DBG]" popup to investigate octave-shift,
    // bend magnitude, and missing-note questions. Counters are atomic;
    // float min/max are written by the audio thread and read by the UI
    // without locking — best-effort, a torn read is harmless cosmetic.
    struct DebugPairStats {
        uint32_t noteOns          = 0;
        uint32_t routedSynth      = 0;
        uint32_t routedNative     = 0;
        uint32_t routedMute       = 0;  // bank == 128 / engine drum-SFX / placeholder
        uint32_t bendUpdates      = 0;  // freqScale changes after NoteOn
        float    baseFreqScaleMin = 0.0f;
        float    baseFreqScaleMax = 0.0f;
        float    bendRatioMin     = 1.0f;
        float    bendRatioMax     = 1.0f;
        uint8_t  lastSemitone     = 0;
    };
    DebugPairStats GetDebugStats(uint8_t fontId, int16_t instOrWave) const;
    void           ResetDebugStats();
    void           ResetDebugStatsForPair(uint8_t fontId, int16_t instOrWave);

    // ── Pack stack + entry sfontId refresh ───────────────────────────────
    // Set by AudioEditor after every SF2 stack change. Used as the
    // multi-enabled tiebreak rank — last entry in this list wins.
    void SetPackLoadOrder(const std::vector<std::string>& order);

    // Refresh each entry's runtime sfontId from a flat list of
    // (pack, sfontId, bank, program) tuples — same shape as AudioEditor's
    // sLoadedPresets. Entries whose pack isn't in the list, or whose
    // (bank, program) doesn't exist in the matched SF2, get sfontId=-1.
    // The list is cached so PickPreset / ClickSynth can resolve sfontIds
    // for newly-created entries without a callback from the UI.
    struct LoadedPresetRef {
        int         sfontId;
        std::string packName;
        int         bank;
        int         program;
    };
    void RefreshEntrySfontIds(const std::vector<LoadedPresetRef>& loadedPresets);

    // Drop every ModSupplied entry whose pack is NOT in the supplied set,
    // so pack toggles don't leave orphan mod entries hanging with
    // sfontId=-1. User-saved entries stay (they're recoverable).
    void RemoveModEntriesNotIn(const std::set<std::string>& packNamesLoaded);

    // Rebuild mActiveEntryIdx for every (fontId, instOrWave). Called after
    // batches of mutations (pack load, file load).
    void RecomputeAllActive();

    // ── Entry queries (UI) ───────────────────────────────────────────────
    int                 GetEntryCount() const { return static_cast<int>(mEntries.size()); }
    const ConfigEntry&  GetEntry(int idx) const { return mEntries[idx]; }
    const ConfigEntry*  GetActiveEntry(uint8_t fontId, int16_t instOrWave) const;
    int                 GetActiveEntryIdx(uint8_t fontId, int16_t instOrWave) const;
    void                GetEntriesForPair(uint8_t fontId, int16_t instOrWave,
                                          std::vector<int>& outIdx) const;
    int                 FindEntry(uint8_t fontId, int16_t instOrWave,
                                  const std::string& pack, int16_t program) const;
    int                 CountSelectedEntries(uint8_t fontId, int16_t instOrWave) const;

    // ── Row UI actions ───────────────────────────────────────────────────
    // Picking a preset from the Preset combo: disable every currently-
    // enabled entry for (fontId, instOrWave); if the previous winner is
    // from the same pack as the new pick clear its selected; find-or-
    // create the picked entry; enable + select + bump lastEnabledSeq.
    void PickPreset(uint8_t fontId, int16_t instOrWave,
                    const std::string& pack, int16_t program, int16_t bank,
                    const std::string& presetName);
    // Disables every enabled entry for (fontId, instOrWave). selected
    // flags are preserved so ClickSynth can restore.
    void ClickNative(uint8_t fontId, int16_t instOrWave);
    // Restoration order:
    //   1. selected entries for (fontId, instOrWave) -> highest seq wins
    //   2. fallback: any disabled-but-resolvable entry (sfontId >= 0) ->
    //      highest seq wins (handles the mod-only-row case where the
    //      user never picked anything)
    //   3. placeholder { packName="", program=-1, presetName="None" }
    //      (audible result: muted)
    void ClickSynth(uint8_t fontId, int16_t instOrWave);

    // ── Per-entry mutators (UI Override widgets) ─────────────────────────
    void SetEntryGain(int idx, float gain);
    void SetEntryTranspose(int idx, int8_t semis);
    void SetEntryReverb(int idx, int8_t cc);
    void SetEntryChorus(int idx, int8_t cc);
    void SetEntryFilterCutoff(int idx, int8_t cc);
    void SetEntryFilterResonance(int idx, int8_t cc);

    // ── Per-pair display name (row label, sparse) ────────────────────────
    std::string GetDisplayName(uint8_t fontId, int16_t instOrWave) const;
    void        SetDisplayName(uint8_t fontId, int16_t instOrWave, const std::string& name);

    // ── Session-only transient state ─────────────────────────────────────
    bool  IsTemporarilyMuted(uint8_t fontId, int16_t instOrWave) const;
    void  SetTemporaryMute(uint8_t fontId, int16_t instOrWave, bool muted);
    void  ClearAllTemporaryMutes();
    float GetTemporaryVolume(uint8_t fontId, int16_t instOrWave) const;
    void  SetTemporaryVolume(uint8_t fontId, int16_t instOrWave, float vol);
    void  ClearAllTemporaryVolumes();

    // ── Persistence + reset ──────────────────────────────────────────────
    void ResetAllOverrides();
    bool SaveOverridesToFile(const std::string& path) const;
    // String overlay = pack mapping.json: new entries land with
    // source=ModSupplied, selected=false, enabled=true.
    bool ApplyOverridesFromString(const std::string& json);
    // File overlay = user fluidsynth_overrides.json: entries land with
    // source=UserPicked. enabled / selected come from the file; missing
    // fields default to true.
    bool ApplyOverridesFromFile(const std::string& path);

    // Publish-as-pack-mapping. Writes a mapping.json shaped for distribution
    // inside a synth pack: only enabled+selected entries belonging to
    // `packNameFilter` are emitted, runtime fields are stripped, and the
    // file carries a top-level "pack_name" field. Returns the number of
    // entries written (or -1 on I/O failure). When packNameFilter is empty,
    // every selected entry is emitted regardless of its source pack.
    int ExportPackMapping(const std::string& path, const std::string& packNameFilter) const;

    // Helper: how many entries `ExportPackMapping` would emit for a given
    // pack filter. Same predicate as the writer; lets the UI show a preview
    // count before the user clicks Export.
    int CountExportableEntries(const std::string& packNameFilter) const;

    void      SetSynthMode(SynthMode mode) { mSynthMode = mode; }
    SynthMode GetSynthMode() const         { return mSynthMode; }

  private:
    MidiTranslator();

    void    RecordDiscovery(uint8_t fontId, int16_t instOrWave, bool mapped);
    uint8_t AllocateChannelForPair(uint8_t fontId, int16_t instOrWave);

    // Find an entry by primary key; create a new one if not present.
    // Caller fills bank/presetName even when creating, and decides
    // source / selected / enabled after. Returns the entry index.
    int  FindOrCreateEntry(uint8_t fontId, int16_t instOrWave,
                           const std::string& pack, int16_t program,
                           int16_t bank, const std::string& presetName,
                           EntrySource source);

    // Recompute mActiveEntryIdx[fontId][instOrWave]. Resolution: filter
    // mEntries by (fontId, instOrWave) && enabled && sfontId>=0; pick the
    // candidate whose pack ranks highest in mPackLoadOrder (last loaded
    // wins). Placeholder entries (program<0) participate normally — they
    // resolve only when explicitly enabled and produce muted output.
    void RecomputeActive(uint8_t fontId, int16_t instOrWave);
    int  PackRank(const std::string& pack) const; // -1 if not in load order

    // Walks the cached loaded-preset list and returns the sfontId for
    // (pack, bank, program), or -1 if the pack isn't loaded / the SF2
    // doesn't have that preset. Used at entry-create time so a fresh
    // pick is immediately resolvable without waiting for the next
    // pack-stack change.
    int16_t ResolveSfontIdFromCache(const std::string& pack, int16_t bank, int16_t program) const;

    NoteTranslatorState mNoteState[kMaxNotes]                = {};
    ChannelState        mChannelState[kMaxMidiChannels]      = {};

    uint8_t mSynthActiveByPair[kMaxFontId][kMaxInstOrWave]  = {};
    uint8_t mNativeActiveByPair[kMaxFontId][kMaxInstOrWave] = {};

    uint8_t  mPairChannel[kMaxFontId][kMaxInstOrWave];
    uint8_t  mChannelsAllocated = 0;

    std::array<uint64_t, (kMaxFontId * kMaxInstOrWave) / 64> mSeenBits{};
    DiscoveredPair       mDiscovered[kMaxDiscovered] = {};
    std::atomic<int>     mDiscoveredCount{0};

    std::set<std::pair<uint8_t, int16_t>>        mTemporaryMute;
    std::map<std::pair<uint8_t, int16_t>, float> mTemporaryVolume;
    std::map<std::pair<uint8_t, int16_t>, std::string> mDisplayName;

    // DEBUG: per-pair stats backing storage. Counters use relaxed atomics;
    // float min/max are written from the audio thread and read from the UI
    // without synchronisation (best-effort diagnostic).
    struct DebugSlot {
        std::atomic<uint32_t> noteOns{0};
        std::atomic<uint32_t> routedSynth{0};
        std::atomic<uint32_t> routedNative{0};
        std::atomic<uint32_t> routedMute{0};
        std::atomic<uint32_t> bendUpdates{0};
        float baseFreqScaleMin = 0.0f;
        float baseFreqScaleMax = 0.0f;
        float bendRatioMin     = 1.0f;
        float bendRatioMax     = 1.0f;
        uint8_t lastSemitone   = 0;
    };
    DebugSlot mDebugStats[kMaxFontId][kMaxInstOrWave];

    // ── Ship 11: entry-based config storage ──────────────────────────────
    std::vector<ConfigEntry> mEntries;
    // -1 = no active entry for this pair → native plays.
    int16_t  mActiveEntryIdx[kMaxFontId][kMaxInstOrWave];
    uint32_t mNextSeq = 1;
    std::vector<std::string> mPackLoadOrder;
    // Cached copy of the AudioEditor's sLoadedPresets list. Refreshed by
    // RefreshEntrySfontIds; consumed by ResolveSfontIdFromCache so any
    // mutator (PickPreset, ClickSynth) can set a new entry's sfontId
    // immediately without going through another pack-stack pass.
    std::vector<LoadedPresetRef> mLoadedPresets;

    SynthMode mSynthMode = SynthMode::Authentic;
};

} // namespace SOH
