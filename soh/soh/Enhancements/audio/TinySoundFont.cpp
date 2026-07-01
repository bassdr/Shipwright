#if ENABLE_TINYSOUNDFONT
#include "soh/Enhancements/audio/TinySoundFont.h"

// Declarations only. The TSF implementation (and stb_vorbis, which it uses to
// decode SF3 Ogg samples) is compiled in isolation in TinySoundFontImpl.cpp --
// see there for why those two must stand alone. TSF's API is `extern` by default,
// so this TU calls it and links against that implementation.
#include <tsf.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace SOH {

TinySoundFont::TinySoundFont(const TinySoundFontConfig& config)
    : mSampleRate(config.sampleRate), mPolyphony(config.polyphony), mGain(config.gain) {
    for (int i = 0; i < kNumChannels; ++i) {
        mChannelFont[i] = -1;
        mChannelWheel[i] = 8192; // center
    }
    SPDLOG_INFO("[TinySoundFont] Synth created. sampleRate={} polyphony={} gain={}", mSampleRate, mPolyphony, mGain);
}

TinySoundFont::~TinySoundFont() {
    ClearSoundFonts();
}

void TinySoundFont::ConfigureFont(tsf* font) {
    // Interleaved stereo at the device rate; gain is applied linearly via
    // set_volume, so the dB term in set_output stays at unity (0 dB).
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, mSampleRate, 0.0f);
    tsf_set_volume(font, mGain);
    tsf_set_max_voices(font, mPolyphony);
}

void TinySoundFont::ClearSoundFonts() {
    std::lock_guard<std::mutex> lock(mMutex);
    for (tsf* font : mFonts) {
        if (font)
            tsf_close(font);
    }
    mFonts.clear();
    for (int i = 0; i < kNumChannels; ++i) {
        mChannelFont[i] = -1;
        mChannelWheel[i] = 8192;
    }
}

int TinySoundFont::AddSoundFont(const std::string& path) {
    std::lock_guard<std::mutex> lock(mMutex);
    tsf* font = tsf_load_filename(path.c_str());
    if (!font) {
        SPDLOG_ERROR("[TinySoundFont] Failed to load SF: {}", path);
        return -1;
    }
    ConfigureFont(font);
    mFonts.push_back(font);
    int id = static_cast<int>(mFonts.size()) - 1;
    SPDLOG_INFO("[TinySoundFont] Loaded SF: {} (id={}, presets={})", path, id, tsf_get_presetcount(font));
    return id;
}

int TinySoundFont::AddSoundFontFromMemory(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (data == nullptr || size == 0)
        return -1;
    // tsf_load_memory copies what it needs out of the buffer, so the caller's
    // copy can be freed right after this returns.
    tsf* font = tsf_load_memory(data, static_cast<int>(size));
    if (!font) {
        SPDLOG_ERROR("[TinySoundFont] Failed to load SF from memory ({} bytes)", size);
        return -1;
    }
    ConfigureFont(font);
    mFonts.push_back(font);
    int id = static_cast<int>(mFonts.size()) - 1;
    SPDLOG_INFO("[TinySoundFont] Loaded SF from memory ({} bytes, id={}, presets={})", size, id,
                tsf_get_presetcount(font));
    return id;
}

void TinySoundFont::LoadSoundFont(const std::string& path) {
    ClearSoundFonts();
    AddSoundFont(path);
}

std::vector<IMidiSynth::LoadedPreset> TinySoundFont::EnumerateLoadedPresets() {
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<LoadedPreset> result;
    // tsf exposes preset names by index but not their (bank, program) tuple, so we
    // probe the standard MIDI bank/program space (0..128 / 0..127, 128 = drum bank)
    // and keep the hits. Cheap: runs only when the loaded set changes.
    for (int id = 0; id < static_cast<int>(mFonts.size()); ++id) {
        tsf* font = mFonts[id];
        if (!font)
            continue;
        for (int bank = 0; bank <= 128; ++bank) {
            for (int program = 0; program < 128; ++program) {
                if (tsf_get_presetindex(font, bank, program) < 0)
                    continue;
                LoadedPreset p;
                p.sfontId = id;
                p.bank = bank;
                p.program = program;
                const char* nm = tsf_bank_get_presetname(font, bank, program);
                p.name = nm ? nm : "";
                result.push_back(std::move(p));
            }
        }
    }
    return result;
}

int TinySoundFont::ResolveFont(int bank, int program) const {
    // Last-loaded-wins on (bank, program) collisions: walk in reverse load order
    // and take the first font that has the preset.
    for (int id = static_cast<int>(mFonts.size()) - 1; id >= 0; --id) {
        if (mFonts[id] && tsf_get_presetindex(mFonts[id], bank, program) >= 0)
            return id;
    }
    return -1;
}

void TinySoundFont::RouteChannel(uint8_t channel, int fontIndex) {
    int prev = mChannelFont[channel];
    if (prev == fontIndex)
        return;
    // Silence the channel on the font it is leaving so stale voices don't hang.
    if (prev >= 0 && prev < static_cast<int>(mFonts.size()) && mFonts[prev])
        tsf_channel_note_off_all(mFonts[prev], channel);
    mChannelFont[channel] = fontIndex;
    if (fontIndex >= 0) {
        tsf_channel_set_pitchrange(mFonts[fontIndex], channel, kPitchBendRangeSemitones);
        tsf_channel_set_pitchwheel(mFonts[fontIndex], channel, mChannelWheel[channel]);
    }
}

void TinySoundFont::NoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    std::lock_guard<std::mutex> lock(mMutex);
    int id = mChannelFont[channel];
    if (id < 0)
        return;
    tsf_channel_note_on(mFonts[id], channel, note, velocity / 127.0f);
}

void TinySoundFont::NoteOff(uint8_t channel, uint8_t note) {
    std::lock_guard<std::mutex> lock(mMutex);
    int id = mChannelFont[channel];
    if (id < 0)
        return;
    tsf_channel_note_off(mFonts[id], channel, note);
}

void TinySoundFont::ProgramChange(uint8_t channel, uint16_t preset) {
    std::lock_guard<std::mutex> lock(mMutex);
    int bank = (preset >> 8) & 0xFF;
    int program = preset & 0xFF;

    int id = ResolveFont(bank, program);
    if (id < 0) {
        SPDLOG_TRACE("[TinySoundFont] ProgramChange ch={} bank={} prog={} -- no loaded SF has it", channel, bank,
                     program);
        RouteChannel(channel, -1);
        return;
    }
    RouteChannel(channel, id);
    tsf_channel_set_bank_preset(mFonts[id], channel, bank, program);
}

bool TinySoundFont::ProgramSelect(uint8_t channel, int sfontId, uint16_t bank, uint16_t program) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (sfontId < 0 || sfontId >= static_cast<int>(mFonts.size()) || !mFonts[sfontId])
        return false;
    if (tsf_get_presetindex(mFonts[sfontId], bank, program) < 0) {
        SPDLOG_TRACE("[TinySoundFont] ProgramSelect ch={} sfontId={} bank={} prog={} -> not in that SF", channel,
                     sfontId, bank, program);
        return false;
    }
    RouteChannel(channel, sfontId);
    tsf_channel_set_bank_preset(mFonts[sfontId], channel, bank, program);
    return true;
}

void TinySoundFont::PitchBend(uint8_t channel, float semitones) {
    std::lock_guard<std::mutex> lock(mMutex);
    float ratio = semitones / kPitchBendRangeSemitones;
    int wheel = static_cast<int>(ratio * 8192.0f) + 8192;
    wheel = std::clamp(wheel, 0, 16383);
    mChannelWheel[channel] = wheel;
    int id = mChannelFont[channel];
    if (id >= 0)
        tsf_channel_set_pitchwheel(mFonts[id], channel, wheel);
}

void TinySoundFont::ControlChange(uint8_t channel, uint8_t cc, uint16_t value) {
    std::lock_guard<std::mutex> lock(mMutex);
    int id = mChannelFont[channel];
    if (id < 0)
        return;
    // tsf takes 7-bit CC values; the interface passes a 14-bit value.
    tsf_channel_midi_control(mFonts[id], channel, cc, (value >> 7) & 0x7F);
}

void TinySoundFont::Render(float* out, uint32_t frameCount) {
    std::lock_guard<std::mutex> lock(mMutex);
    std::memset(out, 0, frameCount * 2 * sizeof(float));
    // Accumulate every loaded font into the same buffer (flag_mixing = 1).
    for (tsf* font : mFonts) {
        if (font && tsf_active_voice_count(font) > 0)
            tsf_render_float(font, out, static_cast<int>(frameCount), /*flag_mixing=*/1);
    }
}

uint32_t TinySoundFont::GetActiveVoiceCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    int count = 0;
    for (tsf* font : mFonts) {
        if (font)
            count += tsf_active_voice_count(font);
    }
    return count < 0 ? 0u : static_cast<uint32_t>(count);
}

uint32_t TinySoundFont::GetPolyphonyLimit() const {
    // Per-font ceiling. With multiple fonts the true ceiling is higher, but voices
    // are stolen per font, so the per-font cap is what a single sound hits first.
    return mPolyphony < 0 ? 0u : static_cast<uint32_t>(mPolyphony);
}

void TinySoundFont::SetMasterGain(float gain) {
    std::lock_guard<std::mutex> lock(mMutex);
    mGain = gain;
    for (tsf* font : mFonts) {
        if (font)
            tsf_set_volume(font, gain);
    }
}

} // namespace SOH
#endif // ENABLE_TINYSOUNDFONT
