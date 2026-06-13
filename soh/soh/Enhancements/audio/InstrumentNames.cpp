#include "InstrumentNames.h"
#include <map>
#include <mutex>
#include <utility>

namespace SOH {

namespace {
// One slot may carry up to three sample variants (low / normal / high).
// Storing them together keeps lookups single-keyed.
struct SlotSamples {
    std::string low;
    std::string normal;
    std::string high;
    uint8_t rangeLo = 0;
    uint8_t rangeHi = 127;
    bool    hasRange = false;
};

struct State {
    std::mutex mutex;
    // Keyed by (fontId, instId). Small enough that std::map's cost is irrelevant.
    std::map<std::pair<uint8_t, int16_t>, SlotSamples> entries;
};

State& GetState() {
    static State s;
    return s;
}
} // namespace

void SetInstrumentSampleName(uint8_t fontId, int16_t instId, SampleRange range,
                             std::string name) {
    if (instId < 0) {
        return;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& slot = s.entries[std::make_pair(fontId, instId)];
    switch (range) {
        case SampleRange::Low:    slot.low    = std::move(name); break;
        case SampleRange::Normal: slot.normal = std::move(name); break;
        case SampleRange::High:   slot.high   = std::move(name); break;
    }
}

void SetInstrumentRange(uint8_t fontId, int16_t instId, uint8_t normalRangeLo, uint8_t normalRangeHi) {
    if (instId < 0) {
        return;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& slot = s.entries[std::make_pair(fontId, instId)];
    slot.rangeLo = normalRangeLo;
    slot.rangeHi = normalRangeHi;
    slot.hasRange = true;
}

InstrumentSampleSet GetInstrumentSampleNames(uint8_t fontId, int16_t instId) {
    InstrumentSampleSet out;
    if (instId < 0) {
        return out;
    }
    auto& s = GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.entries.find(std::make_pair(fontId, instId));
    if (it == s.entries.end()) {
        return out;
    }
    out.low      = it->second.low;
    out.normal   = it->second.normal;
    out.high     = it->second.high;
    out.rangeLo  = it->second.rangeLo;
    out.rangeHi  = it->second.rangeHi;
    out.hasRange = it->second.hasRange;
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
