#pragma once
#include <cstdint>
#include <string>

namespace SOH {

// Per-(fontId, instId) sample-name registry, populated by the SoundFont resource
// factory as it loads each font. Lets the bypass UI show a real sample name
// rather than a generic per-font label. Set runs during resource loading (any
// thread), Get on the GUI thread, so both take the mutex.

// Range identifier for the three sample variants each instrument can carry. The
// engine picks one per note pitch (low for very low notes, high for very high,
// normal for the middle range); many instruments only populate one or two.
enum class SampleRange : uint8_t { Low = 0, Normal = 1, High = 2 };

// The audio engine exposes a melodic instrument as instOrWave = instrument-array
// index + 2; instOrWave 0 and 1 are the drum and SFX channels (see
// AudioSeq_SetInstrument and MidiTranslator::kDrumHistInst). Every consumer keys
// by instOrWave, so the factory must register under (array index + this base).
inline constexpr int16_t kMelodicInstOrWaveBase = 2;

// Record one sample filename for an (instrument slot, range). Pass the raw
// string the factory loaded (with directory prefix) — the helper strips it
// on display. Empty `name` clears any prior entry for that range.
void SetInstrumentSampleName(uint8_t fontId, int16_t instId, SampleRange range, std::string name);

// Record the engine's low/normal/high split boundaries for an instrument slot
// (in engine-semitone space): semitone < lo -> low sample, lo..hi -> normal,
// semitone > hi -> high. Lets the bypass UI mirror the engine's per-pitch
// sample routing.
void SetInstrumentRange(uint8_t fontId, int16_t instId, uint8_t normalRangeLo, uint8_t normalRangeHi);

// Snapshot of all three range names for an instrument slot; empty strings mean no
// sample is registered for that range. Lets the bypass UI disambiguate slots that
// play a different sample per pitch.
struct InstrumentSampleSet {
    std::string low;
    std::string normal;
    std::string high;
    // Engine split boundaries (see SetInstrumentRange). hasRange is false when the
    // factory never registered this slot; callers then fall back to a single
    // full-range entry.
    uint8_t rangeLo = 0;
    uint8_t rangeHi = 127;
    bool hasRange = false;
    bool empty() const {
        return low.empty() && normal.empty() && high.empty();
    }
};
InstrumentSampleSet GetInstrumentSampleNames(uint8_t fontId, int16_t instId);

// Convenience: extract the basename from a stored sample path. Returns
// the input unchanged if it has no '/' separator. The returned pointer
// references inside `path` — do not outlive it.
const char* StripSamplePathPrefix(const std::string& path);

} // namespace SOH
