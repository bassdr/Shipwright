#include "VoiceBaker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <ship/core/Context.h>

#include "soh/OTRGlobals.h"
#include "soh/Enhancements/audio/VoicePlayer.h"
#include "soh/Enhancements/tts/VoiceClips.h"
#include "soh/Enhancements/tts/VoiceSynth.h"

namespace SOH {

namespace {

struct Job {
    std::string text;
    std::string language;
    std::string profile;
    float gain = 1.0f;
    // Loads the language's models and leaves; nothing is spoken and nothing is
    // written.
    bool warm = false;
};

// espeak needs a voice name; the model's language is the only place to get one
// that matches what the model was trained on.
[[nodiscard]] std::string EspeakVoiceFor(const std::string& language) {
    const size_t dash = language.find('-');
    return dash == std::string::npos ? language : language.substr(0, dash);
}

class Worker {
  public:
    static Worker& Instance() {
        static Worker worker;
        return worker;
    }

    void Push(Job job) {
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            const std::string key = job.profile + '/' + job.text;
            if (!mSeen.insert(key).second) {
                return;
            }
            mQueue.push_back(std::move(job));
        }
        Start();
        mWake.notify_one();
    }

    // A joinable std::thread destroyed at exit calls std::terminate, and this
    // worker is a static, so it has to stop itself.
    ~Worker() {
        Shutdown();
    }

    void Shutdown() {
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mWake.notify_all();
        if (mThread.joinable()) {
            mThread.join();
        }
    }

  private:
    void Start() {
        const std::lock_guard<std::mutex> lock(mStartMutex);
        if (!mStarted && !mStopping) {
            mStarted = true;
            mThread = std::thread([this]() { Run(); });
        }
    }

    // The profile file is the player's to edit, so it is read from the app
    // directory rather than baked in; a voice with no entry stays silent.
    [[nodiscard]] const nlohmann::json& Profiles() {
        if (!mProfilesLoaded) {
            mProfilesLoaded = true;
            const std::string path = Ship::Context::GetPathRelativeToAppDirectory("voice-profiles.json", appShortName);
            std::ifstream file(path);
            if (!file.is_open()) {
                SPDLOG_INFO("No voice-profiles.json at {}; voice acting stays silent", path);
                return mProfiles;
            }
            try {
                file >> mProfiles;
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_WARN("voice-profiles.json could not be read: {}", e.what());
                mProfiles = nlohmann::json::object();
            }
        }
        return mProfiles;
    }

    // Built in so the feature works with no file at all; voice-speech.json replaces
    // whichever marks it names and leaves the rest alone.
    [[nodiscard]] static VoiceSpeech DefaultSpeech(const std::string& language) {
        VoiceSpeech speech;
        speech.marks["."] = VoiceMark{ ".", 0.30f, 1.0f, 0.0f, 1.0f };
        speech.marks["?"] = VoiceMark{ "?", 0.30f, 1.0f, 0.0f, 1.0f };
        // A second and third mark is the line shouting louder, not just longer.
        speech.marks["!"] = VoiceMark{ "!", 0.30f, 1.15f, 0.0f, 0.95f };
        speech.marks["!!"] = VoiceMark{ "!", 0.30f, 1.25f, 0.0f, 0.92f };
        speech.marks["!!!"] = VoiceMark{ "!", 0.35f, 1.35f, 0.0f, 0.90f };
        speech.marks[","] = VoiceMark{ ",", 0.20f, 1.0f, 0.0f, 1.0f };
        speech.marks[";"] = VoiceMark{ ";", 0.20f, 1.0f, 0.0f, 1.0f };
        // A colon holds far longer than the line ever wants it to.
        speech.marks[":"] = VoiceMark{ ",", 0.20f, 1.0f, 0.0f, 1.0f };
        // A row of dots is the game writing a beat, and it is a longer one.
        speech.marks["..."] = VoiceMark{ ".", 0.60f, 1.0f, 0.0f, 1.0f };

        // Better silence than the alternative: read as words the model says "ah ah
        // ah", and built from a repeated burst it is an electronic fake. Neither is
        // a laugh, so the line is spoken without it.
        speech.unspoken = { "Wha ha ha ha", "Wha ha ha", "Ha ha ha", "Heh heh heh", "Ho ho ho" };

        if (language == "fr-FR") {
            // espeak reads this as English and switches language mid-line, which is
            // both the wrong accent and four junk phonemes from the marker it emits.
            speech.say = { { "Ding Dong", "digne dogne" } };
        }
        return speech;
    }

    [[nodiscard]] const VoiceSpeech& SpeechFor(const std::string& language) {
        const auto found = mSpeech.find(language);
        if (found != mSpeech.end()) {
            return found->second;
        }

        VoiceSpeech speech = DefaultSpeech(language);
        const std::string path = Ship::Context::GetPathRelativeToAppDirectory("voice-speech.json", appShortName);
        std::ifstream file(path);
        if (file.is_open()) {
            try {
                nlohmann::json all;
                file >> all;
                if (all.contains(language)) {
                    const nlohmann::json& entry = all[language];
                    // Longest first, so a rule for a phrase is not undone by a rule
                    // for one of its words.
                    const auto rules = [&entry](const char* key) {
                        std::vector<std::pair<std::string, std::string>> out;
                        // Named, not the result of value() inline: items() holds a
                        // reference, and the temporary dies before the loop reads it.
                        const nlohmann::json section = entry.value(key, nlohmann::json::object());
                        for (const auto& [from, to] : section.items()) {
                            out.emplace_back(from, to.template get<std::string>());
                        }
                        std::sort(out.begin(), out.end(),
                                  [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
                        return out;
                    };
                    speech.say = rules("say");
                    speech.phonemes = rules("phonemes");

                    const nlohmann::json unspoken = entry.value("unspoken", nlohmann::json::array());
                    if (!unspoken.empty()) {
                        speech.unspoken.clear();
                        for (const auto& spelling : unspoken) {
                            speech.unspoken.push_back(spelling.get<std::string>());
                        }
                        std::sort(speech.unspoken.begin(), speech.unspoken.end(),
                                  [](const auto& a, const auto& b) { return a.size() > b.size(); });
                    }

                    const nlohmann::json marks = entry.value("marks", nlohmann::json::object());
                    for (const auto& [name, tuning] : marks.items()) {
                        if (name.empty()) {
                            continue;
                        }
                        VoiceMark& mark = speech.marks[name];
                        // A run is given to the model as one mark: repeating it was
                        // measured at 10 ms more silence, so the pause carries it.
                        mark.emit = tuning.value("emit", std::string(1, name.at(0)));
                        mark.pause = tuning.value("pause", mark.pause);
                        mark.emphasis = tuning.value("emphasis", mark.emphasis);
                        mark.pitch = tuning.value("pitch", mark.pitch);
                        mark.lengthScale = tuning.value("length_scale", mark.lengthScale);
                    }
                }
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_WARN("voice-speech.json could not be read: {}", e.what());
                speech = DefaultSpeech(language);
            }
        }
        SPDLOG_INFO("voice tuning for {}: {} say rules, {} phoneme rules, {} marks, {} unspoken", language,
                    speech.say.size(), speech.phonemes.size(), speech.marks.size(), speech.unspoken.size());
        return mSpeech.emplace(language, std::move(speech)).first->second;
    }

    [[nodiscard]] VoiceSynth* SynthFor(const std::string& model, const std::string& language) {
        const auto found = mSynths.find(model);
        if (found != mSynths.end()) {
            return found->second.get();
        }
        const std::string path =
            Ship::Context::GetPathRelativeToAppDirectory("voice-models/" + model + ".onnx", appShortName);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            SPDLOG_INFO("Voice model {} is not installed at {}", model, path);
            mSynths.emplace(model, nullptr);
            return nullptr;
        }
        auto synth = VoiceSynth::Create(path, EspeakVoiceFor(language));
        VoiceSynth* raw = synth.get();
        mSynths.emplace(model, std::move(synth));
        return raw;
    }

    // One short line through each model: loading it is most of the cost, but the
    // first inference is where ONNX Runtime allocates its arenas.
    void WarmModels(const std::string& language) {
        std::set<std::string> models;
        for (const auto& [name, languages] : Profiles().items()) {
            if (!languages.is_object() || !languages.contains(language)) {
                continue;
            }
            const std::string model = languages[language].value("model", std::string());
            if (!model.empty()) {
                models.insert(model);
            }
        }
        for (const std::string& model : models) {
            VoiceSynth* synth = SynthFor(model, language);
            if (synth != nullptr) {
                VoiceProfile settings;
                settings.model = model;
                const std::vector<float> discarded = synth->Render("ready", settings);
                SPDLOG_INFO("voice model {} warmed, {} samples", model, discarded.size());
            }
        }
    }

    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [this]() { return mStopping || !mQueue.empty(); });
                if (mStopping && mQueue.empty()) {
                    return;
                }
                job = std::move(mQueue.front());
                mQueue.pop_front();
            }

            const nlohmann::json& profiles = Profiles();
            if (job.warm) {
                WarmModels(job.language);
                continue;
            }
            if (!profiles.contains(job.profile) || !profiles[job.profile].contains(job.language)) {
                continue;
            }
            const nlohmann::json& entry = profiles[job.profile][job.language];

            VoiceProfile settings;
            settings.model = entry.value("model", std::string());
            settings.speaker = entry.value("speaker", 0);
            settings.lengthScale = entry.value("length_scale", 1.0f);
            settings.noiseScale = entry.value("noise_scale", 0.667f);
            settings.pitch = entry.value("pitch", 0.0f);
            settings.speech = SpeechFor(job.language);
            if (settings.model.empty()) {
                continue;
            }

            VoiceSynth* synth = SynthFor(settings.model, job.language);
            if (synth == nullptr) {
                continue;
            }

            // A line the player is waiting on is worth timing: the wait before the
            // first word is the whole of this loop, and which step owns it is not
            // guessable from the outside.
            const auto startedAt = std::chrono::steady_clock::now();
            std::vector<float> audio = synth->Render(job.text, settings);
            if (audio.empty()) {
                continue;
            }
            const auto renderedAt = std::chrono::steady_clock::now();

            // Shift first: the overlap-add leaves a tail that only a trim
            // afterwards removes, and normalising last keeps the level honest.
            VoicePitchShift(audio, synth->SampleRate(), settings.pitch);
            VoiceTrimSilence(audio);
            VoiceNormalise(audio);
            const auto shapedAt = std::chrono::steady_clock::now();

            const std::string directory = VoiceClipDirectory(job.language, job.profile);
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const std::string path = VoiceClipPath(job.text, job.language, job.profile);
            if (!VoiceWriteOpus(path, audio, synth->SampleRate())) {
                continue;
            }
            const auto encodedAt = std::chrono::steady_clock::now();

            // The textbox is still up: play it now rather than making the player
            // read the line again to hear it.
            std::shared_ptr<VoiceClip> clip = VoiceClip::FromOpusFile(path);
            if (clip != nullptr) {
                VoicePlayer::Instance().Play(VoicePlayer::Channel::Acted, clip, job.gain);
            }
            const auto playedAt = std::chrono::steady_clock::now();

            const auto ms = [](auto from, auto to) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
            };
            SPDLOG_INFO("voice {}: {:.1f}s of audio in {} ms (render {}, shape {}, encode {}, decode {})", job.profile,
                        static_cast<float>(audio.size()) / static_cast<float>(synth->SampleRate()),
                        ms(startedAt, playedAt), ms(startedAt, renderedAt), ms(renderedAt, shapedAt),
                        ms(shapedAt, encodedAt), ms(encodedAt, playedAt));
        }
    }

    std::mutex mMutex;
    std::mutex mStartMutex;
    std::condition_variable mWake;
    std::deque<Job> mQueue;
    std::set<std::string> mSeen;
    std::thread mThread;
    bool mStarted = false;
    bool mStopping = false;

    // Worker thread only.
    std::map<std::string, std::unique_ptr<VoiceSynth>> mSynths;
    nlohmann::json mProfiles = nlohmann::json::object();
    std::map<std::string, VoiceSpeech> mSpeech;
    bool mProfilesLoaded = false;
};

} // namespace

VoiceBaker& VoiceBaker::Instance() {
    static VoiceBaker baker;
    return baker;
}

void VoiceBaker::Request(const std::string& text, const std::string& language, const std::string& profile,
                         const float gain) {
    Worker::Instance().Push(Job{ text, language, profile, gain });
}

void VoiceBaker::Warm(const std::string& language) {
    Worker::Instance().Push(Job{ std::string(), language, std::string(), 1.0f, true });
}

void VoiceBaker::Shutdown() {
    Worker::Instance().Shutdown();
}

} // namespace SOH
