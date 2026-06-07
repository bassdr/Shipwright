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

// The audio engine exposes a melodic instrument to the sequence player (and
// thus to MidiTranslator and the bypass UI) as instOrWave = soundfont
// instrument-array index + 2: instOrWave 0 and 1 are the drum and SFX
// "channels" (see AudioSeq_SetInstrument and MidiTranslator::kDrumHistInst).
// Every consumer of this registry keys by instOrWave, so the SoundFont factory
// must register names/ranges under (array index + this base). Registering by
// the raw array index instead shifts every entry two slots — music rows then
// show the wrong instrument, the top two go blank, and the drum/SFX rows pick
// up melodic-instrument names.
inline constexpr int16_t kMelodicInstOrWaveBase = 2;

// Record one sample filename for an (instrument slot, range). Pass the raw
// string the factory loaded (with directory prefix) — the helper strips it
// on display. Empty `name` clears any prior entry for that range.
void SetInstrumentSampleName(uint8_t fontId, int16_t instId, SampleRange range,
                             std::string name);

// Record the engine's low/normal/high split boundaries for an instrument
// slot (Instrument::normalRangeLo / normalRangeHi, in engine-semitone space):
// semitone < lo -> low sample, lo..hi -> normal, semitone > hi -> high. The
// SoundFont factory captures these alongside the sample names so the bypass
// UI's "auto-split by engine ranges" can mirror the engine's per-pitch sample
// routing without touching the live audio state.
void SetInstrumentRange(uint8_t fontId, int16_t instId, uint8_t normalRangeLo, uint8_t normalRangeHi);

// Snapshot of all three range names for an instrument slot. Empty
// strings mean "no sample registered for that range." The bypass UI
// uses this to disambiguate slots where the engine plays different
// samples per pitch (e.g. low note = horse sample, normal note = ocarina
// sample) — without it the user only sees one and gets confused by
// audible mismatches.
struct InstrumentSampleSet {
    std::string low;
    std::string normal;
    std::string high;
    // Engine split boundaries (see SetInstrumentRange). hasRange is false when
    // the factory never registered this slot (e.g. a mod font not yet capturing
    // ranges); callers fall back to a single full-range entry then.
    uint8_t rangeLo = 0;
    uint8_t rangeHi = 127;
    bool    hasRange = false;
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
