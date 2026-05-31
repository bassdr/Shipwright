#include "InstrumentNames.h"
#include <mutex>
#include <unordered_map>

namespace SOH {

namespace {
// One slot may carry up to three sample variants (low / normal / high).
// Storing them together keeps lookups single-keyed and the data
// cache-friendly.
struct SlotSamples {
    std::string low;
    std::string normal;
    std::string high;
};

struct State {
    std::mutex mutex;
    // Key layout: (fontId << 16) | (instId & 0xFFFF). instId is int16_t in
    // the public API; we cast through uint16_t to keep negative sentinels
    // distinguishable (though Set/Get reject instId < 0 up front).
    std::unordered_map<uint32_t, SlotSamples> entries;
};

State& GetState() {
    static State s;
    return s;
}

inline uint32_t MakeKey(uint8_t fontId, int16_t instId) {
    return (static_cast<uint32_t>(fontId) << 16) |
           static_cast<uint32_t>(static_cast<uint16_t>(instId));
}
} // namespace

void SetInstrumentSampleName(uint8_t fontId, int16_t instId, SampleRange range,
                             std::string name) {
    if (instId < 0) {
        return;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& slot = s.entries[MakeKey(fontId, instId)];
    switch (range) {
        case SampleRange::Low:    slot.low    = std::move(name); break;
        case SampleRange::Normal: slot.normal = std::move(name); break;
        case SampleRange::High:   slot.high   = std::move(name); break;
    }
}

const std::string& GetInstrumentSampleName(uint8_t fontId, int16_t instId) {
    static const std::string kEmpty;
    if (instId < 0) {
        return kEmpty;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.entries.find(MakeKey(fontId, instId));
    if (it == s.entries.end()) {
        return kEmpty;
    }
    // Prefer normal — the typical middle-pitch sample — then fall back to
    // whichever range was populated. Plays nicely with instruments that
    // only define low+high or only normal.
    const SlotSamples& slot = it->second;
    if (!slot.normal.empty()) return slot.normal;
    if (!slot.low.empty())    return slot.low;
    return slot.high; // may be empty too — that's fine
}

InstrumentSampleSet GetInstrumentSampleNames(uint8_t fontId, int16_t instId) {
    InstrumentSampleSet out;
    if (instId < 0) {
        return out;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.entries.find(MakeKey(fontId, instId));
    if (it == s.entries.end()) {
        return out;
    }
    out.low    = it->second.low;
    out.normal = it->second.normal;
    out.high   = it->second.high;
    return out;
}

const char* StripSamplePathPrefix(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return path.c_str();
    }
    return path.c_str() + slash + 1;
}

} // namespace SOH
