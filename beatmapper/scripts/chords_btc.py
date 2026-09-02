#!/usr/bin/env python3
"""Chord recognition via the BTC transformer (Park et al., ISMIR 2019),
large-vocabulary model (170 classes incl. sevenths, sus, dim, aug).

Usage: chords_btc.py AUDIO_FILE
Run with: external/venv-btc/bin/python scripts/chords_btc.py AUDIO

Prints one line per chord to stdout:  start<TAB>end<TAB>label
Labels are beatmapper-style ("C", "Am", "E7", "Dmaj7"); no-chord ("N")
and unknown ("X") spans are omitted, and any /bass suffix is dropped.

Results are cached in ~/.cache/beatmapper/chords/ keyed on the audio file's
path, size and mtime (same scheme as chords_madmom.py), so repeated runs
cost one model pass per track.

The BTC repo lives at ../external/BTC-ISMIR19 relative to this script;
paths are resolved from the script's own location so cwd does not matter.
The inference loop below mirrors the repo's test.py exactly (audio ->
22050 Hz mono -> CQT n_bins=144 bpo=24 hop=2048 -> log-amplitude ->
normalize by the checkpoint's mean/std -> 108-frame chunks -> argmax).
"""
import hashlib
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BTC_DIR = os.path.join(SCRIPT_DIR, "..", "external", "BTC-ISMIR19")
MODEL_FILE = os.path.join(BTC_DIR, "test", "btc_model_large_voca.pt")
CONFIG_FILE = os.path.join(BTC_DIR, "run_config.yaml")


def cache_path(audio):
    st = os.stat(audio)
    key = f"{os.path.abspath(audio)}|{st.st_size}|{int(st.st_mtime)}|btc-large"
    h = hashlib.sha1(key.encode()).hexdigest()[:24]
    d = os.path.expanduser("~/.cache/beatmapper/chords")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, h + ".tsv")


def to_beatmapper_label(label):
    # BTC large-voca labels: "C" (maj), "A:min", "E:7", "D:maj7", "N", "X",
    # optionally with a bass like "D:min/b3" (dropped).
    label = label.split("/")[0]
    if label in ("N", "X"):
        return None
    root, _, qual = label.partition(":")
    qual_map = {
        "": "", "maj": "", "min": "m", "7": "7", "min7": "m7",
        "maj7": "maj7", "sus4": "sus4", "sus2": "sus2", "dim": "dim",
        "aug": "aug", "min6": "m6", "maj6": "6",
    }
    return root + qual_map.get(qual, qual)


def recognize(audio):
    # ffmpeg (used by audioread for mp3/m4a decode) lives in /opt/homebrew/bin
    os.environ["PATH"] = os.environ.get("PATH", "") + os.pathsep + "/opt/homebrew/bin"
    sys.path.insert(0, BTC_DIR)
    import numpy as np
    import torch
    import yaml
    from btc_model import BTC_model
    from utils.hparams import HParams
    from utils.mir_eval_modules import audio_file_to_features, idx2voca_chord

    # HParams.load uses yaml.load without a Loader (breaks on PyYAML>=6);
    # load the yaml ourselves instead of patching the repo.
    with open(CONFIG_FILE) as f:
        config = HParams(**yaml.safe_load(f))
    config.feature['large_voca'] = True
    config.model['num_chords'] = 170
    idx_to_chord = idx2voca_chord()

    device = torch.device("cpu")
    model = BTC_model(config=config.model).to(device)
    # weights_only=False: torch>=2.6 defaults to True, but this 2019
    # checkpoint stores numpy scalars (mean/std) alongside the state dict.
    checkpoint = torch.load(MODEL_FILE, map_location=device, weights_only=False)
    mean = checkpoint['mean']
    std = checkpoint['std']
    model.load_state_dict(checkpoint['model'])

    feature, feature_per_second, _ = audio_file_to_features(audio, config)

    # Inference loop copied from BTC-ISMIR19/test.py.
    feature = feature.T
    feature = (feature - mean) / std
    time_unit = feature_per_second
    n_timestep = config.model['timestep']

    num_pad = n_timestep - (feature.shape[0] % n_timestep)
    feature = np.pad(feature, ((0, num_pad), (0, 0)), mode="constant", constant_values=0)
    num_instance = feature.shape[0] // n_timestep

    start_time = 0.0
    segments = []
    with torch.no_grad():
        model.eval()
        feature = torch.tensor(feature, dtype=torch.float32).unsqueeze(0).to(device)
        for t in range(num_instance):
            self_attn_output, _ = model.self_attn_layers(
                feature[:, n_timestep * t:n_timestep * (t + 1), :])
            prediction, _ = model.output_layer(self_attn_output)
            prediction = prediction.squeeze()
            for i in range(n_timestep):
                if t == 0 and i == 0:
                    prev_chord = prediction[i].item()
                    continue
                if prediction[i].item() != prev_chord:
                    segments.append((start_time, time_unit * (n_timestep * t + i),
                                     idx_to_chord[prev_chord]))
                    start_time = time_unit * (n_timestep * t + i)
                    prev_chord = prediction[i].item()
                if t == num_instance - 1 and i + num_pad == n_timestep:
                    if start_time != time_unit * (n_timestep * t + i):
                        segments.append((start_time, time_unit * (n_timestep * t + i),
                                         idx_to_chord[prev_chord]))
                    break
    return segments


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    audio = sys.argv[1]
    if not os.path.exists(audio):
        sys.stderr.write(f"no such file: {audio}\n")
        return 1
    cp = cache_path(audio)
    if os.path.exists(cp):
        sys.stdout.write(open(cp).read())
        return 0
    lines = []
    for start, end, label in recognize(audio):
        bm = to_beatmapper_label(label)
        if bm is None:
            continue
        lines.append(f"{float(start):.3f}\t{float(end):.3f}\t{bm}\n")
    out = "".join(lines)
    tmp = cp + ".tmp"
    with open(tmp, "w") as f:
        f.write(out)
    os.replace(tmp, cp)
    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
