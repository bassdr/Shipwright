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
};

struct State {
    std::mutex mutex;
    // Keyed by (fontId, instId) directly. The map is small (~38 fonts ×
    // ≤16 instruments × a few short strings — well under 10 KB), so
    // ordered map's O(log n) cost is invisible and we skip writing a hash
    // specialisation for std::pair.
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
