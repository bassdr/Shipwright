#include "VoicePlayer.h"

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

void VoicePlayer::Play(std::shared_ptr<VoiceClip> clip, float gain) {
    if (clip == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(mHandoff);
    mRetired.reset();
    mPending = std::move(clip);
    mPendingGain = gain;
    mHasPending = true;
    mActive.store(true, std::memory_order_relaxed);
}

void VoicePlayer::Stop() {
    std::lock_guard<std::mutex> guard(mHandoff);
    mRetired.reset();
    mPending.reset();
    mPendingGain = 1.0f;
    mHasPending = true;
    mActive.store(false, std::memory_order_relaxed);
}

bool VoicePlayer::IsPlaying() const noexcept {
    return mActive.load(std::memory_order_relaxed);
}

void VoicePlayer::Mix(float* bus, uint32_t frames, int32_t outRate) {
    // A missed pickup costs one batch of latency, which is cheaper than letting
    // the game thread stall the mix while it hands a clip over.
    if (mHandoff.try_lock()) {
        if (mHasPending) {
            mRetired = std::move(mCurrent);
            mCurrent = std::move(mPending);
            mGain = mPendingGain;
            mPending.reset();
            mHasPending = false;
            mCursor = 0.0;
        }
        mHandoff.unlock();
    }

    if (mCurrent == nullptr || outRate <= 0) {
        return;
    }

    const std::vector<float>& samples = mCurrent->Samples();
    const float* src = samples.data();
    const size_t frameCount = samples.size() / VoiceClip::kChannels;
    const double step = double(VoiceClip::kRate) / double(outRate);

    double cursor = mCursor;
    for (uint32_t f = 0; f < frames && cursor < double(frameCount); f++) {
        const auto frame = int64_t(cursor);
        const auto t = float(cursor - double(frame));
        for (uint32_t c = 0; c < VoiceClip::kChannels; c++) {
            bus[f * VoiceClip::kChannels + c] += Interpolate(src, frameCount, frame, c, t) * mGain;
        }
        cursor += step;
    }
    mCursor = cursor;

    if (cursor >= double(frameCount)) {
        mActive.store(false, std::memory_order_relaxed);
    }
}

} // namespace SOH
