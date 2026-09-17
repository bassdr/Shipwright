#!/usr/bin/env -S uv run --quiet --with piper-tts --script
"""Turn a tts_dump manifest into the voice clips the game looks up.

Run `tts_dump` in the game console first; it writes one manifest per language
under the app directory. The clip file name is the hash the game computed, so
this script never has to reproduce the game's text decoding:

    scripts/voice-bake.py ~/.local/share/soh/voice/en-US/manifest.tsv

Baking is incremental. Clips that already exist are left alone, so a changed
voice needs --force and a widened corpus costs only the new lines.
"""

import argparse
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

DEFAULT_VOICES = {
    "en-US": "en_US-hfc_female-medium",
    "fr-FR": "fr_FR-siwis-medium",
    "de-DE": "de_DE-thorsten-medium",
}

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


def read_manifest(path):
    """Rows of (hash, text), first occurrence wins for a repeated line."""
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 4:
            continue
        digest, text = fields[0], fields[3]
        if digest not in rows:
            rows[digest] = text
    return rows


def speakable(text):
    """The manifest keeps the game's own spacing; a synthesiser wants prose."""
    return " ".join(text.replace("\\n", "\n").replace("\\\\", "\\").split())


def load_voice(name, data_dir, bitrate):
    from piper import PiperVoice
    from piper.download_voices import download_voice

    data_dir.mkdir(parents=True, exist_ok=True)
    model = data_dir / f"{name}.onnx"
    if not model.exists():
        download_voice(name, data_dir)
    global _voice, _bitrate
    _voice = PiperVoice.load(model, download_dir=data_dir)
    _bitrate = bitrate


def encode(pcm, rate, destination, bitrate):
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", "-",
         "-af", FILTERS, "-c:a", "libopus", "-b:a", bitrate,
         "-application", "voip", str(destination)],
        input=pcm, check=True,
    )


def bake_one(job):
    digest, text, destination = job
    chunks = list(_voice.synthesize(speakable(text)))
    if not chunks:
        return digest, "empty"
    pcm = b"".join(chunk.audio_int16_bytes for chunk in chunks)
    encode(pcm, chunks[0].sample_rate, Path(destination), _bitrate)
    return digest, "baked"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="manifest.tsv written by the tts_dump console command")
    parser.add_argument("--out", type=Path, help="where the clips go (default: beside the manifest)")
    parser.add_argument("--voice", help="Piper voice name (default: picked from the manifest's language folder)")
    parser.add_argument("--data-dir", type=Path, default=Path.home() / ".cache" / "soh-voice-models")
    parser.add_argument("--bitrate", default=DEFAULT_BITRATE)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--limit", type=int, help="bake only the first N lines, to audition a voice")
    parser.add_argument("--force", action="store_true", help="re-bake clips that already exist")
    args = parser.parse_args()

    if not args.manifest.is_file():
        sys.exit(f"no manifest at {args.manifest}")

    language = args.manifest.parent.name
    voice = args.voice or DEFAULT_VOICES.get(language)
    if voice is None:
        sys.exit(f"no default voice for {language}; pass --voice")

    out = args.out or args.manifest.parent
    out.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(args.manifest)
    jobs = []
    for digest, text in rows.items():
        destination = out / f"{digest}.opus"
        if destination.exists() and not args.force:
            continue
        if speakable(text):
            jobs.append((digest, text, str(destination)))
    if args.limit:
        jobs = jobs[: args.limit]

    print(f"{language}: {len(rows)} lines, {len(jobs)} to bake with {voice}", flush=True)
    if not jobs:
        return

    done = 0
    with ProcessPoolExecutor(args.jobs, initializer=load_voice,
                             initargs=(voice, args.data_dir, args.bitrate)) as pool:
        for _digest, status in pool.map(bake_one, jobs, chunksize=8):
            done += status == "baked"
            if done % 100 == 0:
                print(f"  {done}/{len(jobs)}", flush=True)

    total = sum(1 for _ in out.glob("*.opus"))
    size = sum(f.stat().st_size for f in out.glob("*.opus"))
    print(f"{language}: {total} clips, {size / 1e6:.1f} MB in {out}")


if __name__ == "__main__":
    main()
