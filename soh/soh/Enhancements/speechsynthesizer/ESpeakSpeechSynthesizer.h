#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SpeechSynthesizer.h"

extern "C" {
#include <espeak-ng/speak_lib.h>
}

class ESpeakSpeechSynthesizer : public SpeechSynthesizer {
  public:
    ESpeakSpeechSynthesizer();

    void Speak(const char* text, const char* language);

  protected:
    bool DoInit(void);
    void DoUninitialize(void);
    void DoApplySettings(int32_t rate, int32_t volume, int32_t pitch);

  private:
    // espeak synthesises on the thread that asks and blocks until the whole
    // utterance is done, which is far too long to hold the game thread.
    void Work();
    // The callback espeak hands samples to, on the worker thread.
    static int Collect(short* wav, int samples, espeak_EVENT* events);

    bool mReady = false;

    std::thread mWorker;
    std::mutex mQueue;
    std::condition_variable mWake;
    std::string mText;
    std::string mLanguage;
    bool mHasJob = false;
    bool mQuit = false;
    // Bumped by every new utterance, so one already being synthesised can see
    // that nobody is waiting for it any more and stop.
    std::atomic<uint32_t> mGeneration{ 0 };
};
