#include "EspeakRuntime.h"

#include <SDL3/SDL_filesystem.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <espeak-ng/speak_lib.h>
}

namespace SOH::Espeak {

namespace {

// espeak never owns an audio device: it synthesises on the thread that asks and hands the
// samples back, which is what lets the game mix speech with everything else and keeps the
// library's global state off a thread of its own.
constexpr espeak_AUDIO_OUTPUT kMode = AUDIO_OUTPUT_SYNCHRONOUS;

int gSampleRate = 0;

[[nodiscard]] bool Start() {
    // The data is built and shipped beside the binary, so espeak is pointed at it rather
    // than left to search the install prefixes of a system copy that need not exist.
    const char* base = SDL_GetBasePath();
    gSampleRate = espeak_Initialize(kMode, 0, base, 0);
    if (gSampleRate < 0) {
        SPDLOG_WARN("espeak-ng failed to initialise from {}; spoken text stays silent",
                    base != nullptr ? base : "the default path");
        return false;
    }
    return true;
}

} // namespace

bool Ready() {
    static const bool ready = Start();
    return ready;
}

int SampleRate() {
    return Ready() ? gSampleRate : 0;
}

std::mutex& Lock() noexcept {
    static std::mutex lock;
    return lock;
}

} // namespace SOH::Espeak
