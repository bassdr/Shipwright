#include "VoiceSynth.h"

#include "EspeakRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>

#include <spdlog/spdlog.h>

#include <ogg/ogg.h>
#include <onnxruntime_cxx_api.h>
#include <opus.h>

extern "C" {
#include <algorithm>
#include <cstring>
#include <thread>
#include <espeak-ng/speak_lib.h>
}

namespace SOH {

namespace {

// The model's phoneme table is keyed on single codepoints, so espeak's UTF-8
// output has to be walked as codepoints rather than bytes.
[[nodiscard]] std::vector<char32_t> Utf8ToCodepoints(const std::string& text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
        char32_t cp = length == 1 ? lead : static_cast<char32_t>(lead & (0xFF >> (length + 1)));
        for (int k = 1; k < length && i + k < text.size(); k++) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += length;
    }
    return out;
}

// Only the phoneme table is needed from the model's sidecar, and it is a flat
// map of one-codepoint keys to a single id, so a targeted scan beats pulling in
// a JSON parser for the rest of the file.
[[nodiscard]] std::map<char32_t, int64_t> ReadPhonemeTable(const std::string& configPath, int32_t& sampleRate) {
    std::map<char32_t, int64_t> table;
    std::ifstream file(configPath);
    if (!file.is_open()) {
        return table;
    }
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    const size_t rateKey = json.find("\"sample_rate\"");
    if (rateKey != std::string::npos) {
        const size_t colon = json.find(':', rateKey);
        sampleRate = std::atoi(json.c_str() + colon + 1);
    }

    size_t start = json.find("\"phoneme_id_map\"");
    if (start == std::string::npos) {
        return table;
    }
    start = json.find('{', start);
    size_t end = start;
    for (int depth = 0; end < json.size(); end++) {
        if (json[end] == '{') {
            depth++;
        } else if (json[end] == '}' && --depth == 0) {
            break;
        }
    }
    const std::string body = json.substr(start, end - start + 1);
    for (size_t i = 0; i + 1 < body.size();) {
        const size_t keyStart = body.find('"', i);
        if (keyStart == std::string::npos) {
            break;
        }
        const size_t keyEnd = body.find('"', keyStart + 1);
        const size_t open = body.find('[', keyEnd);
        const size_t close = body.find(']', open);
        if (keyEnd == std::string::npos || open == std::string::npos || close == std::string::npos) {
            break;
        }
        const std::vector<char32_t> key = Utf8ToCodepoints(body.substr(keyStart + 1, keyEnd - keyStart - 1));
        if (!key.empty()) {
            table[key[0]] = std::atoll(body.c_str() + open + 1);
        }
        i = close + 1;
    }
    return table;
}

void PutU16(std::vector<unsigned char>& out, const uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>(v >> 8));
}

void PutU32(std::vector<unsigned char>& out, const uint32_t v) {
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
}

// Wraps encoded packets in the Ogg container the clip loader reads. The two
// header packets are what make it an Opus file rather than a bare bitstream.
[[nodiscard]] bool WriteOggOpus(const std::string& path, const std::vector<std::vector<unsigned char>>& packets,
                                const uint32_t frames) {
    constexpr uint16_t kPreSkip = 312; // what libopus reports at 48 kHz

    std::vector<unsigned char> head;
    const char* magic = "OpusHead";
    head.insert(head.end(), magic, magic + 8);
    head.push_back(1);
    head.push_back(1);
    PutU16(head, kPreSkip);
    PutU32(head, 48000);
    PutU16(head, 0);
    head.push_back(0);

    std::vector<unsigned char> tags;
    const char* tagMagic = "OpusTags";
    tags.insert(tags.end(), tagMagic, tagMagic + 8);
    const std::string vendor = "soh";
    PutU32(tags, static_cast<uint32_t>(vendor.size()));
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    PutU32(tags, 0);

    ogg_stream_state stream;
    if (ogg_stream_init(&stream, 0x50484F53) != 0) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        ogg_stream_clear(&stream);
        return false;
    }
    const auto flush = [&file, &stream](const bool pageOut) {
        ogg_page page;
        while (pageOut ? ogg_stream_pageout(&stream, &page) : ogg_stream_flush(&stream, &page)) {
            file.write(reinterpret_cast<const char*>(page.header), page.header_len);
            file.write(reinterpret_cast<const char*>(page.body), page.body_len);
        }
    };

    ogg_packet packet{};
    packet.packet = head.data();
    packet.bytes = static_cast<long>(head.size());
    packet.b_o_s = 1;
    packet.granulepos = 0;
    packet.packetno = 0;
    ogg_stream_packetin(&stream, &packet);
    flush(false);

    packet = {};
    packet.packet = tags.data();
    packet.bytes = static_cast<long>(tags.size());
    packet.granulepos = 0;
    packet.packetno = 1;
    ogg_stream_packetin(&stream, &packet);
    flush(false);

    int64_t granule = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        granule += 960;
        packet = {};
        packet.packet = const_cast<unsigned char*>(packets[i].data());
        packet.bytes = static_cast<long>(packets[i].size());
        packet.e_o_s = (i + 1 == packets.size()) ? 1 : 0;
        packet.granulepos = std::min<int64_t>(granule, static_cast<int64_t>(frames) + kPreSkip);
        packet.packetno = static_cast<long>(i + 2);
        ogg_stream_packetin(&stream, &packet);
        flush(true);
    }
    flush(false);
    ogg_stream_clear(&stream);
    return true;
}

} // namespace

struct VoiceSynth::Impl {
    Ort::Env env{ ORT_LOGGING_LEVEL_ERROR, "soh-voice" };
    std::unique_ptr<Ort::Session> session;
    std::map<char32_t, int64_t> phonemes;
    std::string espeakVoice;
    bool hasSpeaker = false;
};

VoiceSynth::VoiceSynth(std::unique_ptr<Impl> impl) noexcept : mImpl(std::move(impl)) {
}

VoiceSynth::~VoiceSynth() = default;

std::unique_ptr<VoiceSynth> VoiceSynth::Create(const std::string& modelPath, const std::string& espeakVoice) {
    auto impl = std::make_unique<Impl>();
    int32_t rate = 22050;
    impl->phonemes = ReadPhonemeTable(modelPath + ".json", rate);
    if (impl->phonemes.empty()) {
        SPDLOG_WARN("Voice model {} has no phoneme table beside it", modelPath);
        return nullptr;
    }
    impl->espeakVoice = espeakVoice;

    try {
        Ort::SessionOptions options;
        // Measured on a twelve-word line: 204 ms on one thread, 122 on two, 91 on
        // four and nothing past that. A quarter of the box is the game's to spare.
        const unsigned int cores = std::thread::hardware_concurrency();
        options.SetIntraOpNumThreads(static_cast<int>(std::clamp(cores / 4U, 1U, 4U)));
        options.SetInterOpNumThreads(1);
#ifdef _WIN32
        const std::wstring wide(modelPath.begin(), modelPath.end());
        impl->session = std::make_unique<Ort::Session>(impl->env, wide.c_str(), options);
#else
        impl->session = std::make_unique<Ort::Session>(impl->env, modelPath.c_str(), options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < impl->session->GetInputCount(); i++) {
            const auto name = impl->session->GetInputNameAllocated(i, allocator);
            impl->hasSpeaker = impl->hasSpeaker || std::strcmp(name.get(), "sid") == 0;
        }
    } catch (const Ort::Exception& e) {
        SPDLOG_WARN("Voice model {} failed to load: {}", modelPath, e.what());
        return nullptr;
    }

    if (!Espeak::Ready()) {
        SPDLOG_INFO("Voice synthesis needs espeak-ng for phonemes; {} stays silent", modelPath);
        return nullptr;
    }

    std::unique_ptr<VoiceSynth> synth(new VoiceSynth(std::move(impl)));
    synth->mSampleRate = rate;
    return synth;
}

namespace {

// One stretch of speech and the mark that closed it. A line becomes several of
// these only where a mark asks for a pause, so an ordinary sentence is still a
// single pass through the model.
struct Segment {
    std::string phonemes;
    VoiceMark mark;
};

// espeak announces a change of language inside the phoneme string, and the model
// happily reads the announcement: "(", "e", "n" and ")" are all real phonemes to
// it. Respell the word in voice-speech.json to stop the switch; drop it here.
void StripLanguageMarkers(std::string& phonemes) {
    size_t open = phonemes.find('(');
    while (open != std::string::npos) {
        const size_t close = phonemes.find(')', open);
        if (close == std::string::npos) {
            phonemes.erase(open);
            return;
        }
        phonemes.erase(open, close - open + 1);
        open = phonemes.find('(', open);
    }
}

[[nodiscard]] std::string ApplyReplacements(const std::string& text,
                                            const std::vector<std::pair<std::string, std::string>>& rules) {
    std::string out = text;
    for (const auto& [from, to] : rules) {
        if (from.empty()) {
            continue;
        }
        size_t at = out.find(from);
        while (at != std::string::npos) {
            out.replace(at, from.size(), to);
            at = out.find(from, at + to.size());
        }
    }
    return out;
}

void AppendSpeech(const std::string& spoken, const VoiceProfile& profile, std::vector<Segment>& segments) {
    const void* cursor = spoken.c_str();
    bool markPending = false;
    while (cursor != nullptr) {
        // Each call consumes one clause and advances the cursor; a single call
        // would silently stop at the first full stop.
        const char* before = static_cast<const char*>(cursor);
        const char* clause = espeak_TextToPhonemes(&cursor, espeakCHARS_UTF8, 0x02);
        const char* stop = cursor != nullptr ? static_cast<const char*>(cursor) : before + std::strlen(before);
        if (cursor != nullptr && stop <= before) {
            break;
        }
        if (clause != nullptr && *clause != '\0') {
            std::string spelled(clause);
            StripLanguageMarkers(spelled);
            segments.back().phonemes += ApplyReplacements(spelled, profile.speech.phonemes);
            markPending = false;
        }

        // espeak reports phonemes only, so the mark that ended the clause never
        // reaches the model and every line comes out flat. The model was trained
        // with these, and its table has an id for each, so put it back.
        char terminator = '\0';
        size_t i = static_cast<size_t>(stop - before);
        for (; i > 0 && terminator == '\0'; i--) {
            const char c = before[i - 1];
            if (c != '\0' && std::strchr(".,!?;:", c) != nullptr) {
                terminator = c;
            }
        }
        if (terminator == '\0' || markPending) {
            // A row of dots is one pause, not one per dot: a second mark measured
            // 10 ms longer than the first, so only the first is worth emitting.
            segments.back().phonemes += ' ';
            continue;
        }
        markPending = true;

        // The run is measured in the line rather than in this clause: espeak hands
        // back a row of dots a few at a time, and how many there are is the point.
        size_t runFrom = static_cast<size_t>(before - spoken.c_str()) + i;
        size_t runTo = runFrom + 1;
        while (runFrom > 0 && spoken.at(runFrom - 1) == terminator) {
            runFrom--;
        }
        while (runTo < spoken.size() && spoken.at(runTo) == terminator) {
            runTo++;
        }

        VoiceMark mark{ std::string(1, terminator), 0.0f, 1.0f, 0.0f, 1.0f };
        for (size_t length = runTo - runFrom; length > 0; length--) {
            const auto tuned = profile.speech.marks.find(std::string(length, terminator));
            if (tuned != profile.speech.marks.end()) {
                mark = tuned->second;
                break;
            }
        }
        segments.back().phonemes += mark.emit;
        segments.back().phonemes += ' ';
        if (mark.pause > 0.0f || mark.emphasis != 1.0f || mark.pitch != 0.0f || mark.lengthScale != 1.0f) {
            segments.back().mark = mark;
            segments.push_back(Segment{});
        }
    }
}
} // namespace

std::vector<float> VoiceSynth::Render(const std::string& text, const VoiceProfile& profile) {
    std::vector<float> audio;
    if (mImpl->session == nullptr || !Espeak::Ready()) {
        return audio;
    }

    std::string spoken = ApplyReplacements(text, profile.speech.say);
    for (const std::string& silent : profile.speech.unspoken) {
        for (size_t at = spoken.find(silent); at != std::string::npos && !silent.empty();
             at = spoken.find(silent, at)) {
            // The mark that ended it goes too, or the line opens on a bare
            // exclamation with nothing in front of it.
            size_t after = at + silent.size();
            while (after < spoken.size() && std::strchr(" !?.,;:", spoken.at(after)) != nullptr) {
                after++;
            }
            spoken.erase(at, after - at);
        }
    }
    std::vector<Segment> segments{ Segment{} };
    {
        // The voice stays selected for as long as espeak is translating this line, and
        // the screen reader shares that selection, so both stay under the one lock.
        const std::lock_guard<std::mutex> guard(Espeak::Lock());
        espeak_SetVoiceByName(mImpl->espeakVoice.c_str());
        AppendSpeech(spoken, profile, segments);
    }

    for (const Segment& segment : segments) {
        std::vector<float> part = RunModel(segment.phonemes, profile, segment.mark.lengthScale);
        if (part.empty()) {
            continue;
        }
        // The model pads its own silence around a segment, so the gap between two
        // of them would otherwise be that plus the pause asked for.
        VoiceTrimSilence(part);
        if (segment.mark.pitch != 0.0f) {
            VoicePitchShift(part, mSampleRate, segment.mark.pitch);
            VoiceTrimSilence(part);
        }
        if (segment.mark.emphasis != 1.0f) {
            for (float& sample : part) {
                sample *= segment.mark.emphasis;
            }
        }
        audio.insert(audio.end(), part.begin(), part.end());
        const size_t silence = static_cast<size_t>(segment.mark.pause * static_cast<float>(mSampleRate));
        audio.insert(audio.end(), silence, 0.0f);
    }
    return audio;
}

std::vector<float> VoiceSynth::RunModel(const std::string& phonemes, const VoiceProfile& profile,
                                        const float lengthScale) {
    std::vector<float> audio;

    // Piper's encoding: begin, blank, then every phoneme trailed by a blank, end.
    constexpr int64_t kBegin = 1, kEnd = 2, kBlank = 0;
    std::vector<int64_t> tokens{ kBegin, kBlank };
    for (const char32_t cp : Utf8ToCodepoints(phonemes)) {
        const auto found = mImpl->phonemes.find(cp);
        if (found == mImpl->phonemes.end()) {
            continue;
        }
        tokens.push_back(found->second);
        tokens.push_back(kBlank);
    }
    tokens.push_back(kEnd);
    if (tokens.size() <= 3) {
        return audio;
    }

    try {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> lengths{ static_cast<int64_t>(tokens.size()) };
        std::vector<float> scales{ profile.noiseScale, profile.lengthScale * lengthScale, profile.noiseW };
        std::vector<int64_t> speaker{ profile.speaker };
        const std::vector<int64_t> tokenShape{ 1, static_cast<int64_t>(tokens.size()) };
        const std::vector<int64_t> one{ 1 }, three{ 3 };

        std::vector<const char*> inputNames{ "input", "input_lengths", "scales" };
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, tokens.data(), tokens.size(), tokenShape.data(), 2));
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, lengths.data(), 1, one.data(), 1));
        inputs.push_back(Ort::Value::CreateTensor<float>(memory, scales.data(), 3, three.data(), 1));
        // A single-speaker model has no speaker input at all, and handing it one
        // fails the whole call rather than being ignored.
        if (mImpl->hasSpeaker) {
            inputNames.push_back("sid");
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, speaker.data(), 1, one.data(), 1));
        }

        const char* outputNames[] = { "output" };
        auto outputs = mImpl->session->Run(Ort::RunOptions{ nullptr }, inputNames.data(), inputs.data(), inputs.size(),
                                           outputNames, 1);

        const float* samples = outputs[0].GetTensorData<float>();
        const size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        audio.assign(samples, samples + count);
    } catch (const Ort::Exception& e) {
        SPDLOG_WARN("Voice synthesis failed: {}", e.what());
        audio.clear();
    }
    return audio;
}

void VoiceTrimSilence(std::vector<float>& samples, const float threshold) {
    if (samples.empty()) {
        return;
    }
    size_t first = 0;
    while (first < samples.size() && std::fabs(samples[first]) < threshold) {
        first++;
    }
    size_t last = samples.size();
    while (last > first && std::fabs(samples[last - 1]) < threshold) {
        last--;
    }
    if (first >= last) {
        samples.clear();
        return;
    }
    samples.erase(samples.begin() + static_cast<std::ptrdiff_t>(last), samples.end());
    samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(first));
}

void VoiceNormalise(std::vector<float>& samples, const float targetRms) {
    if (samples.empty()) {
        return;
    }
    double sum = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * s;
    }
    const double rms = std::sqrt(sum / static_cast<double>(samples.size()));
    if (rms < 1e-6) {
        return;
    }
    float gain = static_cast<float>(targetRms / rms);
    // Leave headroom rather than clipping the peaks a quiet line would need.
    float peak = 0.0f;
    for (const float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    if (peak * gain > 0.99f) {
        gain = 0.99f / peak;
    }
    for (float& s : samples) {
        s *= gain;
    }
}

void VoicePitchShift(std::vector<float>& samples, const int32_t rate, const float semitones) {
    if (samples.empty() || std::fabs(semitones) < 0.01f) {
        return;
    }
    const double ratio = std::pow(2.0, semitones / 12.0);

    // Resample first: this moves pitch and formants together and changes the
    // duration by the same factor.
    std::vector<float> resampled;
    resampled.reserve(static_cast<size_t>(samples.size() / ratio) + 1);
    for (double pos = 0.0; pos < static_cast<double>(samples.size()) - 1.0; pos += ratio) {
        const size_t i = static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i));
        resampled.push_back(samples[i] * (1.0f - frac) + samples[i + 1] * frac);
    }

    // Then put the duration back by overlap-adding at the original rate. Windows
    // are aligned on the best correlation in a small search range so successive
    // grains stay in phase, which is what stops the shift sounding metallic.
    const size_t window = static_cast<size_t>(rate * 0.040);
    const size_t half = window / 2;
    const size_t search = static_cast<size_t>(rate * 0.005);
    const size_t target = samples.size();
    if (resampled.size() < window * 2 || target < window * 2) {
        samples = std::move(resampled);
        return;
    }

    std::vector<float> out(target, 0.0f);
    std::vector<float> weight(target, 0.0f);
    const double step = static_cast<double>(resampled.size() - window) / static_cast<double>(target - window);
    size_t writePos = 0;
    double readPos = 0.0;
    while (writePos + window < target && readPos + window < static_cast<double>(resampled.size())) {
        size_t best = static_cast<size_t>(readPos);
        if (writePos > 0 && best > search) {
            double bestScore = -1e30;
            const size_t from = best - search;
            const size_t to = std::min(best + search, resampled.size() - window - 1);
            for (size_t candidate = from; candidate <= to; candidate++) {
                double score = 0.0;
                for (size_t k = 0; k < half; k += 4) {
                    score += static_cast<double>(out[writePos + k]) * resampled[candidate + k];
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = candidate;
                }
            }
        }
        for (size_t k = 0; k < window; k++) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * static_cast<float>(k) /
                                                   static_cast<float>(window - 1));
            out[writePos + k] += resampled[best + k] * w;
            weight[writePos + k] += w;
        }
        writePos += half;
        readPos += step * static_cast<double>(half);
    }
    for (size_t i = 0; i < out.size(); i++) {
        if (weight[i] > 1e-4f) {
            out[i] /= weight[i];
        }
    }
    samples = std::move(out);
}

bool VoiceWriteOpus(const std::string& path, const std::vector<float>& samples, const int32_t rate,
                    const int32_t bitrate) {
    if (samples.empty()) {
        return false;
    }
    // Opus only encodes a handful of rates; 48k is the one it works at natively
    // and the one the clip player already expects.
    constexpr int32_t kOpusRate = 48000;
    std::vector<float> resampled;
    const double ratio = static_cast<double>(rate) / kOpusRate;
    resampled.reserve(static_cast<size_t>(samples.size() / ratio) + 1);
    for (double pos = 0.0; pos < static_cast<double>(samples.size()) - 1.0; pos += ratio) {
        const size_t i = static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i));
        resampled.push_back(samples[i] * (1.0f - frac) + samples[i + 1] * frac);
    }

    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(kOpusRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (encoder == nullptr || error != OPUS_OK) {
        SPDLOG_WARN("Could not create the Opus encoder for {}", path);
        return false;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));

    // A bare Opus packet stream is not a file; the loader reads Ogg, so each
    // packet is wrapped in an Ogg page below.
    constexpr int kFrame = 960; // 20 ms at 48 kHz
    std::vector<std::vector<unsigned char>> packets;
    std::vector<unsigned char> buffer(4000);
    for (size_t at = 0; at + kFrame <= resampled.size(); at += kFrame) {
        const int bytes =
            opus_encode_float(encoder, resampled.data() + at, kFrame, buffer.data(), static_cast<int>(buffer.size()));
        if (bytes < 0) {
            opus_encoder_destroy(encoder);
            return false;
        }
        packets.emplace_back(buffer.begin(), buffer.begin() + bytes);
    }
    opus_encoder_destroy(encoder);
    return WriteOggOpus(path, packets, static_cast<uint32_t>(resampled.size()));
}

} // namespace SOH
