#pragma once
#if ENABLE_TINYSOUNDFONT

#include "IMidiSynth.h"
#include <mutex>
#include <vector>
#include <cstdint>

struct tsf; // forward decl; the tsf.h implementation is pulled into TinySoundFont.cpp only.

namespace SOH {

// Backend tuning supplied by the integrating game, mirroring FluidSynthConfig so
// the host can configure either backend the same way. TinySoundFont has no custom
// modulators or reverb, so the FluidSynth-only knobs (linearVelocity, reverb) have
// no equivalent here -- they are simply absent rather than ignored fields.
struct TinySoundFontConfig {
    // Output rate; must match the device (typically 44100 or 48000).
    int sampleRate = 44100;

    // Max simultaneous voices PER loaded soundfont. TinySoundFont steals the
    // oldest voice once this is hit. Sized generously since idle voices are cheap.
    int polyphony = 256;

    // Master output gain (linear; 1.0 = unity). Applied to every loaded font.
    float gain = 1.0f;
};

// A lightweight IMidiSynth backend built on TinySoundFont (tsf.h), a header-only
// SF2/SF3 synth with no external dependency. It is feature-thin next to FluidSynth
// -- no custom velocity modulators and no reverb -- but pulls in zero libraries,
// which is its whole reason to exist as an alternative backend.
//
// TinySoundFont loads exactly one soundfont per tsf instance, so multi-pack
// stacking is done here: one tsf per loaded SF, with cross-font preset lookup
// (last-loaded-wins) and per-channel routing layered on top to match the
// IMidiSynth contract FluidSynth implements natively.
class TinySoundFont final : public IMidiSynth {
  public:
    explicit TinySoundFont(const TinySoundFontConfig& config);
    ~TinySoundFont() override;

    // Single-shot replace: unloads every loaded SF then loads this one.
    void LoadSoundFont(const std::string& path) override;

    // Add an SF (path or memory) alongside any already-loaded ones. Returns the
    // sfont id (its load index) on success, or -1 on failure.
    int AddSoundFont(const std::string& path);
    int AddSoundFontFromMemory(const uint8_t* data, size_t size) override;

    // Unload every loaded SF. Safe to call when none are loaded.
    void ClearSoundFonts();

    std::vector<LoadedPreset> EnumerateLoadedPresets() override;

    void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void NoteOff(uint8_t channel, uint8_t note) override;
    void ProgramChange(uint8_t channel, uint16_t preset) override;
    bool ProgramSelect(uint8_t channel, int sfontId, uint16_t bank, uint16_t program) override;
    void PitchBend(uint8_t channel, float semitones) override;
    void ControlChange(uint8_t channel, uint8_t cc, uint16_t value) override;
    void Render(float* out, uint32_t frameCount) override;
    uint32_t GetActiveVoiceCount() const override;
    uint32_t GetPolyphonyLimit() const override;
    void SetMasterGain(float gain) override;

    // Pitch bend range in semitones. Matches FluidSynth's default so the
    // MidiTranslator's bend math lands identically on either backend.
    static constexpr float kPitchBendRangeSemitones = 12.0f;

  private:
    // Apply sample rate / polyphony / gain to a freshly-loaded font.
    void ConfigureFont(tsf* font);

    // Pick the loaded font that should sound (bank, program), honoring
    // last-loaded-wins. Returns the font index, or -1 if none has it.
    int ResolveFont(int bank, int program) const;

    // Point `channel` at `fontIndex`, silencing the channel on its previous
    // font if it moved, and (re)establishing the channel's pitch state.
    void RouteChannel(uint8_t channel, int fontIndex);

    static constexpr int kNumChannels = 64;

    // One tsf per loaded SF, in load order. The index doubles as the sfont id
    // exposed through ProgramSelect / EnumerateLoadedPresets.
    std::vector<tsf*> mFonts;

    int mSampleRate;
    int mPolyphony;
    float mGain;

    // Which loaded font each channel currently sounds through (-1 = unassigned).
    int mChannelFont[kNumChannels];

    // Per-channel 14-bit pitch wheel (0..16383, 8192 = center). Cached so a
    // re-route to another font re-applies the channel's current bend.
    int mChannelWheel[kNumChannels];

    // Serializes Render() (audio thread) against the game-thread mutators.
    mutable std::mutex mMutex;
};

} // namespace SOH

#endif // ENABLE_TINYSOUNDFONT
