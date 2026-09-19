#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace SOH {

// How one punctuation mark is spoken. The model decides the intonation from the
// mark it is given; everything else here is applied to the audio around it.
struct VoiceMark {
    // What the model is fed in place of the mark. Empty feeds it nothing, and a
    // different mark borrows that one's intonation - a colon read as a comma.
    std::string emit;
    // Seconds of silence after the mark. Any mark with a pause is rendered as its
    // own segment, which is what makes the length exact rather than the model's
    // own guess.
    float pause = 0.0f;
    float emphasis = 1.0f;
    // Semitones, applied to the segment the mark closes. A laugh is a run of
    // bursts that fall away; without this every burst comes out at the same pitch.
    float pitch = 0.0f;
    // Multiplies the voice's own length scale for the segment the mark closes.
    // Below 1 is faster and reads as more urgent.
    float lengthScale = 1.0f;
};

// Per-language speech tuning, from voice-speech.json. It is the player's file:
// espeak's idea of a word is not always the right one, and how long a full stop
// should hold is a matter of taste.
struct VoiceSpeech {
    // Applied to the line before it is phonemised, in order. The replacement is
    // ordinary text respelled for the language - espeak reads "digne dogne" the
    // way a French speaker reads "Ding Dong".
    std::vector<std::pair<std::string, std::string>> say;
    // Applied to what espeak produced, in IPA, for when respelling the word is not
    // enough: this is where a vowel is lengthened with a run of length marks or the
    // stress is moved. Both symbols are in the model's own table.
    std::vector<std::pair<std::string, std::string>> phonemes;
    // Keyed on the run of marks as written, so "..." is tuned apart from ".": the
    // game spells a long pause as a row of dots and means something by it.
    std::map<std::string, VoiceMark> marks;
    // Dropped from the line before it is phonemised. A laugh written out as words
    // is read as words, and no amount of tuning made the model laugh instead, so
    // the spelling is left unvoiced rather than mangled.
    std::vector<std::string> unspoken;
};

// How one character is voiced: which model speaks the line and what is done to
// the result. Mirrors an entry of voice-profiles.json.
struct VoiceProfile {
    std::string model;
    int64_t speaker = 0;
    float lengthScale = 1.0f;
    float noiseScale = 0.667f;
    float noiseW = 0.8f;
    // Semitones. Pitch shifting here resamples, so the formants move with the
    // pitch - a smaller speaker rather than the same speaker talking lower.
    // Cast close to the target pitch and this stays near zero.
    float pitch = 0.0f;
    VoiceSpeech speech;
};

// Turns a line of dialogue into audio using the player's own installed voice
// models, on their machine. Nothing here reads or writes game data; it is handed
// an already-decoded string.
//
// Construction is cheap; the model is loaded on the first line that needs it and
// then kept, because loading costs about a second and every later line is free.
class VoiceSynth {
  public:
    // Returns nullptr when the model cannot be found or loaded, which is not an
    // error worth stopping for: the line simply stays silent.
    [[nodiscard("a failed load returns nullptr rather than a silent stub")]] static std::unique_ptr<VoiceSynth>
    Create(const std::string& modelPath, const std::string& espeakVoice);

    // Mono float at SampleRate(). Empty when the text produces no phonemes.
    [[nodiscard("synthesis is the whole point of the call")]] std::vector<float> Render(const std::string& text,
                                                                                        const VoiceProfile& profile);

    [[nodiscard("the rate is needed to interpret the samples")]] int32_t SampleRate() const noexcept {
        return mSampleRate;
    }

    ~VoiceSynth();
    VoiceSynth(const VoiceSynth&) = delete;
    VoiceSynth& operator=(const VoiceSynth&) = delete;

  private:
    // One pass through the model. Render splits a line where a mark asks for a
    // pause, so a line is one call or several.
    [[nodiscard("synthesis is the whole point of the call")]] std::vector<float>
    RunModel(const std::string& phonemes, const VoiceProfile& profile, float lengthScale);

    struct Impl;
    explicit VoiceSynth(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> mImpl;
    int32_t mSampleRate = 22050;
};

// Trims the lead-in and tail the model leaves, then brings the level to a
// consistent target. Replaces what ffmpeg's silenceremove and loudnorm did.
void VoiceTrimSilence(std::vector<float>& samples, float threshold = 0.003f);
void VoiceNormalise(std::vector<float>& samples, float targetRms = 0.126f);

// Pitch shift by resampling and then restoring the duration with overlap-add.
// Formants follow the pitch; see the note on VoiceProfile::pitch.
void VoicePitchShift(std::vector<float>& samples, int32_t rate, float semitones);

// Writes mono float as an Opus file the existing clip loader can read back.
[[nodiscard("callers must know whether the cache was written")]] bool
VoiceWriteOpus(const std::string& path, const std::vector<float>& samples, int32_t rate, int32_t bitrate = 32000);

} // namespace SOH
