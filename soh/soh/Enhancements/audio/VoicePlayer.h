#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SDL_AudioStream;

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

    // Mono 16-bit at the synthesiser's own rate, which is what espeak hands back.
    [[nodiscard("an empty utterance returns nullptr rather than a silent clip")]] static std::shared_ptr<VoiceClip>
    FromPcm(const int16_t* samples, size_t count, int32_t rate);

    const std::vector<float>& Samples() const noexcept {
        return mSamples;
    }

    // Reachable only from the factories above, which hold the tag.
    VoiceClip(PrivateTag, std::vector<float>&& samples) noexcept;

  private:
    std::vector<float> mSamples;
};

// The speech bus: every spoken sound the game makes, whether it is an actor's
// line or the screen reader announcing a menu item. Both leave together, either
// mixed into the game's output or on a stream of their own, so a player who
// sends speech to a headset gets all of it there and keeps the music in the game.
//
// Play() and Stop() run on the game thread and hand a decoded clip over;
// Mix() runs on the audio thread and never blocks on either.
class VoicePlayer {
  public:
    // Kept apart so neither cuts the other off: a menu readout and a line of
    // dialogue are answers to different things the player did.
    enum class Channel { Acted, Reader };

    static VoicePlayer& Instance();

    // Replaces whatever that channel is playing, matching how the speech backends interrupt.
    void Play(Channel channel, std::shared_ptr<VoiceClip> clip, float gain);
    void Stop(Channel channel);

    // Cheap enough for the audio thread to gate its stock fast path on. False
    // while speech is on its own stream, where it adds nothing to the game bus.
    [[nodiscard("the answer decides whether the mix has to run at all")]] bool IsPlaying() const noexcept;

    // Audio thread: adds `frames` of interleaved stereo into `bus`, resampling
    // from the clip rate to `outRate`. Adds nothing when idle or routed away.
    void Mix(float* bus, uint32_t frames, int32_t outRate);

    // Opens or closes the bus's own output device, by name as the system reports
    // it; empty asks for the default. Game thread.
    //
    // Only a device the game is not already playing on becomes a stream of its own:
    // SDL folds everything opened on one device back into a single stream, which is
    // what the system then sees. On the game's own device the speech still escapes
    // the game's volume, but it cannot be routed away from the music.
    void SetSeparateOutput(bool separate, const std::string& device);
    [[nodiscard("the answer decides whether the game's volume applies")]] bool IsSeparate() const noexcept;

  private:
    VoicePlayer() = default;

    // One clip playing, one waiting to be picked up by whichever thread mixes.
    struct Voice {
        std::mutex handoff;
        std::shared_ptr<VoiceClip> pending;
        // Freeing the outgoing clip on the audio thread would put an allocator
        // call in the mix; the next Play() or Stop() drops it on the game thread.
        std::shared_ptr<VoiceClip> retired;
        float pendingGain = 1.0f;
        bool hasPending = false;

        // Mixing thread only, past the handoff.
        std::shared_ptr<VoiceClip> current;
        double cursor = 0.0;
        float gain = 1.0f;

        std::atomic<bool> active{ false };

        void Play(std::shared_ptr<VoiceClip> clip, float clipGain);
        void Stop();
        void Mix(float* bus, uint32_t frames, int32_t outRate);
    };

    // Shared by both mixing threads so a routing change cannot have them running
    // over the same cursors; only one of them is ever active for longer than that.
    std::mutex mMixing;
    std::array<Voice, 2> mVoices;
    std::atomic<bool> mSeparate{ false };
    SDL_AudioStream* mStream = nullptr;
    std::string mDevice;

    void MixAll(float* bus, uint32_t frames, int32_t outRate);
    static void FeedStream(void* self, SDL_AudioStream* stream, int additional, int total);
};

} // namespace SOH
