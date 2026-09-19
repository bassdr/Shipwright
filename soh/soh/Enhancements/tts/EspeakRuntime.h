#pragma once

#include <mutex>

namespace SOH::Espeak {

// espeak-ng keeps one voice, one output mode and one translator for the whole process.
// Everything that talks to it comes through here, so it is initialised exactly once and
// a second caller cannot reconfigure the first.
[[nodiscard("a false answer means espeak is unusable and calling it will not work")]] bool Ready();

// Zero until the library is up. espeak decides its own rate, and what it hands back has
// to be resampled to whatever the game is playing at.
[[nodiscard("a zero rate means espeak is unavailable")]] int SampleRate();

// Translation and synthesis both run on the thread that calls in, against that shared
// state, so the voice renderer's worker has to be held apart from the screen reader.
[[nodiscard("the lock is the point of calling this")]] std::mutex& Lock() noexcept;

} // namespace SOH::Espeak
