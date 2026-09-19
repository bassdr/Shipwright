#!/usr/bin/env python3
"""Audition a line without launching the game.

Mirrors what VoiceSynth::Render does: the say rules, espeak, the phoneme rules,
one segment per mark that asks for a pause, and the per-mark emphasis, speed and
silence. Same models, same voice-profiles.json, same voice-speech.json.

    uv run --with onnxruntime --with numpy scripts/voice-preview.py \
        --voice guard --text "Wha ha ha ha!" --play

Divergence to know about: the game lets espeak decide where a clause ends, this
splits on the marks itself. Same result for ordinary lines; a line that leans on
espeak's own sentence logic can differ slightly.
"""
import argparse, json, re, struct, subprocess, sys, wave
from pathlib import Path

import numpy as np
import onnxruntime as ort

GAME = Path("/home/david/Jeux/PC/SoH")
MARKS = ".,!?;:"


def load(name, default=None):
    path = GAME / name
    if not path.exists():
        return default if default is not None else {}
    return json.loads(path.read_text())


def espeak(text, voice):
    out = subprocess.run(["espeak-ng", "-v", voice, "-q", "--ipa", text],
                         capture_output=True, text=True).stdout
    # espeak announces a language switch inside the phonemes; the model reads it out
    return re.sub(r"\([a-z-]+\)", "", " ".join(l.strip() for l in out.split("\n") if l.strip()))


def rules(section):
    return sorted(section.items(), key=lambda kv: -len(kv[0]))


def apply_rules(text, pairs):
    for frm, to in pairs:
        text = text.replace(frm, to)
    return text


def shift(a, semitones):
    if semitones == 0:
        return a
    ratio = 2.0 ** (semitones / 12.0)
    return np.interp(np.arange(0, len(a), ratio), np.arange(len(a)), a).astype(np.float32)


def segment(text, speech, voice):
    """[(phonemes, mark)] - one entry per stretch the model renders in one go."""
    marks = speech.get("marks", {})
    out, buf = [], ""
    for piece in re.split(r"([%s]+)" % re.escape(MARKS), text):
        if not piece:
            continue
        if piece[0] in MARKS:
            run = piece
            tuning = None
            for length in range(len(run), 0, -1):
                if run[:length] in marks:
                    tuning = marks[run[:length]]
                    break
            tuning = tuning or {}
            buf += tuning.get("emit", run[0]) + " "
            if tuning.get("pause", 0) or tuning.get("emphasis", 1) != 1 or tuning.get("length_scale", 1) != 1:
                out.append((buf, tuning))
                buf = ""
        else:
            buf += apply_rules(espeak(piece, voice), rules(speech.get("phonemes", {}))) + " "
    if buf.strip():
        out.append((buf, {}))
    return out


def trim(a, threshold=0.003):
    loud = np.abs(a) > threshold
    if not loud.any():
        return a[:0]
    return a[loud.argmax(): len(a) - loud[::-1].argmax()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--lang", default="fr-FR")
    ap.add_argument("--text", required=True)
    ap.add_argument("--out", default="/tmp/voice-preview.wav")
    ap.add_argument("--play", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    profiles = load("voice-profiles.json")
    speech = load("voice-speech.json").get(args.lang, {})
    if args.voice not in profiles or args.lang not in profiles[args.voice]:
        sys.exit(f"no {args.voice}/{args.lang} in voice-profiles.json")
    profile = profiles[args.voice][args.lang]

    model = GAME / "voice-models" / (profile["model"] + ".onnx")
    idmap = json.loads((model.with_suffix(".onnx.json")).read_text())["phoneme_id_map"]
    rate = json.loads((model.with_suffix(".onnx.json")).read_text())["audio"]["sample_rate"]
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(model), options, providers=["CPUExecutionProvider"])
    names = {i.name for i in session.get_inputs()}

    def run(phonemes, ls):
        tokens = [1, 0]
        for ch in phonemes:
            if ch in idmap:
                tokens += [idmap[ch][0], 0]
        tokens.append(2)
        if len(tokens) <= 3:
            return np.zeros(0, dtype=np.float32)
        feed = {"input": np.array([tokens], dtype=np.int64),
                "input_lengths": np.array([len(tokens)], dtype=np.int64),
                "scales": np.array([profile.get("noise_scale", 0.667),
                                    profile.get("length_scale", 1.0) * ls,
                                    profile.get("noise_w", 0.8)], dtype=np.float32),
                "sid": np.array([profile.get("speaker", 0)], dtype=np.int64)}
        return trim(session.run(None, {k: v for k, v in feed.items() if k in names})[0].squeeze())

    spoken = apply_rules(args.text, rules(speech.get("say", {})))
    for silent in sorted(speech.get("unspoken", []), key=len, reverse=True):
        spoken = re.sub(re.escape(silent) + r"[ !?.,;:]*", "", spoken)
    audio = []
    for phonemes, mark in segment(spoken, speech, args.lang.split("-")[0]):
        part = run(phonemes, mark.get("length_scale", 1.0))
        if len(part) == 0:
            continue
        part = trim(shift(part, mark.get("pitch", 0.0)))
        audio.append(part * mark.get("emphasis", 1.0))
        audio.append(np.zeros(int(mark.get("pause", 0.0) * rate), dtype=np.float32))
        if not args.quiet:
            print(f"  segment {len(audio)//2:2d}  {len(part)/rate:5.2f}s  "
                  f"emph {mark.get('emphasis', 1.0):.2f}  ls {mark.get('length_scale', 1.0):.2f}  "
                  f"pause {mark.get('pause', 0.0):.2f}  {phonemes.strip()}")

    out = np.concatenate(audio) if audio else np.zeros(1, dtype=np.float32)
    peak = np.max(np.abs(out)) or 1.0
    out = (out / peak * 0.9 * 32767).astype(np.int16)
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(out.tobytes())
    if not args.quiet:
        print(f"{len(out)/rate:.2f}s -> {args.out}")
    if args.play:
        subprocess.run(["paplay", args.out])


if __name__ == "__main__":
    main()
