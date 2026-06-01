#include "AudioEditor.h"
#include "sequence.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <libultraship/libultraship.h>
#include <functions.h>
#include "soh/ShipUtils.h"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include <ship/utils/StringHelper.h>
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/SohGui.hpp"
#include "AudioCollection.h"
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/ShipUtils.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/randomizer/SeedContext.h"

#if ENABLE_FLUIDSYNTH
#include <ship/audio/MidiSynthManager.h>
#include <ship/audio/FluidSynth.h>
#include "MidiTranslator.h"
#include "GmInstrumentMap.h"
#include "DefaultFluidSynthOverrides.h"
#include "InstrumentNames.h"
#include <ship/resource/archive/ArchiveManager.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>
extern "C" void SOH_MidiTranslator_Reset();

namespace {
// Synth packs come from two sources, stacked at apply time in this order:
//   1. Mod-supplied   — resources under audio/synth/<pack>/ inside any
//                       mounted .o2r archive (the existing path).
//   2. Loose          — bare .sf2 files dropped into <config-dir>/synth-packs/,
//                       with an optional sibling <basename>.json for the
//                       mapping overlay.
// Both sources allow an unlimited number of packs; FluidSynth's preset
// lookup walks loaded SF2s in reverse load order, so the last enabled pack
// wins on (bank, program) collisions — same "last-loaded wins" semantics as
// the wider mod stack.
constexpr const char* kSynthPackRoot          = "audio/synth";
constexpr const char* kSynthPackSf2Name       = "soundfont.sf2";
constexpr const char* kSynthPackJsonName      = "mapping.json";
constexpr const char* kLooseSynthPacksDirName = "synth-packs";

struct SynthPackEntry {
    enum class Source { Archive, Loose };
    std::string name;          // display name + key used in the disabled-set CSV
    Source      source;
    // Archive: virtual resource paths inside the archive.
    // Loose:   absolute filesystem paths.
    std::string sf2Path;
    std::string mappingPath;   // empty if no mapping json is available
};

// One row per (sfontId, bank, program) tuple across every loaded SF2.
// Populated by ApplyFluidSynthFromCVars after the load loop, consumed by
// the bypass UI's Bank/Pack + Preset combos. Names come from the SF2's
// phdr chunk via fluid_preset_get_name (FluidSynth side) so the UI shows
// what the SF2 author called the preset, not just a numeric (bank, prog).
struct LoadedPresetEntry {
    int         sfontId;
    std::string packName;
    int         bank;
    int         program;
    std::string name;
};
static std::vector<LoadedPresetEntry> sLoadedPresets;

// Derived from sLoadedPresets — unique (sfontId, bank) tuples in load
// order. Drives the Bank/Pack combo. Kept in step with sLoadedPresets.
struct BankSelectorEntry {
    int         sfontId;
    std::string packName;
    int         bank;
};
static std::vector<BankSelectorEntry> sBankSelectors;

// Auto-save model: every UI edit that touches a persisted field commits
// immediately to fluidsynth_overrides.json. Combined with the translator's
// mUserModified set, this means in-memory state == on-disk state, so pack
// toggles (which call ReapplyOverrideChain → reset → re-overlay from disk)
// no longer lose the user's unsaved work.
//
// Drag/slider widgets use ImGui::IsItemDeactivatedAfterEdit() to fire one
// save per drag session (release) instead of once per frame; click-type
// widgets (radios, selectables, buttons) save inline.
void AutoSaveOverrides() {
    auto path = Ship::Context::GetPathRelativeToAppDirectory(
        "fluidsynth_overrides.json", appShortName);
    SOH::MidiTranslator::Instance().SaveOverridesToFile(path);
}

// Session-only Override-column state — never persisted.
// sSoloedPairs: every pair the user has soloed. Empty = no solo active.
//               When non-empty, every NON-soloed discovered pair is forced
//               muted via the translator. Multi-row solo: clicking Solo on
//               a second row adds it; both audible side-by-side.
// sExplicitMutedPairs: per-row mutes set by the Mute button on Native rows.
//                      (Synth rows use the temp-volume slider's 0.0 stop as
//                      their mute, so they don't enter this set.) Stacks
//                      with solo — an explicitly muted row stays silent even
//                      while soloed.
static std::set<std::pair<uint8_t, int16_t>> sSoloedPairs;
static std::set<std::pair<uint8_t, int16_t>> sExplicitMutedPairs;

// Returns every pack the user could enable: archive-supplied first (alpha
// sorted), then loose-folder SF2s (alpha sorted). Packs are not filtered
// by the disabled-set CVar here — callers decide whether to apply that
// filter (UI shows everything; apply path skips disabled rows).
std::vector<SynthPackEntry> EnumerateSynthPacks() {
    std::vector<SynthPackEntry> result;

    // ── Archive-supplied packs ───────────────────────────────────────
    auto archives = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    if (auto matches = archives->ListFiles(std::string(kSynthPackRoot) + "/*/" + kSynthPackSf2Name)) {
        const size_t prefixLen = std::strlen(kSynthPackRoot) + 1;
        const size_t suffixLen = std::strlen(kSynthPackSf2Name) + 1;
        std::vector<SynthPackEntry> arc;
        arc.reserve(matches->size());
        for (const auto& path : *matches) {
            if (path.size() <= prefixLen + suffixLen) continue;
            std::string name = path.substr(prefixLen, path.size() - prefixLen - suffixLen);
            SynthPackEntry e;
            e.name        = name;
            e.source      = SynthPackEntry::Source::Archive;
            e.sf2Path     = path;
            e.mappingPath = std::string(kSynthPackRoot) + "/" + name + "/" + kSynthPackJsonName;
            arc.push_back(std::move(e));
        }
        std::sort(arc.begin(), arc.end(),
                  [](const SynthPackEntry& a, const SynthPackEntry& b) { return a.name < b.name; });
        // Dedupe by name across multiple archives shipping the same pack —
        // ListFiles returns one entry per archive that contains the file,
        // and we only want one row in the UI.
        arc.erase(std::unique(arc.begin(), arc.end(),
                              [](const SynthPackEntry& a, const SynthPackEntry& b) {
                                  return a.name == b.name;
                              }),
                  arc.end());
        for (auto& e : arc) result.push_back(std::move(e));
    }

    // ── Loose folder ─────────────────────────────────────────────────
    // The folder is created lazily — its absence is the normal first-run
    // state and not an error. We never write to the folder; the user owns it.
    std::string looseDirStr = Ship::Context::GetPathRelativeToAppDirectory(
        kLooseSynthPacksDirName, appShortName);
    std::filesystem::path looseDir(looseDirStr);
    std::error_code ec;
    if (std::filesystem::is_directory(looseDir, ec)) {
        std::vector<SynthPackEntry> loose;
        for (const auto& entry : std::filesystem::directory_iterator(looseDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            auto ext = entry.path().extension().string();
            // Case-insensitive .sf2 match — Windows users often have .SF2 etc.
            std::string extLower = ext;
            std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extLower != ".sf2") continue;

            SynthPackEntry e;
            e.name    = entry.path().stem().string();
            e.source  = SynthPackEntry::Source::Loose;
            e.sf2Path = entry.path().string();
            std::filesystem::path jsonPath = entry.path();
            jsonPath.replace_extension(".json");
            if (std::filesystem::exists(jsonPath, ec)) {
                e.mappingPath = jsonPath.string();
            }
            loose.push_back(std::move(e));
        }
        std::sort(loose.begin(), loose.end(),
                  [](const SynthPackEntry& a, const SynthPackEntry& b) { return a.name < b.name; });
        for (auto& e : loose) result.push_back(std::move(e));
    }

    return result;
}

// ── Disabled-pack CVar (CSV, default empty = all discovered packs enabled) ──
//
// Stored as a plain comma-separated string of pack names — the names are
// user-controlled but practically alphanumeric, so a CSV is good enough and
// keeps the CVar layer simple (no JSON escaping). Empty entries are
// tolerated on parse so the file survives manual edits.

std::set<std::string> ParseDisabledPacksCSV() {
    std::set<std::string> result;
    std::string csv = CVarGetString(CVAR_AUDIO("FluidSynthDisabledPacks"), "");
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        std::string name = csv.substr(start, comma == std::string::npos ? std::string::npos
                                                                        : comma - start);
        if (!name.empty()) result.insert(std::move(name));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

void WriteDisabledPacksCSV(const std::set<std::string>& disabled) {
    std::string csv;
    for (const auto& n : disabled) {
        if (!csv.empty()) csv += ",";
        csv += n;
    }
    CVarSetString(CVAR_AUDIO("FluidSynthDisabledPacks"), csv.c_str());
}

bool IsPackDisabled(const std::string& name) {
    return ParseDisabledPacksCSV().count(name) > 0;
}

void SetPackDisabled(const std::string& name, bool disabled) {
    auto cur = ParseDisabledPacksCSV();
    if (disabled) cur.insert(name);
    else          cur.erase(name);
    WriteDisabledPacksCSV(cur);
}

// Read the SF2 / mapping bytes for a pack into memory. Archive packs go
// through ArchiveManager::LoadFile; loose packs are read straight off the
// filesystem. Returns an empty vector when the file is missing — the caller
// distinguishes "load failed" from "no mapping" by the *Path field of the
// entry (loose: jsonPath empty → no mapping; archive: probe the load).
std::vector<uint8_t> ReadPackFile(const SynthPackEntry& entry, bool wantSf2) {
    std::vector<uint8_t> bytes;
    const std::string& path = wantSf2 ? entry.sf2Path : entry.mappingPath;
    if (path.empty()) return bytes;

    if (entry.source == SynthPackEntry::Source::Archive) {
        auto archives = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
        auto file = archives->LoadFile(path);
        if (!file || !file->Buffer || file->Buffer->size() <= file->BufferOffset) {
            return bytes;
        }
        const uint8_t* data = reinterpret_cast<const uint8_t*>(file->Buffer->data()) + file->BufferOffset;
        const size_t   size = file->Buffer->size() - file->BufferOffset;
        bytes.assign(data, data + size);
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return bytes;
        in.seekg(0, std::ios::end);
        std::streamoff len = in.tellg();
        if (len <= 0) return bytes;
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<size_t>(len));
        in.read(reinterpret_cast<char*>(bytes.data()), len);
        if (!in) bytes.clear();
    }
    return bytes;
}

// Filter EnumerateSynthPacks() down to the rows the user has left enabled.
// Discovery order is preserved (mods first, loose second), which is also
// the SF2 load order — and therefore the priority order FluidSynth uses
// for collision resolution (last-loaded wins).
std::vector<SynthPackEntry> EnabledPacksInOrder() {
    auto all = EnumerateSynthPacks();
    auto disabled = ParseDisabledPacksCSV();
    std::vector<SynthPackEntry> out;
    out.reserve(all.size());
    for (auto& e : all) {
        if (!disabled.count(e.name)) out.push_back(std::move(e));
    }
    return out;
}

// Push the current SF2 stack + pack load order into the translator and
// recompute every entry's runtime sfontId. Called after every change
// that affects entry resolution: pack load/unload, file load, user pick.
//
// We forward sLoadedPresets through as the validator (an entry's
// (bank, program) tuple must exist in the matched SF2 for sfontId to be
// non-negative) and the packs vector as the load order tiebreak (last
// in the list wins when multiple enabled entries cover the same pair).
void RefreshEntryResolution(const std::vector<SynthPackEntry>& packs) {
    auto& tr = SOH::MidiTranslator::Instance();
    std::vector<std::string> order;
    order.reserve(packs.size());
    std::set<std::string> loaded;
    for (const auto& p : packs) {
        order.push_back(p.name);
        loaded.insert(p.name);
    }
    tr.SetPackLoadOrder(order);
    tr.RemoveModEntriesNotIn(loaded);

    std::vector<SOH::MidiTranslator::LoadedPresetRef> refs;
    refs.reserve(sLoadedPresets.size());
    for (const auto& lp : sLoadedPresets) {
        refs.push_back({ lp.sfontId, lp.packName, lp.bank, lp.program });
    }
    tr.RefreshEntrySfontIds(refs);
    tr.RecomputeAllActive();
}

// Common prefix used by ReapplyOverrideChain and ResetToPackBaseline:
// wipe in-memory state, then layer built-in defaults followed by each
// enabled pack's mapping.json (in the same order as the SF2 load).
// The user JSON layer (if any) is what the two callers differ on.
void ApplyBaselineOnly(const std::vector<SynthPackEntry>& packs) {
    SOH::MidiTranslator::Instance().ResetAllOverrides();
    SOH::MidiTranslator::Instance().ApplyOverridesFromString(SOH::kDefaultFluidSynthOverridesJson);

    for (const auto& pack : packs) {
        if (pack.mappingPath.empty()) continue; // SF2-only pack is valid
        auto bytes = ReadPackFile(pack, /*wantSf2=*/false);
        if (bytes.empty()) continue;
        std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        SOH::MidiTranslator::Instance().ApplyOverridesFromString(json);
    }
}

// Apply the override chain in precedence order:
//   1. Reset to factory state (Auto / 1.0× / -1 / 0)
//   2. Built-in defaults (currently empty; reserved for future curated set)
//   3. Each enabled pack's mapping.json — overlays defaults in load order
//   4. User's fluidsynth_overrides.json — wins over all the above
// Called at startup and whenever the enabled-pack set changes, so the
// live translator state always reflects the current source-of-truth chain.
void ReapplyOverrideChain(const std::vector<SynthPackEntry>& packs) {
    ApplyBaselineOnly(packs);
    SOH::MidiTranslator::Instance().ApplyOverridesFromFile(
        Ship::Context::GetPathRelativeToAppDirectory("fluidsynth_overrides.json", appShortName));
    // Entry storage and the pack load order just changed — re-derive
    // sfontIds and the active-entry cache so the audio thread sees a
    // consistent state on the next note.
    RefreshEntryResolution(packs);
}

void ReapplyOverrideChain() {
    ReapplyOverrideChain(EnabledPacksInOrder());
}

// Wipe the user's customisations from in-memory state and restore the
// pack-derived baseline (built-in defaults + every enabled pack's
// mapping.json). User's on-disk fluidsynth_overrides.json is not touched
// — the user has to click Save afterward to persist the cleared state.
void ResetToPackBaseline() {
    auto packs = EnabledPacksInOrder();
    ApplyBaselineOnly(packs);
    // ApplyBaselineOnly only touches the entry storage; we still need to
    // refresh sfontIds and the active-entry cache, otherwise the
    // freshly-loaded mod entries have sfontId=-1 and resolution finds
    // nothing.
    RefreshEntryResolution(packs);
}

// Status line surfaced by the FluidSynth tab so the user sees what's
// active right now and what (if anything) failed. Written by the apply /
// reconcile paths, read by the tab UI. Plain-string status keeps the
// state model trivial; isError just toggles the colour.
struct PipelineStatus {
    std::string message;
    bool        isError = false;
};
static PipelineStatus sLastStatus;

void SetStatus(std::string msg, bool isError = false) {
    sLastStatus.message = std::move(msg);
    sLastStatus.isError = isError;
}

// Apply the current Modern audio pipeline + Synth pack configuration.
// Always switches the AudioPlayer into float mode. Installs FluidSynth
// and stacks every enabled pack's SF2 when at least one pack is enabled;
// with none enabled, the audio thread runs the native engine through the
// float path (no timbre change).
//
// Returns true if the requested configuration was applied. A false
// return is surfaced by callers as the ModernAudioPipeline checkbox
// flipping back off so the UI stays honest about what's active.
bool ApplyFluidSynthFromCVars() {
    auto audioPlayer = Ship::Context::GetRawInstance()->GetAudio()->GetAudioPlayer();
    if (!audioPlayer) {
        SPDLOG_INFO("[AudioEditor] Float audio: audio player not ready, skipping apply");
        SetStatus("Audio player not ready.", true);
        return false;
    }
    // Float pipeline always on when this code runs — the checkbox is the
    // gate. The AudioPlayer reinits the backend; on failure we bail.
    if (!audioPlayer->SetUseFloatPipeline(true)) {
        SPDLOG_ERROR("[AudioEditor] Float audio: AudioPlayer refused float mode, aborting apply");
        SetStatus("Audio backend refused float mode.", true);
        return false;
    }
    OTRAudio_SetFloatPipeline(true);

    auto packs = EnabledPacksInOrder();

    // Atomic rebuild — drop the prior synth + mix-source FIRST so the
    // audio thread sees silence in between rather than transient state
    // from the old synth overlapping the new one. The old shared_ptr's
    // destructor runs synchronously once the manager and mix-source both
    // release it, which the two SetSynth(nullptr)+SetMixSource(nullptr)
    // calls below guarantee before we construct the replacement.
    //
    // This matters most when toggling pack checkboxes: without the
    // explicit teardown, the old FluidSynth could briefly remain reachable
    // via the AudioPlayer's mix-source callback while the new one is
    // already loading SF2s, and confusion about "which synth loaded what
    // in what order" becomes possible. With it, load order is exactly
    // packs-vector order, every time.
    audioPlayer->SetMixSource(nullptr);
    Ship::MidiSynthManager::Instance().SetSynth(nullptr);

    if (packs.empty()) {
        // Float-only mode: native engine through the float path, no SF2 layer.
        SOH_MidiTranslator_Reset();
        sLoadedPresets.clear();
        sBankSelectors.clear();
        SPDLOG_INFO("[AudioEditor] Float audio: pipeline active (no synth packs enabled)");
        SetStatus("Modern audio pipeline active. No synth packs enabled.");
        return true;
    }

    // Log the intended load order — visible in the spdlog stream when
    // the user reports "this toggle didn't load what I expected". The
    // last entry has highest priority in FluidSynth's reverse-load-order
    // preset lookup.
    {
        std::string orderLog;
        for (const auto& p : packs) {
            if (!orderLog.empty()) orderLog += " -> ";
            orderLog += p.name;
        }
        SPDLOG_INFO("[AudioEditor] FluidSynth: pack load order (last wins): {}", orderLog);
    }

    // Run FluidSynth at the device's output rate. The float audio pipeline
    // mixes the synth contribution into AudioPlayer::Play *after* the
    // resampler, so the synth output skips the rate-conversion step entirely
    // and we get whatever native quality FluidSynth produces at the device
    // rate (typically 48 kHz). The native engine still runs at the source
    // rate (32 kHz) and is resampled up to meet the synth at the mix step.
    double sampleRate = static_cast<double>(audioPlayer->GetSampleRate());

    // Mode-driven configuration. Authentic = Graham-Smith modulators + console-era
    // reverb (per docs/Reproducing_Console_OSTs_accurately.md). Enhanced = stock
    // SF2 modulators + a subtle reverb that lets the SF2's musical interpretation
    // breathe through. Translator branches its NoteOn / CC11 routing on the same mode.
    auto mode = static_cast<SOH::SynthMode>(CVarGetInteger(CVAR_AUDIO("FluidSynthMode"), 0));
    bool grahamSmith = (mode == SOH::SynthMode::Authentic);

    auto synth = std::make_shared<Ship::FluidSynth>(sampleRate, grahamSmith);

    // Stack every enabled pack's SF2 in discovery order. FluidSynth walks
    // loaded sfonts in reverse on preset lookup, so the LAST loaded pack
    // wins on (bank, program) collisions — matches the mod stack precedence.
    size_t      totalBytes  = 0;
    size_t      loadedPacks = 0;
    std::string firstFailure;
    std::unordered_map<int, std::string> idToPackName;
    for (const auto& pack : packs) {
        auto bytes = ReadPackFile(pack, /*wantSf2=*/true);
        if (bytes.empty()) {
            SPDLOG_ERROR("[AudioEditor] FluidSynth: pack '{}' has no readable soundfont.sf2",
                         pack.name);
            if (firstFailure.empty()) firstFailure = pack.name;
            continue;
        }
        int id = synth->AddSoundFontFromMemory(bytes.data(), bytes.size());
        if (id < 0) {
            SPDLOG_ERROR("[AudioEditor] FluidSynth: pack '{}' SF2 rejected by FluidSynth", pack.name);
            if (firstFailure.empty()) firstFailure = pack.name;
            continue;
        }
        idToPackName[id] = pack.name;
        totalBytes += bytes.size();
        loadedPacks++;
        SPDLOG_INFO("[AudioEditor] FluidSynth: loaded SF2 from pack '{}' ({} bytes, id={})",
                    pack.name, bytes.size(), id);
    }

    if (loadedPacks == 0) {
        SetStatus(firstFailure.empty()
                      ? std::string("No enabled synth pack provided a readable SF2.")
                      : std::string("Pack '" + firstFailure + "' has no readable soundfont.sf2."),
                  true);
        return false;
    }

    // Refresh the bypass-UI preset cache off the freshly-loaded sfonts.
    // FluidSynth's iteration order is the SF2's phdr order grouped by
    // sfont; we keep that ordering for the cache so the UI rows track
    // the load sequence (and therefore the priority order FluidSynth
    // applies on preset lookup).
    {
        auto raw = synth->EnumerateLoadedPresets();
        sLoadedPresets.clear();
        sLoadedPresets.reserve(raw.size());
        for (auto& r : raw) {
            LoadedPresetEntry e;
            e.sfontId  = r.sfontId;
            auto it    = idToPackName.find(r.sfontId);
            e.packName = (it != idToPackName.end()) ? it->second : "(unknown)";
            e.bank     = r.bank;
            e.program  = r.program;
            e.name     = std::move(r.name);
            sLoadedPresets.push_back(std::move(e));
        }
        // Unique (sfontId, bank) tuples preserving the load-order of sfonts.
        // Inside one sfont, banks come out in iteration order — usually
        // numerically ascending but we don't rely on that.
        sBankSelectors.clear();
        for (const auto& p : sLoadedPresets) {
            bool exists = false;
            for (const auto& b : sBankSelectors) {
                if (b.sfontId == p.sfontId && b.bank == p.bank) { exists = true; break; }
            }
            if (!exists) {
                sBankSelectors.push_back({ p.sfontId, p.packName, p.bank });
            }
        }
        SPDLOG_INFO("[AudioEditor] FluidSynth: {} presets across {} (sfont, bank) groups",
                    sLoadedPresets.size(), sBankSelectors.size());
    }

    // Refresh entry resolution against the new SF2 stack. Both inputs
    // (entry sfontIds and the pack load order) changed here.
    RefreshEntryResolution(packs);

    SetStatus(std::to_string(loadedPacks) + " pack" + (loadedPacks == 1 ? "" : "s") +
              " loaded (" + std::to_string(totalBytes / (1024 * 1024)) + " MiB total, " +
              std::to_string(sLoadedPresets.size()) + " presets).");

    if (mode == SOH::SynthMode::Authentic) {
        synth->SetReverbParams(0.65, 0.0, 1.0, 1.0);
    } else {
        synth->SetReverbParams(0.20, 0.5, 0.5, 0.30);
    }
    Ship::MidiSynthManager::Instance().SetSynth(synth);
    SOH::MidiTranslator::Instance().SetSynthMode(mode);
    SOH_MidiTranslator_Reset();

    // Register the synth as a MixSource on the AudioPlayer. The callback
    // runs at the device's output rate (which is what we constructed the
    // synth at, above), so its contribution bypasses the resampler. The
    // shared_ptr capture keeps the synth alive as long as the AudioPlayer
    // holds the callback — important because SetSynth(nullptr) drops the
    // manager's strong reference.
    std::weak_ptr<Ship::FluidSynth> weakSynth = synth;
    audioPlayer->SetMixSource([weakSynth](float* stereoOut, int frames) {
        if (auto s = weakSynth.lock()) {
            s->Render(stereoOut, frames);
        } else {
            std::fill(stereoOut, stereoOut + frames * 2, 0.0f);
        }
    });
    return true;
}
} // namespace
#endif

namespace {
// Compile-independent enable/disable for the Modern audio pipeline. The
// float pipeline gate works without FluidSynth compiled in; FluidSynth-
// specific lifecycle (synth install, mix source, status messages) is
// only present in builds where the synth is available.

void DisableModernAudioPipeline() {
    if (auto audioPlayer = Ship::Context::GetRawInstance()->GetAudio()->GetAudioPlayer()) {
#if ENABLE_FLUIDSYNTH
        audioPlayer->SetMixSource(nullptr);
#endif
        audioPlayer->SetUseFloatPipeline(false);
    }
    OTRAudio_SetFloatPipeline(false);
#if ENABLE_FLUIDSYNTH
    Ship::MidiSynthManager::Instance().SetSynth(nullptr);
    SOH_MidiTranslator_Reset();
    SetStatus("Modern audio pipeline disabled.");
#endif
}

bool EnableModernAudioPipeline() {
#if ENABLE_FLUIDSYNTH
    // Full apply path: float mode + (optional) synth pack + status line.
    return ApplyFluidSynthFromCVars();
#else
    auto audioPlayer = Ship::Context::GetRawInstance()->GetAudio()->GetAudioPlayer();
    if (!audioPlayer || !audioPlayer->SetUseFloatPipeline(true)) {
        return false;
    }
    OTRAudio_SetFloatPipeline(true);
    return true;
#endif
}

// Reconcile the CVar against the live audio state once per draw, so a
// toggle on any tab (or from the menu search) takes effect even if the
// tab where the original handler lived isn't currently drawn. Static
// last-seen value is initialised lazily on first call so startup-time
// reconcile (handled by RegisterAudioWidgets) isn't re-run.
void ReconcileModernAudioPipelineIfChanged() {
    int now = CVarGetInteger(CVAR_AUDIO("ModernAudioPipeline"), 0);
    static int sLast = now;
    if (sLast == now) {
        return;
    }
    if (now) {
        if (!EnableModernAudioPipeline()) {
            CVarSetInteger(CVAR_AUDIO("ModernAudioPipeline"), 0);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            now = 0;
        }
    } else {
        DisableModernAudioPipeline();
    }
    sLast = now;
}
} // namespace

extern "C" {
#include "z64save.h"
extern SaveContext gSaveContext;
}

Vec3f pos = { 0.0f, 0.0f, 0.0f };
f32 freqScale = 1.0f;
s8 reverbAdd = 0;

using namespace UIWidgets;

static WidgetInfo lowHpAlarm;
static WidgetInfo naviCall;
static WidgetInfo enemyProx;
static WidgetInfo leeverProx;
static WidgetInfo leadingMusic;
static WidgetInfo displaySeqName;
static WidgetInfo ovlDuration;
static WidgetInfo voicePitch;
static WidgetInfo randomAudioGenModes;
static WidgetInfo lowerOctaves;

#if ENABLE_FLUIDSYNTH
static WidgetInfo fluidSynthEnabled;
static WidgetInfo fluidSynthGain;
#endif

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

// Authentic sequence counts
// used to ensure we have enough to shuffle
#define SEQ_COUNT_BGM_WORLD 30
#define SEQ_COUNT_BGM_BATTLE 6
#define SEQ_COUNT_FANFARE 15
#define SEQ_COUNT_OCARINA 12
#define SEQ_COUNT_NOSHUFFLE 6
#define SEQ_COUNT_BGM_EVENT 17
#define SEQ_COUNT_INSTRUMENT 6
#define SEQ_COUNT_SFX 57
#define SEQ_COUNT_VOICE 108
#define SEQ_COUNT_ENDING 5

size_t AuthenticCountBySequenceType(SeqType type) {
    switch (type) {
        case SEQ_NOSHUFFLE:
            return SEQ_COUNT_NOSHUFFLE;
        case SEQ_BGM_WORLD:
            return SEQ_COUNT_BGM_WORLD;
        case SEQ_BGM_EVENT:
            return SEQ_COUNT_BGM_EVENT;
        case SEQ_BGM_BATTLE:
            return SEQ_COUNT_BGM_BATTLE;
        case SEQ_OCARINA:
            return SEQ_COUNT_OCARINA;
        case SEQ_FANFARE:
            return SEQ_COUNT_FANFARE;
        case SEQ_SFX:
            return SEQ_COUNT_SFX;
        case SEQ_INSTRUMENT:
            return SEQ_COUNT_INSTRUMENT;
        case SEQ_VOICE:
            return SEQ_COUNT_VOICE;
        case SEQ_ENDING:
            return SEQ_COUNT_ENDING;
        default:
            return 0;
    }
}

static const std::map<int32_t, const char*> audioRandomizerModes = {
    { RANDOMIZE_OFF, "Manual" },
    { RANDOMIZE_ON_NEW_SCENE, "On New Scene" },
    { RANDOMIZE_ON_RANDO_GEN_ONLY, "On Rando Gen Only" },
    { RANDOMIZE_ON_FILE_LOAD, "On File Load" },
    { RANDOMIZE_ON_FILE_LOAD_SEEDED, "On File Load (Seeded)" },
};

// Grabs the current BGM sequence ID and replays it
// which will lookup the proper override, or reset back to vanilla
void ReplayCurrentBGM() {
    u16 curSeqId = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
    // TODO: replace with Audio_StartSeq when the macro is shared
    // The fade time and audio player flags will always be 0 in the case of replaying the BGM, so they are not set here
    Audio_QueueSeqCmd(0x00000000 | curSeqId);
}

// Attempt to update the BGM if it matches the current sequence that is being played
// The seqKey that is passed in should be the vanilla ID, not the override ID
void UpdateCurrentBGM(u16 seqKey, SeqType seqType) {
    if (seqType != SEQ_BGM_WORLD) {
        return;
    }

    u16 curSeqId = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
    if (curSeqId == seqKey) {
        ReplayCurrentBGM();
    }
}

void RandomizeGroup(SeqType type, bool manual = true) {
    std::vector<u16> values;

    uint64_t localRngState = 0;
    uint64_t* shuffleState = nullptr;

    if (!manual) {
        int randomizeMode = CVarGetInteger(CVAR_AUDIO("RandomizeAudioGenModes"), 0);
        if (randomizeMode == RANDOMIZE_ON_FILE_LOAD_SEEDED || randomizeMode == RANDOMIZE_ON_RANDO_GEN_ONLY) {

            uint32_t finalSeed = type + (IS_RANDO ? Rando::Context::GetInstance()->GetSeed()
                                                  : static_cast<uint32_t>(gSaveContext.ship.stats.fileCreatedAt));
            ShipUtils::RandInit(finalSeed, &localRngState);
            shuffleState = &localRngState;
        }
        // For RANDOMIZE_ON_NEW_SCENE, shuffleState remains nullptr, which uses the global RNG
    }

    // An empty IncludedSequences set means that the AudioEditor window has never been drawn
    if (AudioCollection::Instance->GetIncludedSequences().empty()) {
        AudioCollection::Instance->InitializeShufflePool();
    }

    // use a while loop to add duplicates if we don't have enough included sequences
    while (values.size() < AuthenticCountBySequenceType(type)) {
        for (const auto& seqData : AudioCollection::Instance->GetIncludedSequences()) {
            if (seqData->category & type && seqData->canBeUsedAsReplacement) {
                values.push_back(seqData->sequenceId);
            }
        }

        // if we didn't find any, return early without shuffling to prevent an infinite loop
        if (!values.size())
            return;
    }
    ShipUtils::Shuffle(values, shuffleState);
    for (const auto& [seqId, seqData] : AudioCollection::Instance->GetAllSequences()) {
        const std::string cvarKey = AudioCollection::Instance->GetCvarKey(seqData.sfxKey);
        const std::string cvarLockKey = AudioCollection::Instance->GetCvarLockKey(seqData.sfxKey);
        // don't randomize locked entries
        if ((seqData.category & type) && CVarGetInteger(cvarLockKey.c_str(), 0) == 0) {
            // Only save authentic sequence CVars
            if ((((seqData.category & SEQ_BGM_CUSTOM) || seqData.category == SEQ_FANFARE) &&
                 seqData.sequenceId >= MAX_AUTHENTIC_SEQID) ||
                seqData.canBeReplaced == false) {
                continue;
            }
            const int randomValue = values.back();
            CVarSetInteger(cvarKey.c_str(), randomValue);
            values.pop_back();
        }
    }
}

void ResetGroup(const std::map<u16, SequenceInfo>& map, SeqType type) {
    for (const auto& [defaultValue, seqData] : map) {
        if (seqData.category == type) {
            // Only save authentic sequence CVars
            if (seqData.category == SEQ_FANFARE && defaultValue >= MAX_AUTHENTIC_SEQID) {
                continue;
            }
            const std::string cvarKey = AudioCollection::Instance->GetCvarKey(seqData.sfxKey);
            const std::string cvarLockKey = AudioCollection::Instance->GetCvarLockKey(seqData.sfxKey);
            if (CVarGetInteger(cvarLockKey.c_str(), 0) == 0) {
                CVarClear(cvarKey.c_str());
            }
        }
    }
}

void LockGroup(const std::map<u16, SequenceInfo>& map, SeqType type) {
    for (const auto& [defaultValue, seqData] : map) {
        if (seqData.category == type) {
            // Only save authentic sequence CVars
            if (seqData.category == SEQ_FANFARE && defaultValue >= MAX_AUTHENTIC_SEQID) {
                continue;
            }
            const std::string cvarKey = AudioCollection::Instance->GetCvarKey(seqData.sfxKey);
            const std::string cvarLockKey = AudioCollection::Instance->GetCvarLockKey(seqData.sfxKey);
            CVarSetInteger(cvarLockKey.c_str(), 1);
        }
    }
}

void UnlockGroup(const std::map<u16, SequenceInfo>& map, SeqType type) {
    for (const auto& [defaultValue, seqData] : map) {
        if (seqData.category == type) {
            // Only save authentic sequence CVars
            if (seqData.category == SEQ_FANFARE && defaultValue >= MAX_AUTHENTIC_SEQID) {
                continue;
            }
            const std::string cvarKey = AudioCollection::Instance->GetCvarKey(seqData.sfxKey);
            const std::string cvarLockKey = AudioCollection::Instance->GetCvarLockKey(seqData.sfxKey);
            CVarSetInteger(cvarLockKey.c_str(), 0);
        }
    }
}

void DrawPreviewButton(uint16_t sequenceId, std::string sfxKey, SeqType sequenceType) {
    const std::string cvarKey = AudioCollection::Instance->GetCvarKey(sfxKey);
    const std::string hiddenKey = "##" + cvarKey;
    const std::string stopButton = ICON_FA_STOP + hiddenKey;
    const std::string previewButton = ICON_FA_PLAY + hiddenKey;

    if (CVarGetInteger(CVAR_AUDIO("Playing"), 0) == sequenceId) {
        if (UIWidgets::Button(stopButton.c_str(), UIWidgets::ButtonOptions()
                                                      .Size(UIWidgets::Sizes::Inline)
                                                      .Padding(ImVec2(10.0f, 6.0f))
                                                      .Tooltip("Stop Preview")
                                                      .Color(THEME_COLOR))) {
            func_800F5C2C();
            // Abrupt stop: the engine swaps the sequence pointer without
            // tracing each active note through Audio_NoteDisable, so
            // FluidSynth voices for whatever was sounding become orphans
            // in their release tail. They eventually free on their own,
            // but cycling through many previews accumulates them faster
            // than they drain, exhausts the voice pool, and triggers
            // voice stealing on the next busy song. Resetting forces an
            // immediate All Notes Off on every channel.
            SOH_MidiTranslator_Reset();
            CVarSetInteger(CVAR_AUDIO("Playing"), 0);
        }
    } else {
        if (UIWidgets::Button(previewButton.c_str(), UIWidgets::ButtonOptions()
                                                         .Size(UIWidgets::Sizes::Inline)
                                                         .Padding(ImVec2(10.0f, 6.0f))
                                                         .Tooltip("Play Preview")
                                                         .Color(THEME_COLOR))) {
            if (CVarGetInteger(CVAR_AUDIO("Playing"), 0) != 0) {
                func_800F5C2C();
                // Same orphan-voice flush as the stop button above.
                SOH_MidiTranslator_Reset();
                CVarSetInteger(CVAR_AUDIO("Playing"), 0);
            } else {
                if (sequenceType == SEQ_SFX || sequenceType == SEQ_VOICE) {
                    Audio_PlaySoundGeneral(sequenceId, &pos, 4, &freqScale, &freqScale, &reverbAdd);
                } else if (sequenceType == SEQ_INSTRUMENT) {
                    Audio_OcaSetInstrument(sequenceId - INSTRUMENT_OFFSET);
                    Audio_OcaSetSongPlayback(9, 1);
                } else {
                    // TODO: Cant do both here, so have to click preview button twice
                    PreviewSequence(sequenceId);
                    CVarSetInteger(CVAR_AUDIO("Playing"), sequenceId);
                }
            }
        }
    }
}

void Draw_SfxTab(const std::string& tabId, SeqType type, const std::string& tabName) {
    const std::map<u16, SequenceInfo>& map = AudioCollection::Instance->GetAllSequences();

    const std::string hiddenTabId = "##" + tabId;
    const std::string resetAllButton = "Reset All" + hiddenTabId;
    const std::string randomizeAllButton = "Randomize All" + hiddenTabId;
    const std::string lockAllButton = "Lock All" + hiddenTabId;
    const std::string unlockAllButton = "Unlock All" + hiddenTabId;

    ImGui::SeparatorText(tabName.c_str());
    if (UIWidgets::Button(resetAllButton.c_str(),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
        auto currentBGM = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
        auto prevReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        ResetGroup(map, type);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        auto curReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        if (type == SEQ_BGM_WORLD && prevReplacement != curReplacement) {
            ReplayCurrentBGM();
        }
    }
    ImGui::SameLine();
    if (UIWidgets::Button(randomizeAllButton.c_str(),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
        auto currentBGM = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
        auto prevReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        RandomizeGroup(type);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        auto curReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        if (type == SEQ_BGM_WORLD && prevReplacement != curReplacement) {
            ReplayCurrentBGM();
        }
    }
    ImGui::SameLine();
    if (UIWidgets::Button(lockAllButton.c_str(),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
        auto currentBGM = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
        auto prevReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        LockGroup(map, type);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        auto curReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        if (type == SEQ_BGM_WORLD && prevReplacement != curReplacement) {
            ReplayCurrentBGM();
        }
    }
    ImGui::SameLine();
    if (UIWidgets::Button(unlockAllButton.c_str(),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
        auto currentBGM = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
        auto prevReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        UnlockGroup(map, type);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        auto curReplacement = AudioCollection::Instance->GetReplacementSequence(currentBGM);
        if (type == SEQ_BGM_WORLD && prevReplacement != curReplacement) {
            ReplayCurrentBGM();
        }
    }

    auto playingFromMenu = CVarGetInteger(CVAR_AUDIO("Playing"), 0);
    auto currentBGM = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);

    // Longest text in Audio Editor
    ImVec2 columnSize = ImGui::CalcTextSize("Navi - Look/Hey/Watchout (Target Enemy)");
    ImGui::BeginTable(tabId.c_str(), 3, ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, columnSize.x + 30);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, columnSize.x + 30);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    for (const auto& [defaultValue, seqData] : map) {
        if (~(seqData.category) & type) {
            continue;
        }
        // Do not display custom sequences in the list
        if ((((seqData.category & SEQ_BGM_CUSTOM) || seqData.category == SEQ_FANFARE) &&
             defaultValue >= MAX_AUTHENTIC_SEQID) ||
            seqData.canBeReplaced == false) {
            continue;
        }

        const std::string initialSfxKey = seqData.sfxKey;
        const std::string cvarKey = AudioCollection::Instance->GetCvarKey(seqData.sfxKey);
        const std::string cvarLockKey = AudioCollection::Instance->GetCvarLockKey(seqData.sfxKey);
        const std::string hiddenKey = "##" + cvarKey;
        const std::string resetButton = ICON_FA_UNDO + hiddenKey;
        const std::string randomizeButton = ICON_FA_RANDOM + hiddenKey;
        const std::string lockedButton = ICON_FA_LOCK + hiddenKey;
        const std::string unlockedButton = ICON_FA_UNLOCK + hiddenKey;
        const int currentValue = CVarGetInteger(cvarKey.c_str(), defaultValue);
        const bool isCurrentlyPlaying = currentValue == playingFromMenu || seqData.sequenceId == currentBGM;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (isCurrentlyPlaying) {
            ImGui::TextColored(UIWidgets::ColorValues.at(UIWidgets::Colors::Yellow), "%s %s", ICON_FA_PLAY,
                               seqData.label.c_str());
        } else {
            ImGui::Text("%s", seqData.label.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::PushItemWidth(-FLT_MIN);
        const int initialValue = map.contains(currentValue) ? currentValue : defaultValue;
        UIWidgets::PushStyleCombobox(THEME_COLOR);
        if (ImGui::BeginCombo(hiddenKey.c_str(), map.at(initialValue).label.c_str())) {
            for (const auto& [value, seqData] : map) {
                // If excluded as a replacement sequence, don't show in other dropdowns except the effect's own
                // dropdown.
                if (~(seqData.category) & type ||
                    (!seqData.canBeUsedAsReplacement && initialSfxKey != seqData.sfxKey)) {
                    continue;
                }

                if (ImGui::Selectable(seqData.label.c_str())) {
                    CVarSetInteger(cvarKey.c_str(), value);
                    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    UpdateCurrentBGM(defaultValue, type);
                }

                if (currentValue == value) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }
        UIWidgets::PopStyleCombobox();
        ImGui::TableNextColumn();
        ImGui::PushItemWidth(-FLT_MIN);
        DrawPreviewButton((type == SEQ_SFX || type == SEQ_VOICE || type == SEQ_INSTRUMENT) ? defaultValue
                                                                                           : currentValue,
                          seqData.sfxKey, type);
        auto locked = CVarGetInteger(cvarLockKey.c_str(), 0) == 1;
        ImGui::SameLine();
        ImGui::PushItemWidth(-FLT_MIN);
        if (UIWidgets::Button(resetButton.c_str(), UIWidgets::ButtonOptions()
                                                       .Size(UIWidgets::Sizes::Inline)
                                                       .Padding(ImVec2(10.0f, 6.0f))
                                                       .Tooltip("Reset to default")
                                                       .Color(THEME_COLOR))) {
            CVarClear(cvarKey.c_str());
            CVarClear(cvarLockKey.c_str());
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            UpdateCurrentBGM(defaultValue, seqData.category);
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(-FLT_MIN);
        if (UIWidgets::Button(randomizeButton.c_str(), UIWidgets::ButtonOptions()
                                                           .Size(UIWidgets::Sizes::Inline)
                                                           .Padding(ImVec2(10.0f, 6.0f))
                                                           .Tooltip("Randomize this sound")
                                                           .Color(THEME_COLOR))) {
            std::vector<SequenceInfo*> validSequences = {};
            for (const auto seqInfo : AudioCollection::Instance->GetIncludedSequences()) {
                if (seqInfo->category & type) {
                    validSequences.push_back(seqInfo);
                }
            }

            if (validSequences.size()) {
                auto it = validSequences.begin();
                const auto& seqData = *std::next(it, ShipUtils::Random(0, validSequences.size()));
                CVarSetInteger(cvarKey.c_str(), seqData->sequenceId);
                if (locked) {
                    CVarClear(cvarLockKey.c_str());
                }
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                UpdateCurrentBGM(defaultValue, type);
            }
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(-FLT_MIN);
        if (UIWidgets::Button(locked ? lockedButton.c_str() : unlockedButton.c_str(),
                              UIWidgets::ButtonOptions()
                                  .Size(UIWidgets::Sizes::Inline)
                                  .Padding(ImVec2(10.0f, 6.0f))
                                  .Tooltip(locked ? "Sound locked" : "Sound unlocked")
                                  .Color(THEME_COLOR))) {
            if (locked) {
                CVarClear(cvarLockKey.c_str());
            } else {
                CVarSetInteger(cvarLockKey.c_str(), 1);
            }
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }
    ImGui::EndTable();
}

extern "C" u16 AudioEditor_GetReplacementSeq(u16 seqId) {
    return AudioCollection::Instance->GetReplacementSequence(seqId);
}

std::string GetSequenceTypeName(SeqType type) {
    switch (type) {
        case SEQ_NOSHUFFLE:
            return "No Shuffle";
        case SEQ_BGM_WORLD:
            return "World";
        case SEQ_BGM_EVENT:
            return "Event";
        case SEQ_BGM_BATTLE:
            return "Battle";
        case SEQ_OCARINA:
            return "Ocarina";
        case SEQ_FANFARE:
            return "Fanfare";
        case SEQ_BGM_ERROR:
            return "Error";
        case SEQ_SFX:
            return "SFX";
        case SEQ_VOICE:
            return "Voice";
        case SEQ_INSTRUMENT:
            return "Instrument";
        case SEQ_BGM_CUSTOM:
            return "Custom";
        default:
            return "No Sequence Type";
    }
}

ImVec4 GetSequenceTypeColor(SeqType type) {
    switch (type) {
        case SEQ_BGM_WORLD:
            return ImVec4(0.0f, 0.2f, 0.0f, 1.0f);
        case SEQ_BGM_EVENT:
            return ImVec4(0.3f, 0.0f, 0.15f, 1.0f);
        case SEQ_BGM_BATTLE:
            return ImVec4(0.2f, 0.07f, 0.0f, 1.0f);
        case SEQ_OCARINA:
            return ImVec4(0.0f, 0.0f, 0.4f, 1.0f);
        case SEQ_FANFARE:
            return ImVec4(0.3f, 0.0f, 0.3f, 1.0f);
        case SEQ_SFX:
            return ImVec4(0.4f, 0.33f, 0.0f, 1.0f);
        case SEQ_VOICE:
            return ImVec4(0.3f, 0.42f, 0.09f, 1.0f);
        case SEQ_INSTRUMENT:
            return ImVec4(0.0f, 0.25f, 0.5f, 1.0f);
        case SEQ_BGM_CUSTOM:
            return ImVec4(0.9f, 0.0f, 0.9f, 1.0f);
        default:
            return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
}

void DrawTypeChip(SeqType type, std::string sequenceName) {
    ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, GetSequenceTypeColor(type));
    std::string buttonLabel = GetSequenceTypeName(type) + "##" + sequenceName;
    ImGui::Button(buttonLabel.c_str());
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
}

void AudioEditorRegisterOnSceneInitHook() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](int16_t sceneNum) {
        if (gSaveContext.gameMode != GAMEMODE_END_CREDITS &&
            CVarGetInteger(CVAR_AUDIO("RandomizeAudioGenModes"), 0) == RANDOMIZE_ON_NEW_SCENE) {

            AudioEditor_AutoRandomizeAll();
        }
    });
}

void AudioEditorRegisterOnGenerationCompletionHook() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGenerationCompletion>([]() {
        if (CVarGetInteger(CVAR_AUDIO("RandomizeAudioGenModes"), 0) == RANDOMIZE_ON_RANDO_GEN_ONLY) {

            AudioEditor_AutoRandomizeAll();
        }
    });
}

void AudioEditorRegisterOnLoadGameHook() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([](int32_t fileNum) {
        if (CVarGetInteger(CVAR_AUDIO("RandomizeAudioGenModes"), 0) == RANDOMIZE_ON_FILE_LOAD ||
            CVarGetInteger(CVAR_AUDIO("RandomizeAudioGenModes"), 0) == RANDOMIZE_ON_FILE_LOAD_SEEDED) {

            AudioEditor_AutoRandomizeAll();
        }
    });
}

void AudioEditor::InitElement() {
    AudioEditorRegisterOnSceneInitHook();
    AudioEditorRegisterOnGenerationCompletionHook();
    AudioEditorRegisterOnLoadGameHook();
}

void AudioEditor::DrawElement() {
    AudioCollection::Instance->InitializeShufflePool();

    // Pick up Modern audio pipeline CVar transitions regardless of which
    // tab is active right now — the toggle can land here from the Audio
    // Options checkbox, from the FluidSynth tab's Enable button, or from
    // the menu search bar.
    ReconcileModernAudioPipelineIfChanged();

    UIWidgets::Separator();
    if (UIWidgets::Button("Randomize All Groups",
                          UIWidgets::ButtonOptions()
                              .Size(ImVec2(230.0f, 0.0f))
                              .Color(THEME_COLOR)
                              .Tooltip("Randomizes all unlocked music and sound effects across tab groups"))) {
        AudioEditor_RandomizeAll();
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Reset All Groups",
                          UIWidgets::ButtonOptions()
                              .Size(ImVec2(230.0f, 0.0f))
                              .Color(THEME_COLOR)
                              .Tooltip("Resets all unlocked music and sound effects across tab groups"))) {
        AudioEditor_ResetAll();
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Lock All Groups", UIWidgets::ButtonOptions()
                                                 .Size(ImVec2(230.0f, 0.0f))
                                                 .Color(THEME_COLOR)
                                                 .Tooltip("Locks all music and sound effects across tab groups"))) {
        AudioEditor_LockAll();
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Unlock All Groups", UIWidgets::ButtonOptions()
                                                   .Size(ImVec2(230.0f, 0.0f))
                                                   .Color(THEME_COLOR)
                                                   .Tooltip("Unlocks all music and sound effects across tab groups"))) {
        AudioEditor_UnlockAll();
    }
    UIWidgets::Separator();

    UIWidgets::PushStyleTabs(THEME_COLOR);
    if (ImGui::BeginTabBar("SfxContextTabBar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {

        static ImVec2 cellPadding(8.0f, 8.0f);
        if (ImGui::BeginTabItem("Audio Options")) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
            ImGui::BeginTable("Audio Options", 1, ImGuiTableFlags_SizingStretchSame);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("SfxOptions", ImVec2(0, -8))) {
                SohGui::mSohMenu->MenuDrawItem(lowHpAlarm, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(naviCall, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(enemyProx, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                if (!CVarGetInteger(CVAR_AUDIO("EnemyBGMDisable"), 0)) {
                    SohGui::mSohMenu->MenuDrawItem(leeverProx, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                }
                SohGui::mSohMenu->MenuDrawItem(leadingMusic, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(displaySeqName, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(ovlDuration, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(voicePitch, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPos().y + 40.f);
                if (UIWidgets::Button("Reset##linkVoiceFreqMultiplier",
                                      UIWidgets::ButtonOptions().Size(ImVec2(80, 36)).Padding(ImVec2(5.0f, 0.0f)))) {
                    CVarSetFloat(CVAR_AUDIO("LinkVoiceFreqMultiplier"), 1.0f);
                }
                SohGui::mSohMenu->MenuDrawItem(randomAudioGenModes, ImGui::GetContentRegionAvail().x, THEME_COLOR);
                SohGui::mSohMenu->MenuDrawItem(lowerOctaves, ImGui::GetContentRegionAvail().x, THEME_COLOR);

                // Master switch for the Modern audio pipeline (float).
                // Always compiled — the float pipeline works without
                // FluidSynth. When ENABLE_FLUIDSYNTH is on, a dedicated
                // FluidSynth tab exposes synth-pack selection and the
                // per-instrument overrides. CVar transitions are handled
                // by ReconcileModernAudioPipelineIfChanged at the top of
                // DrawElement so a toggle here (or from the menu search)
                // takes effect regardless of which tab is active.
                // Rendered via MenuDrawItem so it matches the visual
                // style of the other Audio Options checkboxes.
                SohGui::mSohMenu->MenuDrawItem(fluidSynthEnabled,
                                               ImGui::GetContentRegionAvail().x, THEME_COLOR);

                // (FluidSynth pack selection, mode/volume, and the
                // per-instrument override table live in the dedicated
                // "FluidSynth" tab — see BeginTabItem("FluidSynth") below.)
            }
            ImGui::EndChild();
            ImGui::EndTable();
            ImGui::PopStyleVar(1);
            ImGui::EndTabItem();
        }

#if ENABLE_FLUIDSYNTH
        if (ImGui::BeginTabItem("FluidSynth")) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
            ImGui::BeginTable("FluidSynthTab", 1, ImGuiTableFlags_SizingStretchSame);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("FluidSynthChild", ImVec2(0, -8))) {

                const bool pipelineOn = CVarGetInteger(CVAR_AUDIO("ModernAudioPipeline"), 0) != 0;

                if (!pipelineOn) {
                    // Pipeline is the gate for everything in this tab. Offer a
                    // one-click affordance so users who land here from the
                    // menu / docs don't have to flip back to Audio Options to
                    // enable it. The CVar transition is picked up by the
                    // top-of-DrawElement reconcile on the next frame.
                    ImGui::TextWrapped(
                        "The Modern audio pipeline is required to use FluidSynth. "
                        "Enable it to load a synth pack and route engine instruments "
                        "through it.");
                    ImGui::Spacing();
                    if (ImGui::Button("Enable Modern Pipeline", ImVec2(220, 0))) {
                        CVarSetInteger(CVAR_AUDIO("ModernAudioPipeline"), 1);
                        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    }
                } else {
                    // ── Status line ──────────────────────────────────────
                    // sLastStatus is written by ApplyFluidSynthFromCVars and
                    // friends so failure messages persist on screen instead
                    // of just landing in spdlog.
                    if (!sLastStatus.message.empty()) {
                        const ImVec4 green(0.40f, 0.85f, 0.45f, 1.0f);
                        const ImVec4 red(0.95f, 0.45f, 0.45f, 1.0f);
                        ImGui::TextColored(sLastStatus.isError ? red : green, "%s",
                                           sLastStatus.message.c_str());
                    }
                    ImGui::Separator();

                    // ── Synth packs ──────────────────────────────────────
                    // Discovered packs come from two sources (see
                    // EnumerateSynthPacks): mod-supplied via mounted .o2r
                    // archives, then loose .sf2 files under
                    // <config-dir>/synth-packs/. The list is cached across
                    // frames; the Rescan button re-enumerates without a
                    // restart so a freshly-dropped file becomes visible.
                    static std::vector<SynthPackEntry> packs;
                    static bool packsListed = false;
                    if (!packsListed) {
                        packs = EnumerateSynthPacks();
                        packsListed = true;
                    }

                    // Count enabled packs for the summary header. We
                    // recompute the disabled set each frame — it's a cheap
                    // parse and keeps the count truthful even if a sibling
                    // tool edits the CVar between frames.
                    auto disabledSet = ParseDisabledPacksCSV();
                    int enabledCount = 0;
                    for (const auto& e : packs) {
                        if (!disabledSet.count(e.name)) enabledCount++;
                    }

                    ImGui::Text("Synth packs (%d enabled / %d discovered)",
                                enabledCount, (int)packs.size());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Rescan##fluidsynthPacks")) {
                        packs = EnumerateSynthPacks();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Re-enumerate audio/synth/* across mounted .o2r archives\n"
                            "and <config-dir>/synth-packs/*.sf2. Use after dropping a\n"
                            "new SF2 or mod without restarting.");
                    }

                    if (packs.empty()) {
                        ImGui::TextDisabled(
                            "No synth packs discovered.\n"
                            "Drop an SF2 into <config-dir>/synth-packs/ (optionally\n"
                            "with a sibling .json mapping) or install a mod that\n"
                            "ships audio/synth/<pack>/soundfont.sf2.");
                    } else {
                        // Bordered child so the list reads as a unit when
                        // many packs are discovered. Height clamps to a
                        // reasonable max so the rest of the tab stays
                        // visible; users can scroll inside.
                        const float rowH = ImGui::GetTextLineHeightWithSpacing();
                        float listH = rowH * (float)std::min<int>(packs.size(), 10) + 12.0f;
                        if (ImGui::BeginChild("##synthPackList",
                                              ImVec2(420.0f, listH),
                                              ImGuiChildFlags_Border)) {
                            for (size_t i = 0; i < packs.size(); i++) {
                                const auto& e = packs[i];
                                bool enabled = !disabledSet.count(e.name);
                                ImGui::PushID((int)i);
                                if (ImGui::Checkbox("##packCheck", &enabled)) {
                                    SetPackDisabled(e.name, !enabled);
                                    Ship::Context::GetRawInstance()->GetWindow()->GetGui()
                                        ->SaveConsoleVariablesNextFrame();
                                    ReapplyOverrideChain();
                                    ApplyFluidSynthFromCVars();
                                }
                                ImGui::SameLine();
                                const char* badge =
                                    (e.source == SynthPackEntry::Source::Archive) ? "[mod]" : "[loose]";
                                ImGui::TextDisabled("%-7s", badge);
                                ImGui::SameLine();
                                ImGui::TextUnformatted(e.name.c_str());
                                if (ImGui::IsItemHovered()) {
                                    if (e.mappingPath.empty()) {
                                        ImGui::SetTooltip("%s\n(SF2 only - no mapping.json)", e.sf2Path.c_str());
                                    } else {
                                        ImGui::SetTooltip("%s\nmapping: %s",
                                                          e.sf2Path.c_str(), e.mappingPath.c_str());
                                    }
                                }
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndChild();
                        ImGui::TextDisabled(
                            "Order = discovery order (mods first, then synth-packs/).\n"
                            "Later packs win on (bank, program) collisions.");
                    }

                    if (enabledCount == 0) {
                        // Pipeline-only mode: native synthesis runs through
                        // the float path, but no SF2 substitution happens.
                        // The synth-side controls (mode radio, volume slider,
                        // bypass table) are still relevant only when at
                        // least one pack is active.
                        ImGui::Spacing();
                        ImGui::TextDisabled(
                            "No synth packs enabled. The Modern audio pipeline is\n"
                            "active on its own (no instrument timbre change).");
                    } else {
                        ImGui::Separator();

                        // ── Synth mode + volume ──────────────────────────
                        SohGui::mSohMenu->MenuDrawItem(fluidSynthGain,
                                                       ImGui::GetContentRegionAvail().x, THEME_COLOR);

                        {
                            int mode = CVarGetInteger(CVAR_AUDIO("FluidSynthMode"), 0);
                            ImGui::TextUnformatted("Synth mode:");
                            ImGui::SameLine();
                            if (ImGui::RadioButton("Authentic##synthMode", mode == 0)) {
                                CVarSetInteger(CVAR_AUDIO("FluidSynthMode"), 0);
                                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Graham-Smith volume curve + console-era reverb.\n"
                                    "Translator fixes NoteOn velocity at 100 and routes the\n"
                                    "sqrt(velocity)-shaped value through CC11.");
                            }
                            ImGui::SameLine();
                            if (ImGui::RadioButton("Enhanced##synthMode", mode == 1)) {
                                CVarSetInteger(CVAR_AUDIO("FluidSynthMode"), 1);
                                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Stock SF2 default modulators + subtle reverb.\n"
                                    "Translator sends the sqrt(velocity)-shaped value as\n"
                                    "NoteOn velocity so the SF2's own concave attenuation\n"
                                    "modulator shapes dynamics. Good with musically-curated\n"
                                    "banks (MuseScore, SC-55, orchestral packs).");
                            }

                            int nowMode = CVarGetInteger(CVAR_AUDIO("FluidSynthMode"), 0);
                            static int sLastMode = nowMode;
                            if (nowMode != sLastMode) {
                                ApplyFluidSynthFromCVars();
                                sLastMode = nowMode;
                            }
                        }

                        ImGui::Separator();

                        // ── Per-instrument overrides ─────────────────────
                        SOH::DiscoveredPair pairs[SOH::MidiTranslator::kMaxDiscovered];
                        int nPairs = SOH::MidiTranslator::Instance().DiscoveredSnapshot(
                            pairs, SOH::MidiTranslator::kMaxDiscovered);

                        ImGui::TextDisabled("Discovered (fontId, instOrWave) - heard %d unique pair%s",
                                            nPairs, nPairs == 1 ? "" : "s");
                        ImGui::SameLine();
                        bool transSemis = CVarGetInteger(CVAR_AUDIO("FluidSynthTransSemitones"), 0) != 0;
                        if (ImGui::SmallButton("Clear list##bypassClear")) {
                            SOH::MidiTranslator::Instance().ClearDiscovered();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset all##bypassReset")) {
                            // Reset all + auto-save the cleared state so disk reflects
                            // the wipe. Without this, the next pack toggle would re-read
                            // the stale fluidsynth_overrides.json and undo the reset.
                            ResetToPackBaseline();
                            AutoSaveOverrides();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Drop every personal override and restore the active pack's\n"
                                "defaults (Mode, Gain, Trans, Preset, effects). Discovered\n"
                                "list is left alone. The change is persisted to disk\n"
                                "automatically.");
                        }
                        ImGui::TextDisabled("Mode picks Native (engine plays this) or Synth (FluidSynth plays this). "
                                            "Gain is a per-instrument CC11 multiplier; Trans shifts pitch by semitones. "
                                            "Edits auto-save to fluidsynth_overrides.json — no Save button needed.");

                        const float viewportH = ImGui::GetMainViewport()->Size.y;
                        float bypassTableHeight = viewportH * 0.65f;
                        if (bypassTableHeight < 400.0f) bypassTableHeight = 400.0f;
                        if (bypassTableHeight > 900.0f) bypassTableHeight = 900.0f;
                        // Column-id helper. Postincrement each named slot so
                        // adding/removing columns shifts the rest automatically. The
                        // saved variables (modeCol, presetCol, ...) are the only IDs
                        // referenced by per-row code below, so renumbering is a one-line
                        // edit here, not a hunt-and-replace.
                        uint8_t col = 0;
                        const uint8_t overrideCol = col++;
                        const uint8_t songCol     = col++;
                        const uint8_t sampleCol   = col++;
                        const uint8_t fontCol     = col++;
                        const uint8_t instCol     = col++;
                        const uint8_t modeCol     = col++;
                        const uint8_t gainCol     = col++;
                        const uint8_t shiftCol    = col++;
                        const uint8_t sourceCol   = col++;
                        const uint8_t presetCol   = col++;
                        const uint8_t reverbCol   = col++;
                        const uint8_t chorusCol   = col++;
                        const uint8_t cutoffCol   = col++;
                        const uint8_t qCol        = col++;
                        const uint8_t kColCount   = col;

                        if (ImGui::BeginTable("##bypassTable", kColCount,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                                  ImGuiTableFlags_Resizable,
                                              ImVec2(0.0f, bypassTableHeight))) {
                            // The Override column holds session-only widgets (Solo,
                            // temp volume). It sits first and its widgets are styled
                            // distinctly so the user reads "these don't get saved" at
                            // a glance.
                            ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthFixed, 168.0f);
                            // Song stretches wider than Sample by default — the modder UX
                            // wants the song-family label readable at a glance, while the
                            // Sample column's editable rename mostly sits idle and only
                            // expands when typed into.
                            ImGui::TableSetupColumn("Song",     ImGuiTableColumnFlags_WidthStretch, 1.6f);
                            ImGui::TableSetupColumn("Sample",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
                            ImGui::TableSetupColumn("Font",     ImGuiTableColumnFlags_WidthFixed, 36.0f);
                            ImGui::TableSetupColumn("Inst",     ImGuiTableColumnFlags_WidthFixed, 78.0f);
                            ImGui::TableSetupColumn("Mode",     ImGuiTableColumnFlags_WidthFixed, 150.0f);
                            ImGui::TableSetupColumn("Gain",     ImGuiTableColumnFlags_WidthFixed, 130.0f);
                            ImGui::TableSetupColumn(transSemis ? "Shift (st)" : "Shift (oct)",
                                                                ImGuiTableColumnFlags_WidthFixed, 85.0f);
                            // Source: read-only `[Pack] B#` label that tracks the Preset
                            // combo's pinned target. Carries the pin-state visual cue
                            // (live / dead / unset) so the Preset combo can stay
                            // narrower and show the preset name in full.
                            ImGui::TableSetupColumn("Source",   ImGuiTableColumnFlags_WidthFixed, 130.0f);
                            ImGui::TableSetupColumn("Preset",   ImGuiTableColumnFlags_WidthFixed, 280.0f);
                            // PR 7 per-pair effect sends + filter. Tightened from 65/50 px
                            // to even widths since the DragInts only need to fit a 3-digit
                            // number; the prior reverb width gave it ~20 px more than it
                            // needed and pushed the rest of the table off-screen on smaller
                            // monitors.
                            ImGui::TableSetupColumn("Reverb",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Chorus",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Cutoff",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Q",        ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupScrollFreeze(0, 2);

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(overrideCol);
                            // Clear-overrides button — wipes all session-only state from the
                            // Override column across every row (solo set, native-row mutes,
                            // and every temp volume). Persisted overrides (Gain, Shift,
                            // Preset, effect CCs) are untouched — those have their own
                            // "Reset all" button above the table.
                            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.20f, 0.05f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.20f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.65f, 0.30f, 1.0f));
                            if (ImGui::SmallButton("Clear##overrideClear")) {
                                sSoloedPairs.clear();
                                sExplicitMutedPairs.clear();
                                SOH::MidiTranslator::Instance().ClearAllTemporaryMutes();
                                SOH::MidiTranslator::Instance().ClearAllTemporaryVolumes();
                            }
                            ImGui::PopStyleColor(3);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Clear every session-only override across the table:\n"
                                    "  - Soloed rows (all unsoloed)\n"
                                    "  - Mute toggles on Native rows\n"
                                    "  - Temp volume sliders on Synth rows\n"
                                    "Persisted overrides (Gain, Shift, Preset, effect CCs) are\n"
                                    "untouched - use 'Reset all' above the table for those.");
                            }
                            ImGui::TableSetColumnIndex(modeCol);
                            if (ImGui::SmallButton("all Native##bypassAllNative")) {
                                SOH::DiscoveredPair tmp[SOH::MidiTranslator::kMaxDiscovered];
                                int n = SOH::MidiTranslator::Instance().DiscoveredSnapshot(
                                    tmp, SOH::MidiTranslator::kMaxDiscovered);
                                for (int i = 0; i < n; i++) {
                                    SOH::MidiTranslator::Instance().ClickNative(
                                        tmp[i].fontId, tmp[i].instOrWave);
                                }
                                AutoSaveOverrides();
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "Force every currently-discovered pair to Native (engine).\n"
                                    "Useful to wipe a bad ForceSynth experiment or to start a\n"
                                    "song from a clean baseline. Undiscovered pairs are\n"
                                    "unaffected. Click Save overrides afterward to persist.");
                            }
                            ImGui::TableSetColumnIndex(shiftCol);
                            {
                                bool transUnit = transSemis;
                                if (ImGui::Checkbox("Semitone##transUnit", &transUnit)) {
                                    CVarSetInteger(CVAR_AUDIO("FluidSynthTransSemitones"),
                                                   transUnit ? 1 : 0);
                                    Ship::Context::GetRawInstance()
                                        ->GetWindow()
                                        ->GetGui()
                                        ->SaveConsoleVariablesNextFrame();
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "Display the Shift column in semitones (+/-24 fine)\n"
                                        "instead of octaves (+/-8 wide). Underlying value is\n"
                                        "the same; column label and DragInt range switch.");
                                }
                            }

                            ImGui::TableHeadersRow();

                            const ImU32 kSynthTint  = IM_COL32(80, 160,  80, 80);
                            const ImU32 kNativeTint = IM_COL32(80, 120, 200, 80);

                            // Effective-mute apply pass — runs once per frame BEFORE the
                            // per-row drawing so the audible state matches the UI even on
                            // the same frame the user clicks. The model:
                            //   effective_mute(pair) = (anySolo && !inSolo) || inExplicitMute
                            // where anySolo = !sSoloedPairs.empty(), inSolo = membership in
                            // sSoloedPairs, inExplicitMute = membership in sExplicitMutedPairs.
                            {
                                bool anySolo = !sSoloedPairs.empty();
                                for (int i = 0; i < nPairs; i++) {
                                    const auto& q = pairs[i];
                                    auto key = std::make_pair(q.fontId, q.instOrWave);
                                    bool inSolo = sSoloedPairs.count(key) > 0;
                                    bool explicitMute = sExplicitMutedPairs.count(key) > 0;
                                    bool eff = (anySolo && !inSolo) || explicitMute;
                                    SOH::MidiTranslator::Instance().SetTemporaryMute(
                                        q.fontId, q.instOrWave, eff);
                                }
                            }

                            for (int i = 0; i < nPairs; i++) {
                                const auto& p = pairs[i];
                                ImGui::TableNextRow();
                                // Push the row index BEFORE any widget so every "##" suffix
                                // in this row gets a unique ID. Previously this lived next
                                // to the Mode column's radio buttons, but the Override and
                                // Song columns (Solo / Unsolo button, ##tempVol DragFloat,
                                // ##displayName InputText) draw before that point and were
                                // colliding across rows.
                                ImGui::PushID(i);

                                const uint8_t synthActive =
                                    SOH::MidiTranslator::Instance().GetSynthActiveCount(p.fontId, p.instOrWave);
                                const uint8_t nativeActive =
                                    SOH::MidiTranslator::Instance().GetNativeActiveCount(p.fontId, p.instOrWave);
                                if (synthActive > 0) {
                                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, kSynthTint);
                                } else if (nativeActive > 0) {
                                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, kNativeTint);
                                }

                                // Hoisted from below: needed by the Override column too.
                                // The Mode/Native vs Synth state comes straight from the
                                // entry resolution — active entry == nullptr means there's
                                // no enabled+resolvable entry for this pair, so native
                                // plays. The "Default:" hint label still references
                                // GetGmPreset since it's the only place to source a
                                // human-readable GM name for the "no override" preset
                                // placeholder.
                                SOH::GmPreset defaultGmForMode = SOH::GetGmPreset(p.fontId, p.instOrWave);
                                const SOH::ConfigEntry* activeEntry =
                                    SOH::MidiTranslator::Instance().GetActiveEntry(p.fontId, p.instOrWave);
                                int activeIdx =
                                    SOH::MidiTranslator::Instance().GetActiveEntryIdx(p.fontId, p.instOrWave);
                                bool effectiveIsNative = (activeEntry == nullptr);

                                // ── Override column (session-only) ─────────
                                // Sits first to signal it's special. Holds the Solo button
                                // plus either a Mute toggle (Native rows) or a temp-volume
                                // slider (Synth rows). All widgets tinted orange so "this
                                // is NOT saved" reads at a glance.
                                ImGui::TableSetColumnIndex(overrideCol);

                                auto pairKey = std::make_pair(p.fontId, p.instOrWave);
                                bool isSoloed       = sSoloedPairs.count(pairKey) > 0;
                                bool isExplicitMuted = sExplicitMutedPairs.count(pairKey) > 0;

                                // Solo button. Multi-row solo: clicking adds to / removes
                                // from the set. The pre-frame apply pass (just above the
                                // for-loop) reads sSoloedPairs and pushes effective mute
                                // state to the translator.
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                                      isSoloed ? ImVec4(0.85f, 0.45f, 0.10f, 1.00f)
                                                               : ImVec4(0.35f, 0.20f, 0.05f, 1.00f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.20f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.65f, 0.30f, 1.0f));
                                if (ImGui::SmallButton(isSoloed ? "Unsolo##solo" : "Solo##solo")) {
                                    if (isSoloed) sSoloedPairs.erase(pairKey);
                                    else          sSoloedPairs.insert(pairKey);
                                }
                                ImGui::PopStyleColor(3);
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "%s\nSession-only - never saved.\n"
                                        "Multi-row Solo: click on multiple rows to play them all\n"
                                        "while every other pair is muted. Click again to remove a\n"
                                        "row from the soloed set; emptying it returns to normal\n"
                                        "playback. Pair with the Background Music tab's preview\n"
                                        "button to loop a sequence while you tune.",
                                        isSoloed ? "Click to remove this pair from the soloed set."
                                                 : "Click to add this pair to the soloed set.");
                                }

                                ImGui::SameLine();
                                if (effectiveIsNative) {
                                    // Native rows have no per-pair synth gain hook, so a temp
                                    // volume slider would be useless. Offer an explicit Mute
                                    // toggle instead — flips the pair in sExplicitMutedPairs,
                                    // applied via the translator's mTemporaryMute set, which
                                    // silences both native and synth paths for this row.
                                    ImGui::PushStyleColor(ImGuiCol_Button,
                                                          isExplicitMuted ? ImVec4(0.75f, 0.10f, 0.10f, 1.00f)
                                                                          : ImVec4(0.35f, 0.10f, 0.05f, 1.00f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.30f, 0.20f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.40f, 0.30f, 1.0f));
                                    if (ImGui::SmallButton(isExplicitMuted ? "Unmute##mute" : "Mute##mute")) {
                                        if (isExplicitMuted) sExplicitMutedPairs.erase(pairKey);
                                        else                 sExplicitMutedPairs.insert(pairKey);
                                    }
                                    ImGui::PopStyleColor(3);
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip(
                                            "%s\nSession-only - never saved.\nSilences this Native pair on\n"
                                            "both engine and synth paths. Native rows have no per-pair gain\n"
                                            "hook so we use a toggle here instead of a volume slider; the\n"
                                            "Synth rows below get a 0..4x volume slider in this slot.",
                                            isExplicitMuted ? "Click to unmute." : "Click to mute this pair.");
                                    }
                                } else {
                                    // Synth row: temp volume slider (0..4x).
                                    // 0.0 acts as a mute (NoteOn velocity / CC11 both reach 0)
                                    // so Synth rows don't need a separate Mute button.
                                    float vol = SOH::MidiTranslator::Instance().GetTemporaryVolume(
                                        p.fontId, p.instOrWave);
                                    float volDisplay = vol;
                                    ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.30f, 0.18f, 0.05f, 0.80f));
                                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.45f, 0.25f, 0.07f, 0.90f));
                                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.60f, 0.35f, 0.10f, 1.00f));
                                    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.95f, 0.55f, 0.20f, 1.00f));
                                    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.70f, 0.35f, 1.00f));
                                    ImGui::SetNextItemWidth(75.0f);
                                    if (ImGui::DragFloat("##tempVol", &volDisplay, 0.01f,
                                                         0.0f, 4.0f,
                                                         (vol == 1.0f) ? "vol -" : "vol %.2f")) {
                                        if (volDisplay < 0.0f) volDisplay = 0.0f;
                                        if (volDisplay > 4.0f) volDisplay = 4.0f;
                                        SOH::MidiTranslator::Instance().SetTemporaryVolume(
                                            p.fontId, p.instOrWave, volDisplay);
                                    }
                                    ImGui::PopStyleColor(5);
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip(
                                            "Temporary volume multiplier (0..4x).\n"
                                            "Session-only - never saved. Stacks on top of the\n"
                                            "persisted Gain column. Drag to 0 to mute this row,\n"
                                            "1.0 (\"vol -\") = no change. Native rows show a Mute\n"
                                            "toggle in this slot instead.");
                                    }
                                }

                                ImGui::TableSetColumnIndex(songCol);
                                // Plain text — the font name is almost always accurate
                                // (per-font names come from runtime fontMap[]). The custom
                                // display_name lives in the Sample column instead, where
                                // it actually replaces something that's often empty or
                                // useless. See the InputTextWithHint below.
                                {
                                    const char* font = SOH::GetFontName(p.fontId);
                                    ImGui::TextUnformatted(font ? font : "(modded font)");
                                }

                                // Sample column: shows the SF2 sample names captured at
                                // load time (L / M / H range splits, deduped when they
                                // match). Many music-font slots have no captured name so
                                // this column was often empty pre-Ship-3. Modders now type
                                // their own label here; the auto name becomes the hint
                                // (visible only when the override is empty) so a slot with
                                // a useful auto name still reads well untouched.
                                //
                                // Keyed by (fontId, instOrWave) — same as before — so a
                                // different fontId using the same instOrWave can carry a
                                // different name. The hint is built fresh each frame from
                                // the SF2 sample dataset, the override comes from the
                                // translator's sparse mDisplayName map and is persisted
                                // alongside the rest of the row.
                                ImGui::TableSetColumnIndex(sampleCol);
                                {
                                    // Build the "auto" sample-name label that doubles as
                                    // the hint placeholder.
                                    char autoBuf[256];
                                    autoBuf[0] = '\0';
                                    {
                                        auto names = SOH::GetInstrumentSampleNames(p.fontId, p.instOrWave);
                                        if (!names.empty()) {
                                            const char* lowName    = SOH::StripSamplePathPrefix(names.low);
                                            const char* normalName = SOH::StripSamplePathPrefix(names.normal);
                                            const char* highName   = SOH::StripSamplePathPrefix(names.high);
                                            auto isSame = [](const char* a, const char* b) {
                                                return *a && *b && std::strcmp(a, b) == 0;
                                            };
                                            const bool lEmpty = names.low.empty();
                                            const bool nEmpty = names.normal.empty();
                                            const bool hEmpty = names.high.empty();
                                            const bool allMatch =
                                                (lEmpty || nEmpty || isSame(lowName, normalName)) &&
                                                (nEmpty || hEmpty || isSame(normalName, highName)) &&
                                                (lEmpty || hEmpty || isSame(lowName, highName));
                                            if (allMatch) {
                                                const char* shown = !nEmpty ? normalName
                                                                  : !lEmpty ? lowName
                                                                            : highName;
                                                char tag[5] = "(";
                                                int t = 1;
                                                if (!lEmpty) tag[t++] = 'L';
                                                if (!nEmpty) tag[t++] = 'M';
                                                if (!hEmpty) tag[t++] = 'H';
                                                tag[t++] = ')';
                                                tag[t]   = '\0';
                                                std::snprintf(autoBuf, sizeof(autoBuf), "%s %s", shown, tag);
                                            } else {
                                                size_t pos = 0;
                                                auto append = [&](const char* prefix, const char* val) {
                                                    if (!*val) return;
                                                    int written = std::snprintf(autoBuf + pos, sizeof(autoBuf) - pos,
                                                                                "%s%s:%s",
                                                                                pos == 0 ? "" : " ",
                                                                                prefix, val);
                                                    if (written > 0) pos += static_cast<size_t>(written);
                                                };
                                                append("L", SOH::StripSamplePathPrefix(names.low));
                                                append("M", SOH::StripSamplePathPrefix(names.normal));
                                                append("H", SOH::StripSamplePathPrefix(names.high));
                                            }
                                        }
                                    }
                                    const char* hint = autoBuf[0] ? autoBuf : "(no sample name)";

                                    const std::string current =
                                        SOH::MidiTranslator::Instance().GetDisplayName(p.fontId, p.instOrWave);
                                    char buf[80];
                                    std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
                                    buf[sizeof(buf) - 1] = '\0';
                                    ImGui::SetNextItemWidth(-FLT_MIN);
                                    if (ImGui::InputTextWithHint("##displayName", hint, buf, sizeof(buf))) {
                                        SOH::MidiTranslator::Instance().SetDisplayName(
                                            p.fontId, p.instOrWave, std::string(buf));
                                    }
                                    if (ImGui::IsItemDeactivatedAfterEdit()) AutoSaveOverrides();
                                    if (ImGui::IsItemHovered()) {
                                        // Always show the engine's three per-range samples,
                                        // whether or not a custom label is set. The custom
                                        // label hides what the engine actually plays per
                                        // pitch range; the tooltip is where the user reads
                                        // back the raw mapping.
                                        auto names = SOH::GetInstrumentSampleNames(p.fontId, p.instOrWave);
                                        const char* lowName    = SOH::StripSamplePathPrefix(names.low);
                                        const char* normalName = SOH::StripSamplePathPrefix(names.normal);
                                        const char* highName   = SOH::StripSamplePathPrefix(names.high);
                                        ImGui::SetTooltip(
                                            "Hi:  %s\nMid: %s\nLow: %s",
                                            (highName   && *highName)   ? highName   : "(empty)",
                                            (normalName && *normalName) ? normalName : "(empty)",
                                            (lowName    && *lowName)    ? lowName    : "(empty)");
                                    }
                                }

                                ImGui::TableSetColumnIndex(fontCol);
                                ImGui::Text("%u", (unsigned)p.fontId);

                                ImGui::TableSetColumnIndex(instCol);
                                ImGui::Text("%d (0x%02X)", (int)p.instOrWave, (unsigned)(uint8_t)p.instOrWave);

                                ImGui::TableSetColumnIndex(modeCol);
                                // Native click disables every enabled entry for this pair
                                // (keeps their selected flag so ClickSynth can restore).
                                // Synth click re-enables: most-recently-enabled selected
                                // entry wins, falls back to any disabled-but-resolvable
                                // entry (mod-only-row case), last resort is a muted
                                // "None" placeholder. See MidiTranslator::ClickSynth.
                                if (ImGui::RadioButton("Native##bypass", effectiveIsNative)) {
                                    SOH::MidiTranslator::Instance().ClickNative(
                                        p.fontId, p.instOrWave);
                                    AutoSaveOverrides();
                                }
                                ImGui::SameLine();
                                if (ImGui::RadioButton("Synth##bypass", !effectiveIsNative)) {
                                    SOH::MidiTranslator::Instance().ClickSynth(
                                        p.fontId, p.instOrWave);
                                    AutoSaveOverrides();
                                }

                                // The per-entry editors (Gain, Shift, effect CCs) operate
                                // on the active entry; when there is none, nothing to
                                // edit so we grey them out. The Preset combo stays live
                                // so the user can pick a preset to leave Native mode.
                                ImGui::BeginDisabled(effectiveIsNative);
                                auto disabledTooltipIfNative = [&]() {
                                    if (effectiveIsNative &&
                                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                        ImGui::SetTooltip(
                                            "No active entry on this row. Gain / Shift /\n"
                                            "effects edit the resolved entry's fields, so\n"
                                            "they have nothing to operate on. Pick a preset\n"
                                            "below or click Synth to restore the most\n"
                                            "recent pick.");
                                    }
                                };

                                ImGui::TableSetColumnIndex(gainCol);
                                float gainStored = activeEntry ? activeEntry->gain : 0.0f;
                                float gainShown = (gainStored == 0.0f) ? 1.0f : gainStored;
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::SliderFloat("##gain", &gainShown, 0.0f, 4.0f, "%.2f")) {
                                    if (activeIdx >= 0) {
                                        SOH::MidiTranslator::Instance().SetEntryGain(
                                            activeIdx, gainShown);
                                    }
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit()) AutoSaveOverrides();
                                disabledTooltipIfNative();

                                ImGui::TableSetColumnIndex(shiftCol);
                                int transStored = activeEntry ? activeEntry->transpose : 0;

                                // Decompose the stored semitone offset into a coarse octave
                                // count + a leftover remainder in [-11..+11]. Using truncate-
                                // toward-zero (the language default for int /) keeps the
                                // remainder's sign matched to the input — e.g. -13 → -1 oct,
                                // -1 st (NOT -2 oct, +11 st).
                                int curOctaves   = transStored / 12;
                                int curRemainder = transStored - curOctaves * 12;
                                int displayValue;
                                int displayMin, displayMax;
                                if (transSemis) {
                                    displayValue = transStored;
                                    displayMin   = -24;
                                    displayMax   =  24;
                                } else {
                                    displayValue = curOctaves;
                                    displayMin   = -8;
                                    displayMax   =  8;
                                }

                                // Tint the cell orange when in octave mode AND there's a
                                // non-zero remainder — the displayed integer doesn't tell
                                // the full story so the user needs a visual cue (tooltip
                                // carries the exact remainder). Subtle alpha so it doesn't
                                // clash with the row's synth/native tint.
                                const bool hasRemainder = (!transSemis && curRemainder != 0);
                                if (hasRemainder) {
                                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                                           IM_COL32(220, 150, 60, 70));
                                }

                                // Build a format string that includes the remainder hint in
                                // octave mode so the user sees "+2 oct (+5 st)" inline.
                                // In semitone mode just show the raw "+N st".
                                char fmt[32];
                                if (transSemis) {
                                    std::strcpy(fmt, "%+d st");
                                } else if (hasRemainder) {
                                    std::snprintf(fmt, sizeof(fmt), "%%+d oct (%+d st)", curRemainder);
                                } else {
                                    std::strcpy(fmt, "%+d oct");
                                }

                                ImGui::SetNextItemWidth(-FLT_MIN);
                                if (ImGui::DragInt("##trans", &displayValue, 0.1f,
                                                   displayMin, displayMax, fmt)) {
                                    displayValue = std::clamp(displayValue, displayMin, displayMax);
                                    int newSemis;
                                    if (transSemis) {
                                        // Direct semitone control — straight assignment.
                                        newSemis = displayValue;
                                    } else {
                                        // Octave control — apply a delta in multiples of 12
                                        // and preserve the remainder so toggling between
                                        // octave/semitone display doesn't silently drop the
                                        // fine offset.
                                        int deltaOctaves = displayValue - curOctaves;
                                        newSemis = transStored + deltaOctaves * 12;
                                    }
                                    newSemis = std::clamp(newSemis, -127, 127);
                                    if (activeIdx >= 0) {
                                        SOH::MidiTranslator::Instance().SetEntryTranspose(
                                            activeIdx, static_cast<int8_t>(newSemis));
                                    }
                                }
                                if (ImGui::IsItemDeactivatedAfterEdit()) AutoSaveOverrides();
                                if (ImGui::IsItemHovered()) {
                                    if (transSemis) {
                                        ImGui::SetTooltip(
                                            "Shift this pair's notes by +/-24 semitones (fine).\n"
                                            "Stored value: %+d st. 0 = no shift. Drums skip this column.",
                                            (int)transStored);
                                    } else if (hasRemainder) {
                                        ImGui::SetTooltip(
                                            "Shift this pair's notes by +/-8 octaves (whole-scale).\n"
                                            "Stored value: %+d st = %+d oct %+d st leftover.\n"
                                            "The leftover semitone offset is preserved when you\n"
                                            "drag here - octaves move by +/-12 around it. Enable\n"
                                            "Semitone precision above to edit the leftover.\n"
                                            "(Cell tinted to flag the leftover.)",
                                            (int)transStored, curOctaves, curRemainder);
                                    } else {
                                        ImGui::SetTooltip(
                                            "Shift this pair's notes by +/-8 octaves (whole-scale).\n"
                                            "Stored value: %+d st. 0 = no shift. Enable Semitone\n"
                                            "precision above for +/-24 st fine control.",
                                            (int)transStored);
                                    }
                                }
                                disabledTooltipIfNative();

                                // ── Source column ─────────────────────────
                                // Reflects the current resolution winner. When there's no
                                // active entry but there are selected entries (user picks
                                // whose source pack isn't loaded), surface the most
                                // recently enabled one as "[missing] [Pack]" so the user
                                // sees WHY the row went native.
                                ImGui::TableSetColumnIndex(sourceCol);
                                const SOH::ConfigEntry* fallbackSaved = nullptr;
                                if (!activeEntry) {
                                    std::vector<int> idxs;
                                    SOH::MidiTranslator::Instance().GetEntriesForPair(
                                        p.fontId, p.instOrWave, idxs);
                                    uint32_t bestSeq = 0;
                                    for (int i2 : idxs) {
                                        const SOH::ConfigEntry& e =
                                            SOH::MidiTranslator::Instance().GetEntry(i2);
                                        if (!e.selected) continue;
                                        if (!fallbackSaved || e.lastEnabledSeq >= bestSeq) {
                                            fallbackSaved = &e;
                                            bestSeq = e.lastEnabledSeq;
                                        }
                                    }
                                }
                                if (activeEntry) {
                                    const ImVec4 kLive(0.55f, 0.90f, 0.55f, 1.0f);
                                    if (activeEntry->packName.empty()) {
                                        ImGui::TextDisabled("[None]");
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Placeholder entry: synth path is muted, native\n"
                                                "is suppressed too. Created by clicking Synth\n"
                                                "with no prior pick available.");
                                        }
                                    } else {
                                        ImGui::TextColored(kLive, "[%s] B%d",
                                                           activeEntry->packName.c_str(),
                                                           activeEntry->bank);
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Active entry: [%s] B%d P%d: %s.\n"
                                                "Resolution picks the enabled candidate whose\n"
                                                "pack is last in the load order; later packs\n"
                                                "win on (font, inst) collisions.",
                                                activeEntry->packName.c_str(),
                                                activeEntry->bank, activeEntry->program,
                                                activeEntry->presetName.c_str());
                                        }
                                    }
                                } else if (fallbackSaved) {
                                    // Two flavours of "row is Native":
                                    //   sfontId >= 0 → user clicked Native (pack still loaded,
                                    //                  entry just disabled). Show the pack
                                    //                  greyed out so the user knows what
                                    //                  Synth-click would restore.
                                    //   sfontId <  0 → source pack isn't loaded right now.
                                    //                  Red [missing] to call it out.
                                    if (fallbackSaved->sfontId >= 0) {
                                        ImGui::TextDisabled("[%s] B%d (off)",
                                                            fallbackSaved->packName.empty()
                                                                ? "?"
                                                                : fallbackSaved->packName.c_str(),
                                                            fallbackSaved->bank);
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Disabled: [%s] B%d P%d: %s.\n"
                                                "Saved as your most recent pick for this row.\n"
                                                "Click Synth to re-enable it.",
                                                fallbackSaved->packName.c_str(),
                                                fallbackSaved->bank, fallbackSaved->program,
                                                fallbackSaved->presetName.c_str());
                                        }
                                    } else {
                                        const ImVec4 kDead(0.95f, 0.50f, 0.50f, 1.0f);
                                        ImGui::TextColored(kDead, "[missing] %s",
                                                           fallbackSaved->packName.empty()
                                                               ? "?"
                                                               : fallbackSaved->packName.c_str());
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Most recent pick: [%s] B%d P%d: %s.\n"
                                                "Source pack isn't loaded right now, so the row\n"
                                                "plays native. Re-enable the pack or pick a new\n"
                                                "preset to bring synth back.",
                                                fallbackSaved->packName.c_str(),
                                                fallbackSaved->bank, fallbackSaved->program,
                                                fallbackSaved->presetName.c_str());
                                        }
                                    }
                                } else {
                                    ImGui::TextDisabled("-");
                                }

                                // Preset combo stays live even when the row is in Native
                                // mode — picking a preset is the user's path back to
                                // Synth (PickPreset disables every other entry for this
                                // pair, finds/creates the picked entry, enables+selects
                                // it). The Default item maps to ClickNative semantics.
                                ImGui::EndDisabled();

                                ImGui::TableSetColumnIndex(presetCol);
                                char defaultLabel[160];
                                if (defaultGmForMode.program == SOH::kUnmapped) {
                                    std::strcpy(defaultLabel, "Default: None");
                                } else if (defaultGmForMode.bank == 128) {
                                    std::snprintf(defaultLabel, sizeof(defaultLabel),
                                                  "Default: drum kit %u, slot %u",
                                                  (unsigned)defaultGmForMode.program,
                                                  (unsigned)defaultGmForMode.drumNote);
                                } else {
                                    std::snprintf(defaultLabel, sizeof(defaultLabel),
                                                  "Default: %u: %s",
                                                  (unsigned)defaultGmForMode.program,
                                                  SOH::kGmProgramNames[defaultGmForMode.program]);
                                }

                                char prgPreview[200];
                                if (activeEntry) {
                                    if (activeEntry->packName.empty()) {
                                        std::strcpy(prgPreview, "(None)");
                                    } else {
                                        std::snprintf(prgPreview, sizeof(prgPreview),
                                                      "P%d: %s",
                                                      activeEntry->program,
                                                      activeEntry->presetName.c_str());
                                    }
                                } else if (fallbackSaved && fallbackSaved->sfontId >= 0) {
                                    // Pack still loaded; user just clicked Native. Show
                                    // the saved preset name so the user remembers what
                                    // Synth-click would restore.
                                    std::snprintf(prgPreview, sizeof(prgPreview),
                                                  "(off) P%d: %s",
                                                  fallbackSaved->program,
                                                  fallbackSaved->presetName.c_str());
                                } else if (fallbackSaved) {
                                    std::snprintf(prgPreview, sizeof(prgPreview),
                                                  "(B%d P%d not loaded)",
                                                  fallbackSaved->bank,
                                                  fallbackSaved->program);
                                } else {
                                    std::strcpy(prgPreview, defaultLabel);
                                }
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                if (ImGui::BeginCombo("##prgCombo", prgPreview,
                                                      ImGuiComboFlags_HeightLargest)) {
                                    static char prgFilter[64] = "";
                                    if (ImGui::IsWindowAppearing()) {
                                        prgFilter[0] = '\0';
                                        ImGui::SetKeyboardFocusHere();
                                    }
                                    ImGui::SetNextItemWidth(-FLT_MIN);
                                    ImGui::InputTextWithHint(
                                        "##prgFilter", "Filter (preset or pack name)",
                                        prgFilter, sizeof(prgFilter));
                                    ImGui::Separator();

                                    const bool filterActive = prgFilter[0] != '\0';
                                    auto containsCi = [](const std::string& hay,
                                                         const char* needle) -> bool {
                                        if (!needle || !*needle) return true;
                                        const size_t nlen = std::strlen(needle);
                                        if (nlen > hay.size()) return false;
                                        auto it = std::search(
                                            hay.begin(), hay.end(),
                                            needle, needle + nlen,
                                            [](char a, char b) {
                                                return std::tolower(static_cast<unsigned char>(a)) ==
                                                       std::tolower(static_cast<unsigned char>(b));
                                            });
                                        return it != hay.end();
                                    };

                                    if (!filterActive &&
                                        ImGui::Selectable(defaultLabel, activeEntry == nullptr)) {
                                        // "Default" in the new model = disable every enabled
                                        // entry for this pair (ClickNative). selected flags
                                        // are preserved so the user can click Synth to
                                        // restore. Picking a real preset below promotes it
                                        // to selected; the user has to explicitly switch.
                                        SOH::MidiTranslator::Instance().ClickNative(
                                            p.fontId, p.instOrWave);
                                        AutoSaveOverrides();
                                    }

                                    int lastSfont = -2;
                                    int shown = 0;
                                    for (const auto& lp : sLoadedPresets) {
                                        if (filterActive &&
                                            !containsCi(lp.name, prgFilter) &&
                                            !containsCi(lp.packName, prgFilter)) {
                                            continue;
                                        }
                                        if (lp.sfontId != lastSfont) {
                                            ImGui::Separator();
                                            ImGui::TextDisabled("%s", lp.packName.c_str());
                                            lastSfont = lp.sfontId;
                                        }
                                        char item[256];
                                        std::snprintf(item, sizeof(item), "B%d P%d: %s##%d:%d:%d",
                                                      lp.bank, lp.program, lp.name.c_str(),
                                                      lp.sfontId, lp.bank, lp.program);
                                        bool sel = activeEntry &&
                                                   activeEntry->packName == lp.packName &&
                                                   activeEntry->bank == lp.bank &&
                                                   activeEntry->program == lp.program;
                                        if (ImGui::Selectable(item, sel)) {
                                            SOH::MidiTranslator::Instance().PickPreset(
                                                p.fontId, p.instOrWave,
                                                lp.packName,
                                                static_cast<int16_t>(lp.program),
                                                static_cast<int16_t>(lp.bank),
                                                lp.name);
                                            AutoSaveOverrides();
                                        }
                                        shown++;
                                    }
                                    if (sLoadedPresets.empty()) {
                                        ImGui::Separator();
                                        ImGui::TextDisabled("(no SF2 presets loaded)");
                                    } else if (filterActive && shown == 0) {
                                        ImGui::Separator();
                                        ImGui::TextDisabled("(no matches for \"%s\")", prgFilter);
                                    }
                                    ImGui::EndCombo();
                                }
                                if (ImGui::IsItemHovered()) {
                                    std::string tip =
                                        "Pick a preset from any loaded SF2. Selecting creates\n"
                                        "or reuses an entry for (font, inst, pack, program),\n"
                                        "marks it selected, and disables any other enabled\n"
                                        "entries for this pair. The pack + preset name are\n"
                                        "persisted so the choice survives an SF2 stack change.\n"
                                        "Default = disable enabled entries (row goes native).";
                                    if (fallbackSaved) {
                                        tip += "\n\nMost recent pick (not currently loaded):\n  ";
                                        tip += fallbackSaved->packName;
                                        tip += " / ";
                                        tip += fallbackSaved->presetName;
                                    }
                                    ImGui::SetTooltip("%s", tip.c_str());
                                }

                                // Re-enter the disabled-when-native scope for the effect
                                // cells below.
                                ImGui::BeginDisabled(effectiveIsNative);

                                // Effect cells operate on the active entry. Each is a
                                // compact DragInt with -1 sentinel = "no override" (synth
                                // uses its channel default).
                                auto drawEffectCell = [&](int colIdx, const char* widgetId,
                                                          int8_t current,
                                                          void (SOH::MidiTranslator::*setter)(int, int8_t),
                                                          const char* tip) {
                                    ImGui::TableSetColumnIndex(colIdx);
                                    int display = current;
                                    ImGui::SetNextItemWidth(58.0f);
                                    if (ImGui::DragInt(widgetId, &display, 0.5f, -1, 127,
                                                       current < 0 ? "off" : "%d")) {
                                        if (display < -1)  display = -1;
                                        if (display > 127) display = 127;
                                        if (activeIdx >= 0) {
                                            (SOH::MidiTranslator::Instance().*setter)(
                                                activeIdx, static_cast<int8_t>(display));
                                        }
                                    }
                                    if (ImGui::IsItemDeactivatedAfterEdit()) AutoSaveOverrides();
                                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                                    disabledTooltipIfNative();
                                };

                                drawEffectCell(
                                    reverbCol, "##reverb",
                                    activeEntry ? activeEntry->reverb : int8_t(-1),
                                    &SOH::MidiTranslator::SetEntryReverb,
                                    "Reverb send (CC91). 0-127 sends increasing amounts of this\n"
                                    "pair's voice through the synth's reverb. Drag below 0 to\n"
                                    "clear the override and restore the channel default.");
                                drawEffectCell(
                                    chorusCol, "##chorus",
                                    activeEntry ? activeEntry->chorus : int8_t(-1),
                                    &SOH::MidiTranslator::SetEntryChorus,
                                    "Chorus send (CC93). 0-127 sends increasing amounts of this\n"
                                    "pair's voice through the synth's chorus. Drag below 0 to\n"
                                    "clear the override and restore the channel default.");
                                drawEffectCell(
                                    cutoffCol, "##cutoff",
                                    activeEntry ? activeEntry->cutoff : int8_t(-1),
                                    &SOH::MidiTranslator::SetEntryFilterCutoff,
                                    "Low-pass filter cutoff (CC74). 64 = no shift from the\n"
                                    "SF2 default; lower darkens, higher brightens. Behaviour\n"
                                    "depends on whether the SF2 author routed CC74 to the\n"
                                    "initial filter Fc generator. Drag below 0 to clear.");
                                drawEffectCell(
                                    qCol, "##fq",
                                    activeEntry ? activeEntry->q : int8_t(-1),
                                    &SOH::MidiTranslator::SetEntryFilterResonance,
                                    "Filter resonance / Q (CC71). 64 = no shift from the SF2\n"
                                    "default; higher emphasises the cutoff frequency. Routing\n"
                                    "depends on the SF2 author. Drag below 0 to clear.");

                                ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTable();
            ImGui::PopStyleVar(1);
            ImGui::EndTabItem();
        }
#endif // #if ENABLE_FLUIDSYNTH

        if (ImGui::BeginTabItem("Background Music")) {
            Draw_SfxTab("backgroundMusic", SEQ_BGM_WORLD, "Background Music");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fanfares")) {
            Draw_SfxTab("fanfares", SEQ_FANFARE, "Fanfares");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Events")) {
            Draw_SfxTab("event", SEQ_BGM_EVENT, "Events");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Battle Music")) {
            Draw_SfxTab("battleMusic", SEQ_BGM_BATTLE, "Battle Music");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Ending")) {
            Draw_SfxTab("ending", SEQ_ENDING, "Ending");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Ocarina")) {
            Draw_SfxTab("instrument", SEQ_INSTRUMENT, "Instruments");
            Draw_SfxTab("ocarina", SEQ_OCARINA, "Ocarina");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sound Effects")) {
            Draw_SfxTab("sfx", SEQ_SFX, "Sound Effects");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voices")) {
            Draw_SfxTab("voice", SEQ_VOICE, "Voices");
            ImGui::EndTabItem();
        }

        static bool excludeTabOpen = false;
        if (ImGui::BeginTabItem("Audio Shuffle Pool Management")) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
            if (!excludeTabOpen) {
                excludeTabOpen = true;
            }

            static std::map<SeqType, bool> showType{
                { SEQ_BGM_WORLD, true }, { SEQ_BGM_EVENT, true },  { SEQ_BGM_BATTLE, true },
                { SEQ_OCARINA, true },   { SEQ_FANFARE, true },    { SEQ_SFX, true },
                { SEQ_VOICE, true },     { SEQ_INSTRUMENT, true }, { SEQ_BGM_CUSTOM, true },
            };

            // make temporary sets because removing from the set we're iterating through crashes ImGui
            std::set<SequenceInfo*> seqsToInclude = {};
            std::set<SequenceInfo*> seqsToExclude = {};

            static ImGuiTextFilter sequenceSearch;
            UIWidgets::PushStyleInput(THEME_COLOR);
            sequenceSearch.Draw("Filter (inc,-exc)", 490.0f);
            UIWidgets::PopStyleInput();
            ImGui::SameLine();
            if (UIWidgets::Button("Exclude All",
                                  UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
                for (auto seqInfo : AudioCollection::Instance->GetIncludedSequences()) {
                    if (sequenceSearch.PassFilter(seqInfo->label.c_str()) && showType[seqInfo->category]) {
                        seqsToExclude.insert(seqInfo);
                    }
                }
            }
            ImGui::SameLine();
            if (UIWidgets::Button("Include All",
                                  UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(THEME_COLOR))) {
                for (auto seqInfo : AudioCollection::Instance->GetExcludedSequences()) {
                    if (sequenceSearch.PassFilter(seqInfo->label.c_str()) && showType[seqInfo->category]) {
                        seqsToInclude.insert(seqInfo);
                    }
                }
            }

            ImGui::BeginTable("sequenceTypes", 9,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_BGM_WORLD));
            ImGui::Selectable(GetSequenceTypeName(SEQ_BGM_WORLD).c_str(), &showType[SEQ_BGM_WORLD]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_BGM_EVENT));
            ImGui::Selectable(GetSequenceTypeName(SEQ_BGM_EVENT).c_str(), &showType[SEQ_BGM_EVENT]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_BGM_BATTLE));
            ImGui::Selectable(GetSequenceTypeName(SEQ_BGM_BATTLE).c_str(), &showType[SEQ_BGM_BATTLE]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_OCARINA));
            ImGui::Selectable(GetSequenceTypeName(SEQ_OCARINA).c_str(), &showType[SEQ_OCARINA]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_FANFARE));
            ImGui::Selectable(GetSequenceTypeName(SEQ_FANFARE).c_str(), &showType[SEQ_FANFARE]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_SFX));
            ImGui::Selectable(GetSequenceTypeName(SEQ_SFX).c_str(), &showType[SEQ_SFX]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_VOICE));
            ImGui::Selectable(GetSequenceTypeName(SEQ_VOICE).c_str(), &showType[SEQ_VOICE]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_INSTRUMENT));
            ImGui::Selectable(GetSequenceTypeName(SEQ_INSTRUMENT).c_str(), &showType[SEQ_INSTRUMENT]);
            ImGui::PopStyleColor(1);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Header, GetSequenceTypeColor(SEQ_BGM_CUSTOM));
            ImGui::Selectable(GetSequenceTypeName(SEQ_BGM_CUSTOM).c_str(), &showType[SEQ_BGM_CUSTOM]);
            ImGui::PopStyleColor(1);

            ImGui::EndTable();

            if (ImGui::BeginTable("tableAllSequences", 2, ImGuiTableFlags_BordersH | ImGuiTableFlags_BordersV)) {
                ImGui::TableSetupColumn("Included", ImGuiTableColumnFlags_WidthStretch, 200.0f);
                ImGui::TableSetupColumn("Excluded", ImGuiTableColumnFlags_WidthStretch, 200.0f);
                ImGui::TableHeadersRow();
                ImGui::TableNextRow();

                // COLUMN 1 - INCLUDED SEQUENCES
                ImGui::TableNextColumn();

                ImGui::BeginChild("ChildIncludedSequences", ImVec2(0, -8));
                for (auto seqInfo : AudioCollection::Instance->GetIncludedSequences()) {
                    if (sequenceSearch.PassFilter(seqInfo->label.c_str()) && showType[seqInfo->category]) {
                        if (UIWidgets::Button(std::string(ICON_FA_TIMES "##" + seqInfo->sfxKey).c_str(),
                                              UIWidgets::ButtonOptions()
                                                  .Size(UIWidgets::Sizes::Inline)
                                                  .Padding(ImVec2(9.0f, 6.0f))
                                                  .Color(THEME_COLOR))) {
                            seqsToExclude.insert(seqInfo);
                        }
                        ImGui::SameLine();
                        DrawPreviewButton(seqInfo->sequenceId, seqInfo->sfxKey, seqInfo->category);
                        ImGui::SameLine();
                        DrawTypeChip(seqInfo->category, seqInfo->label);
                        ImGui::SameLine();
                        ImGui::Text("%s", seqInfo->label.c_str());
                    }
                }
                ImGui::EndChild();

                // remove the sequences we added to the temp set
                for (auto seqInfo : seqsToExclude) {
                    AudioCollection::Instance->RemoveFromShufflePool(seqInfo);
                }

                // COLUMN 2 - EXCLUDED SEQUENCES
                ImGui::TableNextColumn();

                ImGui::BeginChild("ChildExcludedSequences", ImVec2(0, -8));
                for (auto seqInfo : AudioCollection::Instance->GetExcludedSequences()) {
                    if (sequenceSearch.PassFilter(seqInfo->label.c_str()) && showType[seqInfo->category]) {
                        if (UIWidgets::Button(std::string(ICON_FA_PLUS "##" + seqInfo->sfxKey).c_str(),
                                              UIWidgets::ButtonOptions()
                                                  .Size(UIWidgets::Sizes::Inline)
                                                  .Padding(ImVec2(9.0f, 6.0f))
                                                  .Color(THEME_COLOR))) {
                            seqsToInclude.insert(seqInfo);
                        }
                        ImGui::SameLine();
                        DrawPreviewButton(seqInfo->sequenceId, seqInfo->sfxKey, seqInfo->category);
                        ImGui::SameLine();
                        DrawTypeChip(seqInfo->category, seqInfo->sfxKey);
                        ImGui::SameLine();
                        ImGui::Text("%s", seqInfo->label.c_str());
                    }
                }
                ImGui::EndChild();

                // add the sequences we added to the temp set
                for (auto seqInfo : seqsToInclude) {
                    AudioCollection::Instance->AddToShufflePool(seqInfo);
                }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar(1);
            ImGui::EndTabItem();
        } else {
            excludeTabOpen = false;
        }

        ImGui::EndTabBar();
    }
    UIWidgets::PopStyleTabs();
}

std::vector<SeqType> allTypes = {
    SEQ_BGM_WORLD,  SEQ_BGM_EVENT, SEQ_BGM_BATTLE, SEQ_OCARINA, SEQ_FANFARE,
    SEQ_INSTRUMENT, SEQ_SFX,       SEQ_VOICE,      SEQ_ENDING,
};

void AudioEditor_RandomizeAll() {
    for (auto type : allTypes) {
        RandomizeGroup(type);
    }

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    ReplayCurrentBGM();
}

void AudioEditor_AutoRandomizeAll() {
    for (auto type : allTypes) {
        RandomizeGroup(type, false);
    }

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    ReplayCurrentBGM();
}

void AudioEditor_RandomizeGroup(SeqType group) {
    RandomizeGroup(group);

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    ReplayCurrentBGM();
}

void AudioEditor_ResetAll() {
    for (auto type : allTypes) {
        ResetGroup(AudioCollection::Instance->GetAllSequences(), type);
    }

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    ReplayCurrentBGM();
}

void AudioEditor_ResetGroup(SeqType group) {
    ResetGroup(AudioCollection::Instance->GetAllSequences(), group);

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    ReplayCurrentBGM();
}

void AudioEditor_LockAll() {
    for (auto type : allTypes) {
        LockGroup(AudioCollection::Instance->GetAllSequences(), type);
    }

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

void AudioEditor_UnlockAll() {
    for (auto type : allTypes) {
        UnlockGroup(AudioCollection::Instance->GetAllSequences(), type);
    }

    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

void RegisterAudioWidgets() {
    lowHpAlarm = { .name = "Mute Low HP Alarm", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    lowHpAlarm.CVar(CVAR_AUDIO("LowHpAlarm"))
        .Options(CheckboxOptions().Color(THEME_COLOR).Tooltip("Disable the low HP beeping sound."));
    SohGui::mSohMenu->AddSearchWidget({ lowHpAlarm, "Enhancements", "Audio Editor", "Audio Options" });

    naviCall = { .name = "Disable Navi Call Audio", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    naviCall.CVar(CVAR_AUDIO("DisableNaviCallAudio"))
        .Options(CheckboxOptions().Color(THEME_COLOR).Tooltip("Disables the voice audio when Navi calls you."));
    SohGui::mSohMenu->AddSearchWidget({ naviCall, "Enhancements", "Audio Editor", "Audio Options" });

    enemyProx = { .name = "Disable Enemy Proximity Music", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    enemyProx.CVar(CVAR_AUDIO("EnemyBGMDisable"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Disables the music change when getting close to enemies. Useful for hearing "
                              "your custom music for each scene more often."));

    leeverProx = { .name = "Enable Enemy Proximity Music for Leever", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    leeverProx.CVar(CVAR_AUDIO("LeeverEnemyBGM"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Plays the battle music when getting close to a Leever, like in Majora's Mask."));

    leadingMusic = { .name = "Disable Leading Music in Lost Woods", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    leadingMusic.CVar(CVAR_AUDIO("LostWoodsConsistentVolume"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Disables the volume shifting in the Lost Woods. Useful for hearing "
                              "your custom music in the Lost Woods if you don't need the navigation assitance "
                              "the volume changing provides. If toggling this while in the Lost Woods, reload "
                              "the area for the effect to kick in."));
    SohGui::mSohMenu->AddSearchWidget({ leadingMusic, "Enhancements", "Audio Editor", "Audio Options" });

    displaySeqName = { .name = "Display Sequence Name in Notifications", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    displaySeqName.CVar(CVAR_AUDIO("SeqNameNotification"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Emits a notification with the current song name whenever it changes. "
                              "(does not apply to fanfares or enemy BGM)."));
    SohGui::mSohMenu->AddSearchWidget({ displaySeqName, "Enhancements", "Audio Editor", "Audio Options" });

    ovlDuration = { .name = "Sequence Notification Duration: %d seconds", .type = WidgetType::WIDGET_CVAR_SLIDER_INT };
    ovlDuration.CVar(CVAR_AUDIO("SeqNameNotificationDuration"))
        .Options(IntSliderOptions().Color(THEME_COLOR).Min(1).Max(20).DefaultValue(10).Size(ImVec2(300.0f, 0.0f)));
    SohGui::mSohMenu->AddSearchWidget({ ovlDuration, "Enhancements", "Audio Editor", "Audio Options" });

    voicePitch = { .name = "Link's Voice Pitch Multiplier", .type = WidgetType::WIDGET_CVAR_SLIDER_FLOAT };
    voicePitch.CVar(CVAR_AUDIO("LinkVoiceFreqMultiplier"))
        .Options(FloatSliderOptions()
                     .Color(THEME_COLOR)
                     .IsPercentage()
                     .Min(0.4f)
                     .Max(2.5f)
                     .DefaultValue(1.0f)
                     .Size(ImVec2(300.0f, 0.0f)));
    SohGui::mSohMenu->AddSearchWidget({ voicePitch, "Enhancements", "Audio Editor", "Audio Options" });

    randomAudioGenModes = { .name = "Automatically Randomize All Music and Sound Effects",
                            .type = WidgetType::WIDGET_CVAR_COMBOBOX };
    randomAudioGenModes.CVar(CVAR_AUDIO("RandomizeAudioGenModes"))
        .Options(
            ComboboxOptions()
                .DefaultIndex(RANDOMIZE_OFF)
                .ComboMap(audioRandomizerModes)
                .Tooltip(
                    "Set when the music and sound effects is automaticly randomized:\n"
                    "- Manual: Manually randomize music or sound effects by pressing the 'Randomize all Groups' "
                    "button\n"
                    "- On New Scene : Randomizes when you enter a new scene.\n"
                    "- On Rando Gen Only: Randomizes only when you generate a new randomizer.\n"
                    "- On File Load: Randomizes on File Load.\n"
                    "- On File Load (Seeded): Randomizes on file load based on the current randomizer seed/file."));
    SohGui::mSohMenu->AddSearchWidget({ randomAudioGenModes, "Enhancements", "Audio Editor", "Audio Options" });

    lowerOctaves = { .name = "Lower Octaves of Unplayable High Notes", .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    lowerOctaves.CVar(CVAR_AUDIO("ExperimentalOctaveDrop"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Some custom sequences may have notes that are too high for the game's audio "
                              "engine to play. Enabling this checkbox will cause these notes to drop a "
                              "couple of octaves so they can still harmonize with the other notes of the "
                              "sequence."));
    SohGui::mSohMenu->AddSearchWidget({ lowerOctaves, "Enhancements", "Audio Editor", "Audio Options" });

#if ENABLE_FLUIDSYNTH
    fluidSynthEnabled = { .name = "Modern audio pipeline (floating point)",
                          .type = WidgetType::WIDGET_CVAR_CHECKBOX };
    fluidSynthEnabled.CVar(CVAR_AUDIO("ModernAudioPipeline"))
        .Options(CheckboxOptions()
                     .Color(THEME_COLOR)
                     .Tooltip("Run the audio path in 32-bit float instead of 16-bit integer.\n"
                              "Removes the resampler's s16 quantisation on its own.\n"
                              "Required for FluidSynth synth packs - see the FluidSynth tab\n"
                              "for pack selection and per-instrument tuning."));
    SohGui::mSohMenu->AddSearchWidget({ fluidSynthEnabled, "Enhancements", "Audio Editor", "Audio Options" });

    fluidSynthGain = { .name = "Synth volume", .type = WidgetType::WIDGET_CVAR_SLIDER_FLOAT };
    fluidSynthGain.CVar(CVAR_AUDIO("FluidSynthGain"))
        .Options(FloatSliderOptions()
                     .Color(THEME_COLOR)
                     .Min(0.0f)
                     .Max(4.0f)
                     .DefaultValue(1.0f)
                     .Size(ImVec2(300.0f, 0.0f))
                     .Tooltip("Per-pack synth output level. Multiplier on the sqrt(velocity)\n"
                              "curve sent as CC11 (expression). 1.0 = no boost (sqrt curve only).\n"
                              "Higher values lift quiet notes but saturate CC11 quickly, which\n"
                              "contributes to mix clipping."));
    SohGui::mSohMenu->AddSearchWidget({ fluidSynthGain, "Enhancements", "Audio Editor", "FluidSynth" });

    // Auto-apply at launch: if the user had the Modern audio pipeline
    // enabled in a previous session, switch the AudioPlayer into float
    // mode now and stack every enabled synth pack so the engine doesn't
    // keep playing on the s16 path until they open the AudioEditor. On
    // hard failure clear the CVar so the checkbox stays honest.
    if (CVarGetInteger(CVAR_AUDIO("ModernAudioPipeline"), 0)) {
        if (!ApplyFluidSynthFromCVars()) {
            CVarSetInteger(CVAR_AUDIO("ModernAudioPipeline"), 0);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }

    // Override layering — applied in order so later sources overlay earlier:
    //   1. ResetAllOverrides — wipe to factory state (Auto / 1.0× / -1 / 0)
    //   2. Built-in defaults string (currently empty; reserved for future
    //      curated set; see DefaultFluidSynthOverrides.h for the policy)
    //   3. Each enabled pack's mapping.json (in discovery order) — overlays defaults
    //   4. User's fluidsynth_overrides.json — wins over both (missing
    //      file is the typical first-run state)
    ReapplyOverrideChain();
#endif
}

static RegisterMenuInitFunc menuInitFunc(RegisterAudioWidgets);
