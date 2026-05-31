#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace SOH {

// Per-channel state tracked across notes on the same MIDI channel.
// 0xFF / 0xFFFF sentinels mean "never sent" so the first NoteOn always
// emits a fresh ControlChange/ProgramChange.
struct ChannelState {
    bool     inited       = false;
    uint16_t lastPreset   = 0xFFFF; // last ProgramChange value (bank<<8|prog)
    int16_t  lastPinSfont = -1;     // last ProgramSelect sfontId; -1 = none / unpinned
    uint8_t  lastVolumeCC = 0xFF;   // last CC 7 (channel volume) sent, 7-bit
    uint8_t  lastPanCC    = 0xFF;   // last CC 10 (pan) sent, 7-bit
    // PR 7 per-pair effect CCs, cached so we only emit on change.
    uint8_t  lastReverbCC = 0xFF;   // CC 91 (reverb send)
    uint8_t  lastChorusCC = 0xFF;   // CC 93 (chorus send)
    uint8_t  lastCutoffCC = 0xFF;   // CC 74 (low-pass filter cutoff)
    uint8_t  lastQCC      = 0xFF;   // CC 71 (low-pass filter resonance)
};

// Which path a translator slot is currently routing audio through. Slots
// transition Idle → (Synth | Native) on first sight and back to Idle on
// NoteDisabled. Bypass mode changes mid-note trigger a Synth↔Native
// transition (retireSlot + re-init).
enum class SlotKind : uint8_t {
    Idle   = 0,
    Synth  = 1, // FluidSynth handles this slot's audio
    Native = 2, // Engine's native synth handles this slot's audio
};

// Per-note state tracked between Audio_ProcessNotes calls.
struct NoteTranslatorState {
    SlotKind kind          = SlotKind::Idle;
    uint8_t  channel       = 0;
    uint8_t  midiNote      = 0;
    // The (fontId, instOrWave) pair this slot is currently owned by. Used
    // so NoteDisabled() can decrement the right per-pair active counter
    // without having to look the engine state up again.
    uint8_t  pairFontId    = 0;
    int16_t  pairInstOrWave = -1;
    // freqScale captured at NoteOn. Pitch bend during the note is
    // 12 * log2(currentFreqScale / baseFreqScale) — independent of
    // sound->tuning since the tuning factor is baked into both values
    // and cancels out of the ratio.
    float    baseFreqScale = 1.0f;
    // Last freqScale we acted on, for change detection.
    float    lastFreqScale = 0.0f;
};

// Per-instrument bypass override surfaced via the AudioEditor UI.
// Indexed by (fontId, instOrWave & 0xFF). Read on the audio thread,
// written from the GUI thread.
enum class BypassMode : uint8_t {
    Auto        = 0, // follow the GM mapping table
    ForceNative = 1, // ignore the mapping; let the native engine play it
    ForceSynth  = 2, // try to play through FluidSynth even if unmapped
};

// Overall translator behaviour, set from AudioEditor and read by ProcessNote.
// Authentic keeps the PR 1.x routing (NoteOn velocity fixed, dynamics via CC11
// with sqrt curve), Enhanced sends the engine velocity through NoteOn directly
// and parks CC11 at max so the SF2's own modulators interpret dynamics.
enum class SynthMode : uint8_t {
    Authentic = 0, // pairs with Graham-Smith modulators + console-era reverb
    Enhanced  = 1, // pairs with stock SF2 modulators + low default reverb
};

// One entry per (fontId, instOrWave) pair the translator has actually
// seen. Cap is small on purpose — the GUI iterates this list, and the
// audio thread populates it. mapped indicates whether GetGmPreset()
// returned a non-kUnmapped result for the pair (UI hint).
struct DiscoveredPair {
    uint8_t fontId;
    int16_t instOrWave;
    bool    mapped;
};

class MidiTranslator {
  public:
    static MidiTranslator& Instance();

    // Called from the extern "C" hook in audio_playback.c once per active
    // note per audio update, immediately after Audio_InitNoteSub().
    //
    // noteIndex     : loop variable i in Audio_ProcessNotes (0..numNotes-1)
    // freqScale     : subAttrs.frequency (vibrato × portamento × resampleRate
    //                 already multiplied in by audio_playback.c)
    // velocity      : subAttrs.velocity (after ADSR scale multiplication)
    // pan           : subAttrs.pan (0-127 engine range)
    // channelVolume : layer->channel->volume — 0..1 channel fader
    // fontId        : note->playbackState.fontId
    // instOrWave    : resolved instOrWave — caller must have substituted
    //                 channel->instOrWave when layer->instOrWave == 0xFF
    // semitone      : layer->semitone (engine semitone 0-127, drives the
    //                 integer MIDI note via `semitone + 21` for melodic
    //                 instruments and direct passthrough for drums)
    // isFinished    : noteSubEu->bitField0.finished
    // channelIdx    : layer->channel->channelIndex (0-15)
    // resampleRate  : gAudioContext.audioBufferParameters.resampleRate
    //
    // Returns true when this note is being handled by the external synth
    // (preset is mapped and a synth is installed). The C hook uses the
    // return value to flip noteSubEu->bitField0.enabled = false, which
    // tells the native engine to skip rendering this note's PCM so the
    // two paths don't double up.
    bool ProcessNote(int noteIndex, float freqScale, float velocity, uint8_t pan, float channelVolume,
                     uint8_t fontId, int16_t instOrWave, uint8_t semitone, bool isFinished, uint8_t channelIdx,
                     float resampleRate);

    // Called when Audio_NoteDisable() fires. Sends NoteOff if the note
    // was active. noteIndex is (note - gAudioContext.notes).
    void NoteDisabled(int noteIndex);

    // Clears all per-note and per-channel state and sends CC123 (All Notes
    // Off) on every channel. Call on audio restart and scene transitions.
    void Reset();

    static constexpr int kMaxNotes        = 64;
    static constexpr int kMaxFontId       = 64;  // SoH has ≤ 38 built-in fonts, leave headroom
    static constexpr int kMaxInstOrWave   = 256; // instOrWave is uint8_t-effective
    static constexpr int kMaxDiscovered   = 128; // UI list cap
    // Matches FluidSynth::kNumChannels. Each unique (fontId, instOrWave) pair
    // gets its own MIDI channel for the session so per-pair effect CCs don't
    // stomp each other. When more than kMaxMidiChannels pairs are seen, the
    // overflow shares channel 0 (effects don't apply per-pair there).
    static constexpr uint8_t kMaxMidiChannels = 64;

    // ── Debug UI plumbing (bypass, gain, GM program override) ─────────────
    // GUI helpers — call from the main thread only.
    BypassMode GetBypass(uint8_t fontId, int16_t instOrWave) const;
    void       SetBypass(uint8_t fontId, int16_t instOrWave, BypassMode mode);

    // Snapshot of pairs the audio thread has reported. The vector is sized
    // at most kMaxDiscovered. Safe to call from the GUI thread.
    int  DiscoveredSnapshot(DiscoveredPair* out, int outCap) const;

    // Reset the discovered-pair list and seen bitmap. Per-instrument bypass
    // and gain overrides are preserved so the user can keep their tunings
    // when wiping the list to see what plays in a new area.
    void ClearDiscovered();

    // Number of note slots currently routing this pair through the synth
    // (returns 0 if the pair is being played by the engine's native synth,
    // or not playing at all). Used by the UI to color-tint rows green.
    uint8_t GetSynthActiveCount(uint8_t fontId, int16_t instOrWave) const;

    // Same idea but for slots routed through the native engine. UI tints
    // these blue. A pair can have both > 0 if multiple slots overlap; in
    // practice the UI prioritises the synth tint.
    uint8_t GetNativeActiveCount(uint8_t fontId, int16_t instOrWave) const;

    // Per-instrument gain multiplier applied to CC11 (expression).
    // 0.0 (default) means "no override" and the translator uses 1.0×.
    // Any other value is the multiplier; the slider in the UI exposes
    // 0–4× range. Setting to a non-default value overrides the global
    // FluidSynthGain for this pair only.
    float GetPerInstrumentGain(uint8_t fontId, int16_t instOrWave) const;
    void  SetPerInstrumentGain(uint8_t fontId, int16_t instOrWave, float gain);

    // Per-instrument GM program override (bank=0/melodic forced when set).
    // -1 (default) means "no override" — use the GmInstrumentMap entry.
    // Valid range: 0..127. This is the discovery tool used to audit and
    // correct entries in the built-in mapping table; saved overrides will
    // later feed the JSON mapping mods (PR 4).
    int16_t GetProgramOverride(uint8_t fontId, int16_t instOrWave) const;
    void    SetProgramOverride(uint8_t fontId, int16_t instOrWave, int16_t program);

    // Per-instrument semitone transpose. 0 (default) leaves the MIDI note
    // alone; non-zero adds to the melodic note number. Drums skip this
    // (they use the GM drum-slot number as the MIDI note, not a pitch).
    // Stored as signed int8_t so the legal range is -128..127, but the UI
    // clamps to -24..+24 (two octaves either way).
    int8_t GetTransposeOverride(uint8_t fontId, int16_t instOrWave) const;
    void   SetTransposeOverride(uint8_t fontId, int16_t instOrWave, int8_t semis);

    // ── PR 7: per-pair effect sends + filter (CC91/CC93/CC74/CC71) ─────────
    // All four are stored with a -1 sentinel = "no override". When set,
    // the value is the 7-bit CC value (0..127) emitted on the pair's
    // MIDI channel on each NoteOn (gated by the channel-state cache so
    // we don't spam the synth).
    int8_t GetReverbSend(uint8_t fontId, int16_t instOrWave) const;
    void   SetReverbSend(uint8_t fontId, int16_t instOrWave, int8_t cc);
    int8_t GetChorusSend(uint8_t fontId, int16_t instOrWave) const;
    void   SetChorusSend(uint8_t fontId, int16_t instOrWave, int8_t cc);
    int8_t GetFilterCutoff(uint8_t fontId, int16_t instOrWave) const;
    void   SetFilterCutoff(uint8_t fontId, int16_t instOrWave, int8_t cc);
    int8_t GetFilterResonance(uint8_t fontId, int16_t instOrWave) const;
    void   SetFilterResonance(uint8_t fontId, int16_t instOrWave, int8_t cc);

    // ── PR 8 (JSON ext): per-pair SF2 bank override ───────────────────────
    // -1 (default) means "no override" — pairs with a program override use
    // bank 0 (GM melodic). When set, valid range is 0..255; values above 0
    // route to community SF2 banks (Xadra uses banks 10/15/20/25 for SFX
    // and ambience; bank 128 is GM percussion). Has no effect unless
    // mProgramOverride is also set, since GetGmPreset only routes pairs
    // that the mapping table marks playable.
    int16_t GetBankOverride(uint8_t fontId, int16_t instOrWave) const;
    void    SetBankOverride(uint8_t fontId, int16_t instOrWave, int16_t bank);

    // ── Ship 1: pack pinning (audio routing) ───────────────────────────────
    // Set the FluidSynth sfontId the pair should pin to when its program
    // override is active, or -1 to clear. AudioEditor calls this at apply
    // time after the SF2 stack loads, walking the pairs that have preset
    // metadata: live entries (pack loaded AND the SF2 has the
    // (bank, program) tuple) get a non-negative sfontId, dead entries get
    // -1 so ProcessNote skips them and native plays.
    int16_t GetPinnedSfontId(uint8_t fontId, int16_t instOrWave) const;
    void    SetPinnedSfontId(uint8_t fontId, int16_t instOrWave, int16_t sfontId);

    // ── PR 8: preset-source metadata (recoverability) ─────────────────────
    // When the user picks a preset from the Bank/Pack + Preset combos, we
    // record the pack name and the SF2 preset name *as authored* alongside
    // the (bank, program) override. The audible result is still determined
    // by FluidSynth's (bank, program) lookup — these strings are purely
    // for human recovery when the SF2 stack changes:
    //   • If the pack the user authored against is renamed / removed,
    //     the bypass UI surfaces the saved name in the preview and tooltip
    //     so the user knows which pack to put back.
    //   • If a different pack now resolves the same (bank, program), the
    //     UI flags the discrepancy ("authored: 'Orchestral Harp', now
    //     resolves to 'Soft Piano'") so the user can re-pick.
    // Stored sparsely — most pairs have no override, and string per cell
    // would burn ~512 KB of empty std::string slots in dense arrays.
    struct PresetMetadata {
        std::string packName;
        std::string presetName;
    };
    std::string GetPresetPackName(uint8_t fontId, int16_t instOrWave) const;
    std::string GetPresetName(uint8_t fontId, int16_t instOrWave) const;
    bool        HasPresetMetadata(uint8_t fontId, int16_t instOrWave) const;
    void        SetPresetMetadata(uint8_t fontId, int16_t instOrWave,
                                  const std::string& packName, const std::string& presetName);
    void        ClearPresetMetadata(uint8_t fontId, int16_t instOrWave);

    // ── Transient session-only overrides (NEVER persisted) ────────────────
    // These survive only for the current process lifetime. Used by the
    // bypass UI's Override column to give modders / tuners session-scoped
    // controls without polluting fluidsynth_overrides.json.
    //
    // Mute: true = ProcessNote returns true (suppressing native via the
    // C-hook's bitField0 path) AND skips the synth NoteOn, so the pair
    // is silent on both paths. Used by the Solo button — it adds every
    // other discovered pair to the set and removes them again on unsolo.
    bool IsTemporarilyMuted(uint8_t fontId, int16_t instOrWave) const;
    void SetTemporaryMute(uint8_t fontId, int16_t instOrWave, bool muted);
    void ClearAllTemporaryMutes();

    // Temporary per-pair volume multiplier (default 1.0). Applies on top
    // of the persisted gain in ProcessNote's shaped-velocity calc. Affects
    // synth output only — native voices pass through unchanged. UI greys
    // the widget for pairs in Native mode for that reason.
    float GetTemporaryVolume(uint8_t fontId, int16_t instOrWave) const;
    void  SetTemporaryVolume(uint8_t fontId, int16_t instOrWave, float vol);
    void  ClearAllTemporaryVolumes();

    // ── Ship 3: custom display_name per pair ──────────────────────────────
    // Modder-friendly free-text label that overrides the auto-generated
    // "Font · slot" row label in the bypass UI. Persisted alongside the
    // other override fields so a mod's mapping.json can ship sensible
    // names like "Hyrule Field strings" that mean something to humans
    // browsing the table later. Empty string = no override (clears the
    // sparse entry).
    std::string GetDisplayName(uint8_t fontId, int16_t instOrWave) const;
    void        SetDisplayName(uint8_t fontId, int16_t instOrWave, const std::string& name);

    // ── User-modified tracking (for auto-save filtering) ──────────────────
    // Each pair where the user (via UI) explicitly set anything joins this
    // set. SaveOverridesToFile then writes only these pairs, so the user
    // JSON stays a true "diff on top of pack chain" instead of accumulating
    // pack-derived entries every time auto-save runs.
    //
    // - Marked by AudioEditor on every user UI edit
    // - Unmarked when the user picks "Default" in the Preset combo
    // - Loaded from disk: ApplyOverridesFromFile marks every parsed pair
    //   (they came from the user file, so they are user-authored)
    // - NOT marked by ApplyOverridesFromString (used for defaults + pack
    //   mapping.json overlay — those are NOT the user's edits)
    // - ResetAllOverrides clears the set
    void MarkPairAsUserModified(uint8_t fontId, int16_t instOrWave);
    void UnmarkPairAsUserModified(uint8_t fontId, int16_t instOrWave);
    bool IsPairUserModified(uint8_t fontId, int16_t instOrWave) const;

    // ── PR 4.1: persistence ────────────────────────────────────────────────
    // Wipe every per-pair override (bypass, gain, program, transpose). Does
    // not touch the discovered list, the active-voice counters, or anything
    // else — only user-set preferences.
    void ResetAllOverrides();

    // Serialise every per-pair cell that differs from default into a JSON
    // file at `path`. Empty array if no overrides exist. Returns true on
    // success. GUI thread only.
    bool SaveOverridesToFile(const std::string& path) const;

    // Overlay JSON-serialised per-pair overrides from a string buffer onto
    // the current state. Does NOT reset anything beforehand — caller is
    // responsible for ResetAllOverrides() if a clean slate is needed.
    // Used to apply the built-in defaults (see DefaultFluidSynthOverrides.h)
    // and to merge user customisations from disk. Per-pair entries overlay
    // field by field — an entry without a "gain" key leaves the existing
    // gain untouched. Returns true on parse success; malformed entries are
    // logged and skipped without aborting the rest.
    bool ApplyOverridesFromString(const std::string& json);

    // Same as ApplyOverridesFromString but reads the buffer from `path`.
    // Missing file is logged at INFO (typical first run) and returns false.
    bool ApplyOverridesFromFile(const std::string& path);

    // PR 3: synth mode set by AudioEditor, read by ProcessNote each call.
    // Plain-byte write/read is atomic on x86; coarse staleness is fine.
    void      SetSynthMode(SynthMode mode) { mSynthMode = mode; }
    SynthMode GetSynthMode() const         { return mSynthMode; }

  private:
    MidiTranslator();

    // Records a (fontId, instOrWave) pair on first sight. Audio-thread side
    // of the discovery mechanism. Cheap fast path when already seen.
    void RecordDiscovery(uint8_t fontId, int16_t instOrWave, bool mapped);

    // Returns the MIDI channel assigned to this pair, allocating on first
    // sight. Allocation is linear (one channel per pair, never recycled);
    // returns 0 as the overflow fallback after kMaxMidiChannels unique
    // pairs have been allocated. Drum vs melodic is decided by the
    // ProgramChange path (bank 128 flips the channel type per call), so
    // we don't reserve a specific drum channel here.
    uint8_t AllocateChannelForPair(uint8_t fontId, int16_t instOrWave);

    NoteTranslatorState mNoteState[kMaxNotes]                = {};
    ChannelState        mChannelState[kMaxMidiChannels]      = {};

    // 64 × 256 = 16 KB. Plain bytes — single-byte stores are atomic on x86.
    // We don't strictly need atomic semantics because the UI writes are
    // rare and we tolerate a one-tick staleness on the audio side.
    uint8_t mBypass[kMaxFontId][kMaxInstOrWave] = {};

    // Active note slots per (fontId, instOrWave), counted separately for
    // each routing path. Incremented on slot transitions (Idle → Synth or
    // Idle → Native), decremented when the slot retires or transitions
    // back to Idle. UI tints rows from these.
    uint8_t mSynthActiveByPair[kMaxFontId][kMaxInstOrWave]  = {};
    uint8_t mNativeActiveByPair[kMaxFontId][kMaxInstOrWave] = {};

    // Per-instrument gain override. 0.0 means "no override" → translator
    // applies 1.0×. Single 32-bit writes from the GUI are coherent for
    // the audio thread on x86; eventual consistency is acceptable.
    float mGainByPair[kMaxFontId][kMaxInstOrWave] = {};

    // Per-instrument GM program override. -1 means "no override" — fall
    // back to the GmInstrumentMap entry. Constructor fills the array with
    // -1 (zero-init would otherwise be valid program 0 = Acoustic Grand Piano).
    int16_t mProgramOverride[kMaxFontId][kMaxInstOrWave];

    // Per-instrument semitone transpose. 0 = no shift. Zero-init is the
    // safe default here so no explicit constructor pass is required.
    int8_t  mTransposeByPair[kMaxFontId][kMaxInstOrWave] = {};

    // PR 7: per-pair effect CCs. -1 = no override (the constructor seeds
    // every cell). When set, the value is sent on the pair's channel as
    // the 7-bit MIDI CC value. Cached against the channel state so we
    // only emit on change.
    int8_t  mReverbByPair[kMaxFontId][kMaxInstOrWave];        // CC 91
    int8_t  mChorusByPair[kMaxFontId][kMaxInstOrWave];        // CC 93
    int8_t  mFilterCutoffByPair[kMaxFontId][kMaxInstOrWave];  // CC 74
    int8_t  mFilterQByPair[kMaxFontId][kMaxInstOrWave];       // CC 71

    // PR 8 (JSON ext): per-pair SF2 bank. -1 = no override; values are
    // forwarded to FluidSynth's bank-select for the pair's channel when
    // a program override is also set.
    int16_t mBankOverride[kMaxFontId][kMaxInstOrWave];

    // Ship 1 (pack pinning): per-pair sfontId. -1 = no pin (legacy
    // pack-agnostic path via ProgramChange). Non-negative = pinned to a
    // specific loaded SF2 via ProgramSelect; if AudioEditor's apply path
    // couldn't resolve the override's pack/bank/program to a loaded
    // preset, it leaves this -1 AND treats the override as "dead"
    // (mProgramOverride is cleared for the duration so native plays).
    // Runtime-only state — refreshed from mPresetMetadata at every apply,
    // never persisted to disk (the sfontId would be stale across sessions
    // anyway).
    int16_t mPinnedSfontId[kMaxFontId][kMaxInstOrWave];

    // PR 8: preset-source metadata, sparse — only the pairs the user
    // explicitly picked from the Preset combo have entries here.
    // ResetAllOverrides clears the whole map; per-pair clears happen via
    // ClearPresetMetadata when the user picks Default.
    std::map<std::pair<uint8_t, int16_t>, PresetMetadata> mPresetMetadata;

    // Ship 3: custom display_name per pair, sparse.
    std::map<std::pair<uint8_t, int16_t>, std::string> mDisplayName;

    // Transient session-only state, sparse — never serialised.
    std::set<std::pair<uint8_t, int16_t>>  mTemporaryMute;
    std::map<std::pair<uint8_t, int16_t>, float> mTemporaryVolume;

    // Set of pairs the user has explicitly authored via the UI. Drives
    // SaveOverridesToFile's "save only the user diff" semantics — see the
    // public Mark/Unmark/IsPairUserModified docs above.
    std::set<std::pair<uint8_t, int16_t>> mUserModified;

    // Per-pair MIDI channel allocation. Allocated linearly on first sight
    // and never recycled: keeping the mapping stable means a pair's
    // ProgramChange / CC91 / etc state survives across NoteOff and stays
    // unique to that pair. 0xFF = unallocated. When the pool runs out
    // (more than kMaxMidiChannels pairs heard), overflow pairs all share
    // channel 0 and their per-pair effects collapse to a single value
    // (logged once).
    uint8_t  mPairChannel[kMaxFontId][kMaxInstOrWave];
    uint8_t  mChannelsAllocated = 0;

    // Discovery bitmap: 1 bit per (fontId, instOrWave) slot. 16384 bits = 2 KB.
    std::array<uint64_t, (kMaxFontId * kMaxInstOrWave) / 64> mSeenBits{};

    // Discovered list, populated in arrival order. mDiscoveredCount is
    // atomic so the GUI can read a stable size; entries are stable once
    // written.
    DiscoveredPair       mDiscovered[kMaxDiscovered] = {};
    std::atomic<int>     mDiscoveredCount{0};

    // Active synth mode. Authentic by default — pairs with Graham-Smith
    // modulators and console-era reverb, see AudioEditor for the
    // FluidSynth-side configuration.
    SynthMode            mSynthMode = SynthMode::Authentic;
};

} // namespace SOH
