#pragma once
#include <cstdint>
#include <string>

namespace SOH {

// ---------------------------------------------------------------------------
// Per-(fontId, instId) sample-name registry, populated by the SoundFont
// resource factory as it loads each font. The factory already reads the
// sample filename from the binary asset to resolve the underlying
// LoadResourceProcess call; we just capture the same string here so the
// bypass UI can show "Title Theme — Piano (Mid)" instead of a generic
// per-font label.
//
// Storage is a hashmap keyed by (fontId, instId); ~38 fonts × ≤16
// instruments × short strings sit well under 10 KB. Both Set and Get take
// the mutex; reads happen on the GUI thread and writes happen during
// resource loading (any thread), so the lock keeps it sound.
//
// Sample filenames typically arrive with a directory prefix (e.g.
// "audio/samples/Strings/Strings 1"); StripSamplePathPrefix returns just
// the basename for display.
// ---------------------------------------------------------------------------

// Range identifier for the three sample variants each instrument can carry.
// Engine picks per note pitch (low for very low notes, high for very high,
// normal for the middle range — many instruments only populate one or two).
enum class SampleRange : uint8_t { Low = 0, Normal = 1, High = 2 };

// Record one sample filename for an (instrument slot, range). Pass the raw
// string the factory loaded (with directory prefix) — the helper strips it
// on display. Empty `name` clears any prior entry for that range.
void SetInstrumentSampleName(uint8_t fontId, int16_t instId, SampleRange range,
                             std::string name);

// Returns the best available sample name for the slot:
//   1. Normal range if registered
//   2. otherwise Low if registered
//   3. otherwise High if registered
//   4. otherwise empty string
// This matches the UI's "show me what plays for typical notes" intent.
const std::string& GetInstrumentSampleName(uint8_t fontId, int16_t instId);

// Snapshot of all three range names for an instrument slot. Empty
// strings mean "no sample registered for that range." The bypass UI
// uses this to disambiguate slots where the engine plays different
// samples per pitch (e.g. low note = horse sample, normal note = ocarina
// sample) — without it the user only sees the normal-range name and gets
// confused by audible mismatches.
struct InstrumentSampleSet {
    std::string low;
    std::string normal;
    std::string high;
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
