#!/usr/bin/env -S uv run --quiet --with piper-tts --script
"""Turn a tts_dump manifest into the voice clips the game looks up.

Run `tts_dump` in the game console first; it writes one manifest per language
under the app directory. The clip file name is the hash the game computed, so
this script never has to reproduce the game's text decoding:

    scripts/voice-bake.py ~/.local/share/soh/voice/en-US/manifest.tsv

Each line carries the voice it was cast to, and clips are written under that
voice's folder, so re-casting one character costs only that character's lines.
Voices are defined in scripts/voice-profiles.json.

Baking is incremental. Clips that already exist are left alone, so a changed
voice needs --force and a widened corpus costs only the new lines.
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

PROFILES_FILE = Path(__file__).with_name("voice-profiles.json")

# Dialogue is speech, not music: a low bitrate is inaudible here and keeps a full
# corpus small enough to ship.
DEFAULT_BITRATE = "32k"

# Trim the lead-in and tail the model leaves, then even out the level, which
# drifts noticeably between short and long lines.
FILTERS = (
    "silenceremove=start_periods=1:start_silence=0.05:start_threshold=-50dB,"
    "areverse,"
    "silenceremove=start_periods=1:start_silence=0.05:start_threshold=-50dB,"
    "areverse,"
    "loudnorm=I=-18:TP=-2:LRA=11"
)

_voice = None
_bitrate = None
_profile = None


def read_manifest(path):
    """{profile: {hash: text}}, first occurrence winning for a repeated line."""
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 5:
            continue
        digest, profile, text = fields[0], fields[1], fields[4]
        rows.setdefault(profile, {}).setdefault(digest, text)
    return rows


def effect_chain(rate, profile):
    """ffmpeg filters that turn the raw voice into this character's.

    A person faking a deeper voice drops pitch far more than their vocal tract
    can shorten, so pitch and formants have to move by different amounts:
    asetrate moves both, rubberband moves pitch alone, and composing the two
    reaches any pair of targets.
    """
    pitch = float(profile.get("pitch", 0.0))
    formant = float(profile.get("formant", 0.0))
    if pitch == 0.0 and formant == 0.0:
        return []
    scale = 2.0 ** (formant / 12.0)
    return [f"asetrate={int(rate * scale)}", f"aresample={rate}", f"atempo={1 / scale:.6f}",
            f"rubberband=pitch={2 ** ((pitch - formant) / 12.0):.6f}:formant=preserved:pitchq=quality"]


def speakable(text):
    """The manifest keeps the game's own spacing; a synthesiser wants prose."""
    return " ".join(text.replace("\\n", "\n").replace("\\\\", "\\").split())


def load_voice(profile, data_dir, bitrate):
    from piper import PiperVoice
    from piper.download_voices import download_voice

    data_dir.mkdir(parents=True, exist_ok=True)
    model = data_dir / f"{profile['model']}.onnx"
    if not model.exists():
        download_voice(profile["model"], data_dir)
    global _voice, _bitrate, _profile
    _voice = PiperVoice.load(model, download_dir=data_dir)
    _bitrate = bitrate
    _profile = profile


def encode(pcm, rate, destination, bitrate, profile):
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", "-",
         "-af", ",".join(effect_chain(rate, profile) + [FILTERS]),
         "-c:a", "libopus", "-b:a", bitrate,
         "-application", "voip", str(destination)],
        input=pcm, check=True,
    )


def bake_one(job):
    from piper import SynthesisConfig

    digest, text, destination = job
    config = SynthesisConfig(speaker_id=int(_profile.get("speaker", 0)),
                             length_scale=float(_profile.get("length_scale", 1.0)))
    if "noise_scale" in _profile:
        config.noise_scale = float(_profile["noise_scale"])
    chunks = list(_voice.synthesize(speakable(text), syn_config=config))
    if not chunks:
        return digest, "empty"
    pcm = b"".join(chunk.audio_int16_bytes for chunk in chunks)
    encode(pcm, chunks[0].sample_rate, Path(destination), _bitrate, _profile)
    return digest, "baked"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="manifest.tsv written by the tts_dump console command")
    parser.add_argument("--out", type=Path, help="where the voice folders go (default: beside the manifest)")
    parser.add_argument("--profiles", type=Path, default=PROFILES_FILE)
    parser.add_argument("--only", help="bake just this voice")
    parser.add_argument("--data-dir", type=Path, default=Path.home() / ".cache" / "soh-voice-models")
    parser.add_argument("--bitrate", default=DEFAULT_BITRATE)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--limit", type=int, help="bake only the first N lines per voice, to audition it")
    parser.add_argument("--force", action="store_true", help="re-bake clips that already exist")
    args = parser.parse_args()

    if not args.manifest.is_file():
        sys.exit(f"no manifest at {args.manifest}")

    language = args.manifest.parent.name
    profiles = json.loads(args.profiles.read_text(encoding="utf-8"))
    out = args.out or args.manifest.parent
    cast = read_manifest(args.manifest)
    if not cast:
        sys.exit(f"{args.manifest} has no rows; re-run tts_dump with a build that writes the voice column")

    for name in sorted(cast):
        if args.only and name != args.only:
            continue
        rows = cast[name]
        definition = profiles.get(name, {}).get(language)
        if definition is None:
            # No stand-in voice: an uncast character stays silent in game rather
            # than speaking in someone else's voice.
            print(f"{language}/{name}: {len(rows)} lines, no {language} voice defined - skipped", flush=True)
            continue

        destination_dir = out / name
        destination_dir.mkdir(parents=True, exist_ok=True)
        jobs = []
        for digest, text in rows.items():
            destination = destination_dir / f"{digest}.opus"
            if destination.exists() and not args.force:
                continue
            if speakable(text):
                jobs.append((digest, text, str(destination)))
        if args.limit:
            jobs = jobs[: args.limit]

        print(f"{language}/{name}: {len(rows)} lines, {len(jobs)} to bake "
              f"with {definition['model']} speaker {definition.get('speaker', 0)}", flush=True)
        if not jobs:
            continue

        done = 0
        with ProcessPoolExecutor(args.jobs, initializer=load_voice,
                                 initargs=(definition, args.data_dir, args.bitrate)) as pool:
            for _digest, status in pool.map(bake_one, jobs, chunksize=8):
                done += status == "baked"
                if done % 100 == 0:
                    print(f"  {done}/{len(jobs)}", flush=True)

        clips = list(destination_dir.glob("*.opus"))
        size = sum(f.stat().st_size for f in clips)
        print(f"{language}/{name}: {len(clips)} clips, {size / 1e6:.1f} MB in {destination_dir}")


if __name__ == "__main__":
    main()
