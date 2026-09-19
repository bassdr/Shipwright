#include "VoicePlayer.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <opusfile.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace SOH {

namespace {

template <typename Tag> std::shared_ptr<VoiceClip> DecodeAll(OggOpusFile* file, Tag tag) {
    std::vector<float> samples;
    const ogg_int64_t totalFrames = op_pcm_total(file, -1);
    if (totalFrames > 0) {
        samples.reserve(size_t(totalFrames) * VoiceClip::kChannels);
    }

    // op_read_float_stereo downmixes or duplicates as needed, so a mono clip and
    // a stereo one both land here as interleaved stereo.
    constexpr int kChunkFrames = 4096;
    float chunk[kChunkFrames * VoiceClip::kChannels];
    for (;;) {
        const int frames = op_read_float_stereo(file, chunk, kChunkFrames * int(VoiceClip::kChannels));
        if (frames < 0) {
            SPDLOG_WARN("Voice clip decode failed with {}", frames);
            return nullptr;
        }
        if (frames == 0) {
            break;
        }
        samples.insert(samples.end(), chunk, chunk + size_t(frames) * VoiceClip::kChannels);
    }

    if (samples.empty()) {
        return nullptr;
    }
    return std::make_shared<VoiceClip>(tag, std::move(samples));
}

// Catmull-Rom between the two frames either side of the cursor. Clip edges
// repeat rather than reading past the buffer.
float Interpolate(const float* src, size_t frameCount, int64_t frame, uint32_t channel, float t) {
    const auto at = [&](int64_t f) {
        const int64_t clamped = f < 0 ? 0 : (f >= int64_t(frameCount) ? int64_t(frameCount) - 1 : f);
        return src[size_t(clamped) * VoiceClip::kChannels + channel];
    };

    const float p0 = at(frame - 1);
    const float p1 = at(frame);
    const float p2 = at(frame + 1);
    const float p3 = at(frame + 2);

    return p1 +
           0.5f * t * ((p2 - p0) + t * ((2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) + t * (3.0f * (p1 - p2) + p3 - p0)));
}

} // namespace

VoiceClip::VoiceClip(PrivateTag, std::vector<float>&& samples) noexcept : mSamples(std::move(samples)) {
}

std::shared_ptr<VoiceClip> VoiceClip::FromOpusMemory(const void* data, size_t size) {
    int error = 0;
    OggOpusFile* file = op_open_memory(static_cast<const unsigned char*>(data), size, &error);
    if (file == nullptr) {
        SPDLOG_WARN("Voice clip is not a readable Opus stream (error {})", error);
        return nullptr;
    }
    std::shared_ptr<VoiceClip> clip = DecodeAll(file, PrivateTag{});
    op_free(file);
    return clip;
}

std::shared_ptr<VoiceClip> VoiceClip::FromPcm(const int16_t* samples, size_t count, int32_t rate) {
    if (samples == nullptr || count == 0 || rate <= 0) {
        return nullptr;
    }

    const double step = double(rate) / double(VoiceClip::kRate);
    const auto frames = size_t(double(count) / step);
    std::vector<float> stereo(frames * VoiceClip::kChannels);
    for (size_t f = 0; f < frames; f++) {
        const double at = double(f) * step;
        const auto index = size_t(at);
        const auto t = float(at - double(index));
        const float a = float(samples[index]) / 32768.0f;
        const float b = float(samples[index + 1 < count ? index + 1 : index]) / 32768.0f;
        const float value = a + (b - a) * t;
        stereo.at(f * VoiceClip::kChannels) = value;
        stereo.at(f * VoiceClip::kChannels + 1) = value;
    }
    if (stereo.empty()) {
        return nullptr;
    }
    return std::make_shared<VoiceClip>(PrivateTag{}, std::move(stereo));
}

std::shared_ptr<VoiceClip> VoiceClip::FromOpusFile(const std::string& path) {
    int error = 0;
    OggOpusFile* file = op_open_file(path.c_str(), &error);
    if (file == nullptr) {
        SPDLOG_WARN("Could not open voice clip {} (error {})", path, error);
        return nullptr;
    }
    std::shared_ptr<VoiceClip> clip = DecodeAll(file, PrivateTag{});
    op_free(file);
    return clip;
}

VoicePlayer& VoicePlayer::Instance() {
    static VoicePlayer instance;
    return instance;
}

void VoicePlayer::Voice::Play(std::shared_ptr<VoiceClip> clip, float clipGain) {
    if (clip == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(handoff);
    retired.reset();
    pending = std::move(clip);
    pendingGain = clipGain;
    hasPending = true;
    active.store(true, std::memory_order_relaxed);
}

void VoicePlayer::Voice::Stop() {
    std::lock_guard<std::mutex> guard(handoff);
    retired.reset();
    pending.reset();
    pendingGain = 1.0f;
    hasPending = true;
    active.store(false, std::memory_order_relaxed);
}

void VoicePlayer::Voice::Mix(float* bus, uint32_t frames, int32_t outRate) {
    // A missed pickup costs one batch of latency, which is cheaper than letting
    // the game thread stall the mix while it hands a clip over.
    if (handoff.try_lock()) {
        if (hasPending) {
            retired = std::move(current);
            current = std::move(pending);
            gain = pendingGain;
            pending.reset();
            hasPending = false;
            cursor = 0.0;
        }
        handoff.unlock();
    }

    if (current == nullptr || outRate <= 0) {
        return;
    }

    const std::vector<float>& samples = current->Samples();
    const float* src = samples.data();
    const size_t frameCount = samples.size() / VoiceClip::kChannels;
    const double step = double(VoiceClip::kRate) / double(outRate);

    double at = cursor;
    for (uint32_t f = 0; f < frames && at < double(frameCount); f++) {
        const auto frame = int64_t(at);
        const auto t = float(at - double(frame));
        for (uint32_t c = 0; c < VoiceClip::kChannels; c++) {
            bus[f * VoiceClip::kChannels + c] += Interpolate(src, frameCount, frame, c, t) * gain;
        }
        at += step;
    }
    cursor = at;

    if (at >= double(frameCount)) {
        active.store(false, std::memory_order_relaxed);
    }
}

void VoicePlayer::Play(Channel channel, std::shared_ptr<VoiceClip> clip, float gain) {
    mVoices.at(size_t(channel)).Play(std::move(clip), gain);
}

void VoicePlayer::Stop(Channel channel) {
    mVoices.at(size_t(channel)).Stop();
}

bool VoicePlayer::IsPlaying() const noexcept {
    if (mSeparate.load(std::memory_order_relaxed)) {
        return false;
    }
    for (const Voice& voice : mVoices) {
        if (voice.active.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool VoicePlayer::IsSeparate() const noexcept {
    return mSeparate.load(std::memory_order_relaxed);
}

void VoicePlayer::MixAll(float* bus, uint32_t frames, int32_t outRate) {
    for (Voice& voice : mVoices) {
        voice.Mix(bus, frames, outRate);
    }
}

void VoicePlayer::Mix(float* bus, uint32_t frames, int32_t outRate) {
    if (mSeparate.load(std::memory_order_relaxed)) {
        return;
    }
    // Held only against the instant a routing change is switching threads over.
    if (mMixing.try_lock()) {
        MixAll(bus, frames, outRate);
        mMixing.unlock();
    }
}

void VoicePlayer::FeedStream(void* self, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0) {
        return;
    }

    constexpr int kFrameBytes = int(VoiceClip::kChannels) * int(sizeof(float));
    std::vector<float> block(size_t(additional / kFrameBytes) * VoiceClip::kChannels, 0.0f);
    auto& player = *static_cast<VoicePlayer*>(self);
    if (player.mMixing.try_lock()) {
        player.MixAll(block.data(), uint32_t(additional / kFrameBytes), VoiceClip::kRate);
        player.mMixing.unlock();
    }
    SDL_PutAudioStreamData(stream, block.data(), int(block.size() * sizeof(float)));
}

void VoicePlayer::SetSeparateOutput(bool separate, const std::string& device) {
    if (separate && mStream != nullptr && device == mDevice) {
        return;
    }

    if (mStream != nullptr) {
        // Ordered so the callback cannot be mixing while its stream goes away.
        SDL_AudioStream* stream = mStream;
        mStream = nullptr;
        mSeparate.store(false, std::memory_order_relaxed);
        SDL_DestroyAudioStream(stream);
    }
    if (!separate) {
        return;
    }

    // Refcounted, and the game's own audio may not have brought this up yet.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SPDLOG_WARN("Speech could not start its own output ({}); it stays in the game mix", SDL_GetError());
        return;
    }

    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!device.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; ids != nullptr && i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name != nullptr && device == name) {
                id = ids[i];
            }
        }
        SDL_free(ids);
        if (id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
            SPDLOG_WARN("Speech output device \"{}\" is gone; using the default one", device);
        }
    }

    const SDL_AudioSpec spec{ SDL_AUDIO_F32, int(VoiceClip::kChannels), VoiceClip::kRate };
    mStream = SDL_OpenAudioDeviceStream(id, &spec, FeedStream, this);
    if (mStream == nullptr) {
        SPDLOG_WARN("Speech could not open its own output ({}); it stays in the game mix", SDL_GetError());
        return;
    }
    mDevice = device;
    SDL_ResumeAudioStreamDevice(mStream);
    mSeparate.store(true, std::memory_order_relaxed);
}

} // namespace SOH
