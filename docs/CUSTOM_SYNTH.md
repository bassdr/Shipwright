# Custom SoundFonts using FluidSynth

SoH can play its music through [FluidSynth](https://www.fluidsynth.org/)
using one or more SoundFont (`.sf2`) files supplied as **synth packs**.
Each engine instrument can be routed to a specific SF2 preset, tuned
per pair (gain, transpose, reverb / chorus / filter), and labeled with
a friendly name. This doc is the user + modder guide.

---

## For users

### Modern Audio Pipeline (F32 instead of S16)

In **Audio Editor → Audio Options**, tick **Modern audio pipeline
(floating point)**. This switches the audio path to 32-bit float.

The float pipeline is its own audio quality win even without FluidSynth or any SF2 loaded — It performs audio processing at higher precision, reducing rounding artifacts and preserving more detail during mixing and resampling.

### Enabling the FluidSynth pipeline

To enable the FluidSynth pipeline, you need FluidSynth to be enabled
at compile time. At the time of writing, the only way to get this is
by compiling Shipwright from sources. First read [BUILDING](BUILDING.md) to get the basics. You'll also need to install [FluidSynth](https://www.fluidsynth.org/download/) and configure CMake so it uses it by adding `-G ENABLE_FLUIDSYNTH=ON` to the configuration command, or editing the configuration after the first generation.

In **Audio Editor**, you will se a new dedicated **FluidSynth** tab. This is where the fun starts.

### Adding SF2 files

Two ways:

**Loose folder.** Drop a `*.sf2` into:

```paths
<SoH-config>/synth-packs/
```

Optionally include a sibling `<basename>.json` for per-instrument tuning.
Create the `synth-packs` folder in the same folder where your `mods` folder is.

**Mod archive.** Files inside any mounted `.o2r` archive at:

```paths
audio/synth/<pack-name>/soundfont.sf2
audio/synth/<pack-name>/mapping.json   (optional)
```

Both sources show up in the FluidSynth tab's pack list — loose packs
are listed after mod-supplied ones, both alphabetised. **Rescan**
refreshes the list without restarting. Untick a pack to disable it.

### Stacking multiple packs

When more than one pack is enabled, FluidSynth searches the last-loaded
pack first for any `(bank, program)` lookup — like the wider mod stack,
**last loaded wins** on collisions. Per-pair pinning (see "Pin
behavior" below) lets you bypass this default when you want a specific
pack to win regardless of order.

### Synth mode

In the FluidSynth tab:

- **Authentic** — replaces the SF2's default velocity / CC7 / CC11 →
  attenuation modulators with halved-amount versions ("Graham-Smith"
  curve, per [ANMP](https://github.com/derselbst/ANMP)). Lifts quiet
  voices ~6 dB. Pairs with a console-era reverb preset. Default.
- **Enhanced** — stock SF2 modulators. Pairs with a subtle reverb.
  Use this with musically-curated banks (orchestral / SC-55 /
  MuseScore) where the SF2 author's dynamics are what you want.

### The per-instrument table

Each row is one engine instrument pair `(Song family, Sample id)` the
audio engine has actually played at least once. Columns:

| Column     | What it does                                                                       |
|------------|------------------------------------------------------------------------------------|
| Override   | Session-only controls (Solo, Mute, temp volume). Orange-tinted — *not saved*.     |
| Song       | Engine font name (e.g. "Fairy Fountain"). Read-only.                              |
| Sample     | Editable label — type a name like "Hyrule Field strings"; hint shows the auto-detected SF2 sample name. |
| Mode       | Native (engine synth) / Synth (FluidSynth).                                        |
| Gain       | Per-instrument synth volume (CC11 multiplier).                                     |
| Shift      | Pitch shift in octaves or semitones (toggle in column header).                     |
| Source     | Read-only pin label — `[Pack] B#`. Green = live, orange = drifted, red = dead.    |
| Preset     | Pick an SF2 preset from any loaded pack. Selecting also pins to that pack.        |
| Reverb / Chorus / Cutoff / Q | Per-instrument MIDI CC91 / CC93 / CC74 / CC71.                       |

**Override column (session-only)**:

- **Solo** — adds the row to the solo set. While the set is non-empty,
  every non-solo row is muted. Click on multiple rows to play them all
  side-by-side.
- **Mute / temp volume** — Native rows get a Mute toggle (engine has no
  per-pair gain hook); Synth rows get a `0..4x` volume slider (drag to
  `0` to mute). Both are session-only.
- **Clear** (in the column header) — wipes solo, mute, temp volume
  across every row. Does *not* touch persisted overrides.

Everything **outside** the Override column auto-saves to
`<SoH-config>/fluidsynth_overrides.json` on every edit. There's no
Save button — your edits stick. To roll back persisted edits, use
**Reset all** above the table.

### Pin behavior

When you pick a preset from the Preset combo, the row is *pinned* to
the SF2 that owns that preset. The pin persists through pack toggles:

- **Live** (Source column = green `[Pack] B#`): the pinned pack is
  loaded and the preset is found. The row plays through that pack.
- **Drift** (orange): the pinned pack is loaded, but a *different*
  pack now resolves the same `(bank, program)` first because the load
  order changed. Re-pick the preset to refresh.
- **Dead** (red `[missing]`): the pinned pack is disabled or the
  preset is gone. The row plays native until the pack is re-enabled.
  Your tuning sits on disk waiting.

Tooltip on the Source column explains each state in context.

### Known limitations

A generic SoundFont can't fully match the original samples:

- The engine uses **per-key sample splits** with their own ADSR / pan /
  attenuation per range, plus baked filter and reverb shaping. A GM
  program plays one sample across the whole range with one envelope.
- **Slot semantics are font-specific.** The same `instOrWave` index
  means different things in different engine fonts. Tuning is per-font
  work — there's no shared "bank 1 layout."
- A SoundFont *purpose-built* for the game's instrument layout is the
  realistic path to "better than native." Synth packs are how those
  community projects ship.

---

## For modders

### Pack layout

Loose form (for iterating during authoring — no zip step):

```paths
<SoH-config>/synth-packs/MyPack.sf2
<SoH-config>/synth-packs/MyPack.json   (optional)
```

Archive form (for distribution):

```paths
<MyPack.o2r>/
  audio/synth/MyPack/soundfont.sf2
  audio/synth/MyPack/mapping.json   (optional)
```

The `<pack-name>` segment is the user-visible identifier — short
ASCII slug like `OoT-HD-Orchestra`, `FluidR3-Default`.

### Authoring loop

1. Drop your SF2 into the loose folder. The pack appears in the
   FluidSynth tab (hit **Rescan** if needed).
2. Enable just your pack, disable the rest. Open the game and play
   the songs you're tuning against.
3. Walk the per-instrument table. For each row:
   - Pick a preset from the **Preset** combo. The row is now pinned to
     your SF2.
   - Tune Gain, Shift, effect CCs by ear.
   - Type a friendly label in **Sample** so the table reads
     coherently when you come back to it.
   - Use **Solo** to hear just one voice. Use the temp-volume slider
     for "is it 5% too loud or 15%?" A/B work.
4. Every edit auto-saves to `fluidsynth_overrides.json`. When you're
   happy, copy that file into your pack as `mapping.json` and ship
   the two files together.

### `mapping.json` schema

Same shape as the user's `fluidsynth_overrides.json` — your pack's
mapping is just a chain layer that overlays defaults, with the user's
file overlaying yours.

```json
{
  "version": 1,
  "entries": [
    {
      "fontId": 6,
      "instOrWave": 12,
      "pack": "MyPack",
      "bank": 0,
      "program": 46,
      "preset_name": "Orchestral Harp",
      "display_name": "Hyrule Field strings",
      "bypass": "synth",
      "gain": 0.85,
      "transpose": 0,
      "reverb": 64,
      "chorus": 16,
      "filter_cutoff": 96,
      "filter_q": 32
    }
  ]
}
```

| Key             | Type              | Meaning                                                             |
|-----------------|-------------------|---------------------------------------------------------------------|
| `fontId`        | int 0–63          | Engine sound-font index. Required. 0–37 are vanilla.                |
| `instOrWave`    | int 0–255         | Engine instrument slot. Required.                                   |
| `pack`          | string            | Pack name this entry pins to. Required for the row to actually pin. |
| `bank`          | int 0–255         | SF2 bank. Default 0 (GM melodic); 128 = GM drums.                   |
| `program`       | int 0–127         | SF2 program inside that bank.                                       |
| `preset_name`   | string            | Human-readable SF2 preset name. Metadata only — used by the UI to detect drift. |
| `display_name`  | string            | Friendly label shown in the Sample column.                          |
| `bypass`        | "synth" / "native"| Force route. Omit for Auto (mapping table default).                 |
| `gain`          | float             | Per-pair CC11 multiplier. Omit = 1.0x.                              |
| `transpose`     | int (semitones)   | Pitch shift. Omit = 0.                                              |
| `reverb`        | int 0–127         | CC91 reverb send. Omit to leave the synth default.                  |
| `chorus`        | int 0–127         | CC93 chorus send.                                                   |
| `filter_cutoff` | int 0–127         | CC74 low-pass cutoff (64 = no shift from SF2 default).              |
| `filter_q`      | int 0–127         | CC71 low-pass resonance (64 = no shift).                            |

Omit any field you don't want to set — partial entries layer cleanly
over the chain below them.

### Pack-bound resolution model

Each entry resolves in one atomic step:

1. Look up `(pack, bank, program)`.
2. If the pack isn't loaded → entry is **dead**, row plays native.
3. If the pack is loaded but doesn't have that `(bank, program)` →
   entry is **dead**, row plays native.
4. Only when both succeed do the rest of the fields apply.

Pinning uses FluidSynth's `program_select` under the hood — so even
if another loaded SF2 has the same `(bank, program)`, your pack wins
for the rows you pinned. Multi-pack stacks compose cleanly.

A dead entry stays on disk. Re-enable the source pack and your tuning
springs back to life with the original audible result.

### Override chain (highest priority wins)

1. Reset to factory state
2. Built-in defaults (currently empty)
3. Pack mapping.json files, in enabled-pack order
4. User's fluidsynth_overrides.json

So a user's per-pair edit always wins over your pack's defaults, and
a later-loaded pack's mapping wins over an earlier one's. This means
users can fork-without-forking: re-tune individual pairs to taste
without touching your pack.

### Packaging

Standard `zip` produces a valid `.o2r`:

```bash
cd my-pack/
zip -r ../MyPack.o2r audio/
```

Users drop the `.o2r` into `<SoH-config>/mods/`.

### Caveats

- **New songs with custom engine fonts are not reliably supported.**
  Custom fonts (under `custom/fonts/`) are assigned a `fontId` by
  archive listing order, which is unordered-map iteration (effectively
  arbitrary). Two mods with custom fonts can shift each other's `fontId` unpredictably, making any FluidSynth mapping against those IDs fragile. A new song that only references vanilla fonts (0–37) is not affected.
- **`MidiTranslator::kMaxFontId`** caps total fonts at 64 (38 vanilla + 26 modded slots).
- **Sample names**: many engine slots have no captured sample name in
  the binary asset. The Sample column shows `(no sample name)` as a
  hint when this happens — that's where `display_name` becomes
  especially useful for walking the table; They can be included in mods.

#### Engine quirks the substitution model can't paper over

These are limits of the SF2-substitution approach itself, not bugs.
A pack author hits them as "this row refuses to behave"; rather than
fight, mark the row Native and move on.

- **Engine drum bank (`Inst 0`) and SFX bank (`Inst 1`) route to
  native unconditionally.** For these channels, the `semitone` byte
  the engine emits is a slot index into a per-font drum / SFX table,
  not a chromatic pitch. FluidSynth has no way to interpret a slot
  index meaningfully, so the translator falls back to the engine
  renderer. Any preset you pick on these rows is preserved in the
  JSON but does not play; the row in the bypass table is labelled
  `Drum` / `SFX` in the Inst column with a tooltip explaining why.
  Worth noting: most OoT songs barely touch the engine drum bank.
  See the next caveat for where audible percussion actually lives.

- **Audible drums usually live on melodic-instrument rows, not
  Inst 0.** OoT's music engine commonly authors percussion as a
  regular `Instrument` whose L / M / H sample slots hold drum hits,
  with the sequence picking specific engine semitones to select each
  drum. Example: the Goron Drum heard in the potion-shop loop lives
  on `(font 28, inst 3)`, not Inst 0. Two consequences:
  - The engine pitch on the row has no relationship to the sample's
    *actual* fundamental — what's labelled "semitone 39" might
    sound like a snare somewhere around 200 Hz, not D#1.
  - A melodic SF2 substitution on the row produces "right rhythm,
    wrong sound at random pitches" because GM percussion expects a
    fixed `pitch → drum` map (MIDI 36 = kick, 38 = snare, ...) the
    engine never agreed to.
  Today the only clean answer on these rows is Native.

- **Single-NoteOn polyphonic samples cannot be substituted.** A few
  OoT instruments bake a chord into a single sample — one engine
  NoteOn fires what sounds like a 10-voice bell stack. The
  substitution model fires *one* preset voice per engine event, so
  reproducing this would need either a custom SF2 preset whose
  single voice already contains the chord stack, or a sequence
  rewrite that emits the chord as N parallel NoteOns. Neither is in
  this tool's scope — leave the row Native.

---

## See also

- [derselbst/**ANMP**](https://github.com/derselbst/ANMP) —
  the closest analog: a FluidSynth-based player for sequenced
  console-era music. Originator of the Graham-Smith volume curve.
- [**ANMP**'s wiki](https://github.com/derselbst/ANMP/wiki), in particular [Reproducing N64 OSTs accurately](https://github.com/derselbst/ANMP/wiki/Reproducing-N64-OSTs-accurately) — reverb preset
  research that informs the Authentic mode default.
