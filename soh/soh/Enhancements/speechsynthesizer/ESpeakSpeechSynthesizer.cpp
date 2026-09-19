#include <algorithm>

#include <spdlog/spdlog.h>

#include "ESpeakSpeechSynthesizer.h"
#include "soh/Enhancements/audio/VoicePlayer.h"
#include "soh/Enhancements/tts/EspeakRuntime.h"

namespace {
// espeak's synth callback takes no user data, so what it fills has to be reachable
// without an instance. Only the worker thread touches these.
std::vector<short> gUtterance;
std::atomic<uint32_t>* gGeneration = nullptr;
uint32_t gSpeaking = 0;
} // namespace

ESpeakSpeechSynthesizer::ESpeakSpeechSynthesizer() {
}

int ESpeakSpeechSynthesizer::Collect(short* wav, int samples, espeak_EVENT* events) {
    // Non-zero abandons the rest of the utterance, which is how a new one cuts in.
    if (gGeneration != nullptr && gGeneration->load(std::memory_order_relaxed) != gSpeaking) {
        return 1;
    }
    if (wav != nullptr && samples > 0) {
        gUtterance.insert(gUtterance.end(), wav, wav + samples);
    }
    return 0;
}

bool ESpeakSpeechSynthesizer::DoInit() {
    this->mReady = SOH::Espeak::Ready();
    if (!this->mReady) {
        SPDLOG_INFO("Failed to initialize espeak-ng");
        return true;
    }

    gGeneration = &this->mGeneration;
    espeak_SetSynthCallback(&ESpeakSpeechSynthesizer::Collect);
    this->mWorker = std::thread(&ESpeakSpeechSynthesizer::Work, this);
    return true;
}

void ESpeakSpeechSynthesizer::DoUninitialize() {
    if (!this->mReady) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(this->mQueue);
        this->mQuit = true;
    }
    this->mGeneration.fetch_add(1, std::memory_order_relaxed);
    this->mWake.notify_one();
    this->mWorker.join();

    // The library is shared with the voice renderer, which may still be phonemising a
    // line on its own thread, so this drops the reader's use of it and nothing more.
    this->mReady = false;
    gGeneration = nullptr;
}

void ESpeakSpeechSynthesizer::Speak(const char* text, const char* language) {
    if (!this->mReady) {
        SPDLOG_INFO("Spoken Text ({}): {}", language, text);
        return;
    }

    // Match the SAPI and AVSpeechSynthesizer backends, which both drop what they are saying when a new
    // utterance arrives. Without this, scrolling a menu queues one utterance per item with no way to skip.
    this->mGeneration.fetch_add(1, std::memory_order_relaxed);
    SOH::VoicePlayer::Instance().Stop(SOH::VoicePlayer::Channel::Reader);

    {
        std::lock_guard<std::mutex> guard(this->mQueue);
        this->mText = text;
        this->mLanguage = language;
        this->mHasJob = true;
    }
    this->mWake.notify_one();
}

void ESpeakSpeechSynthesizer::Work() {
    for (;;) {
        std::string text;
        std::string language;
        {
            std::unique_lock<std::mutex> guard(this->mQueue);
            this->mWake.wait(guard, [this]() { return this->mHasJob || this->mQuit; });
            if (this->mQuit) {
                return;
            }
            text = std::move(this->mText);
            language = std::move(this->mLanguage);
            this->mHasJob = false;
        }

        gUtterance.clear();
        gSpeaking = this->mGeneration.load(std::memory_order_relaxed);
        {
            // espeak holds one voice and one translator for the process, so the voice
            // renderer has to be kept out for as long as this utterance is running.
            std::lock_guard<std::mutex> guard(SOH::Espeak::Lock());
            // Set every time rather than only on a change: the voice renderer shares this
            // library and leaves its own voice selected behind it.
            espeak_VOICE voice = { .languages = language.c_str() };
            if (espeak_SetVoiceByProperties(&voice) != EE_OK) {
                continue;
            }
            espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0, espeakCHARS_UTF8, nullptr, nullptr);
        }

        if (gSpeaking != this->mGeneration.load(std::memory_order_relaxed)) {
            continue;
        }
        std::shared_ptr<SOH::VoiceClip> clip =
            SOH::VoiceClip::FromPcm(gUtterance.data(), gUtterance.size(), SOH::Espeak::SampleRate());
        if (clip != nullptr) {
            SOH::VoicePlayer::Instance().Play(SOH::VoicePlayer::Channel::Reader, std::move(clip), 1.0f);
        }
    }
}

void ESpeakSpeechSynthesizer::DoApplySettings(int32_t rate, int32_t volume, int32_t pitch) {
    if (!this->mReady) {
        return;
    }

    const std::lock_guard<std::mutex> guard(SOH::Espeak::Lock());
    espeak_SetParameter(espeakRATE, std::clamp(espeakRATE_NORMAL * rate / 100, espeakRATE_MINIMUM, espeakRATE_MAXIMUM),
                        0);
    espeak_SetParameter(espeakVOLUME, volume, 0);
    espeak_SetParameter(espeakPITCH, pitch, 0);
}
