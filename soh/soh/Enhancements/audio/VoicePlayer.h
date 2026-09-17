#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SOH {

// A decoded voice clip, held as interleaved stereo float at the Opus rate.
// Opus always decodes at 48 kHz, so the clip is rate-agnostic and survives a
// change of output device rate; VoicePlayer resamples as it mixes.
class VoiceClip {
    struct PrivateTag {};

  public:
    static constexpr int32_t kRate = 48000;
    static constexpr uint32_t kChannels = 2;

    // Returns nullptr when the bytes are not a readable Opus stream.
    [[nodiscard("a failed decode returns nullptr rather than an empty clip")]] static std::shared_ptr<VoiceClip>
    FromOpusMemory(const void* data, size_t size);
    [[nodiscard("a failed decode returns nullptr rather than an empty clip")]] static std::shared_ptr<VoiceClip>
    FromOpusFile(const std::string& path);

    const std::vector<float>& Samples() const noexcept {
        return mSamples;
    }

    // Reachable only from the factories above, which hold the tag.
    VoiceClip(PrivateTag, std::vector<float>&& samples) noexcept;

  private:
    std::vector<float> mSamples;
};

// Mixes spoken clips into the game bus, so they land on the same device and
// under the same master volume as everything else the game plays. This is
// deliberately not where the accessibility speech goes: the speech backends
// open their own output stream, which is what lets a screen-reader user route
// speech to a different device than the game.
//
// Play() and Stop() run on the game thread and hand a decoded clip over;
// Mix() runs on the audio thread and never blocks on either.
class VoicePlayer {
  public:
    static VoicePlayer& Instance();

    // Replaces whatever is playing, matching how the speech backends interrupt.
    void Play(std::shared_ptr<VoiceClip> clip, float gain);
    void Stop();

    // Cheap enough for the audio thread to gate its stock fast path on.
    bool IsPlaying() const noexcept;

    // Audio thread: adds `frames` of interleaved stereo into `bus`, resampling
    // from the clip rate to `outRate`. Adds nothing when idle.
    void Mix(float* bus, uint32_t frames, int32_t outRate);

  private:
    VoicePlayer() = default;

    std::mutex mHandoff;
    std::shared_ptr<VoiceClip> mPending;
    // Freeing the outgoing clip on the audio thread would put an allocator call
    // in the mix; the next Play() or Stop() drops it on the game thread instead.
    std::shared_ptr<VoiceClip> mRetired;
    float mPendingGain = 1.0f;
    bool mHasPending = false;

    // Audio thread only, past the handoff.
    std::shared_ptr<VoiceClip> mCurrent;
    double mCursor = 0.0;
    float mGain = 1.0f;

    std::atomic<bool> mActive{ false };
};

} // namespace SOH
