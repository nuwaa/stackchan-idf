#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""Train the vowel MLP on features produced by the reference pipeline.

  python3 train.py <dataset-dir> --out model.json [--header vowel_model.hpp]

Features come from proto.Detector (so they are exactly what the firmware
computes). Training frames: voiced frames inside a vowel label with ≥ 20 ms
margin to the label edges. Speaker-scale augmentation resamples each
utterance by ±8 / ±15 % (shifts formants like a longer / shorter vocal tract).
Accuracy is reported with leave-one-speaker-out cross-validation before the
final model is fitted on every speaker.

Real microphone recordings can be mixed in with --real <wav> <labels.txt>
(repeatable). The WAV is 16 kHz mono as captured by the device mic
(tools/mic_tap.py); labels.txt lists, per line, a vowel and the start times (s)
of the voiced segments where it is held: `a 4.93 13.09 ...`. Each segment
runs to the end of the voiced run that starts there. The frames are weighted
--real-weight times (default 20) so a few minutes of the real mic / room /
voice pull the model towards the deployment conditions without drowning the
synthetic speakers. Not used in the LOSO evaluation, only in the final fit.
"""
import argparse, json, pathlib
import numpy as np
import proto

HIDDEN = 16
AUG_RATES = (0.85, 0.92, 1.0, 1.08, 1.15)


def resample(x, rate):
    """Linear-interpolation resample by `rate` (>1 = shorter = higher formants)."""
    n = int(len(x) / rate)
    idx = np.arange(n) * rate
    i0 = np.floor(idx).astype(int)
    fr = (idx - i0).astype(np.float32)
    i1 = np.minimum(i0 + 1, len(x) - 1)
    return (x[i0] * (1 - fr) + x[i1] * fr).astype(np.float32)


def collect(ds, idx, rates):
    X, Y, S = [], [], []
    for e in idx:
        x0 = proto.read_wav(ds / f"{e['id']}.wav")
        labs0 = proto.read_lab(ds / f"{e['id']}.lab")
        for rate in rates:
            x = resample(x0, rate) if rate != 1.0 else x0
            labs = [(a / rate, b / rate, l) for a, b, l in labs0]
            det = proto.Detector()
            for i in range(len(x) // proto.HOP):
                r = det.push(x[i * proto.HOP:(i + 1) * proto.HOP])
                t = ((i + 1) * proto.HOP - proto.WIN) / proto.FS
                lab, (into, left) = proto.label_at(labs, t)
                if lab not in proto.VOWELS or into < 0.02 or left < 0.02 or not r["voiced"]:
                    continue
                X.append(r["feat"]); Y.append(proto.VOWELS.index(lab)); S.append(e["speaker"])
    return np.array(X), np.array(Y), np.array(S)


def collect_real(wav, labels, weight):
    """Frames from a device-mic recording, labelled by segment start times."""
    x = proto.read_wav(wav)
    det = proto.Detector()
    feats, voiced = [], []
    for i in range(len(x) // proto.HOP):
        r = det.push(x[i * proto.HOP:(i + 1) * proto.HOP])
        feats.append(r["feat"]); voiced.append(r["voiced"])
    feats = np.array(feats); voiced = np.array(voiced)
    lab = np.full(len(feats), -1)
    for line in pathlib.Path(labels).read_text().split("\n"):
        p = line.split()
        if not p or p[0] not in proto.VOWELS:
            continue
        v = proto.VOWELS.index(p[0])
        for st in map(float, p[1:]):
            i = max(0, int(st * proto.FS / proto.HOP) - 1)
            while i < len(feats) and not voiced[i]:
                i += 1
            gap = 0
            while i < len(feats) and gap <= 6:  # allow ~100 ms unvoiced dropouts
                if voiced[i]:
                    lab[i] = v; gap = 0
                else:
                    gap += 1
                i += 1
    m = lab >= 0
    print(f"real {wav}: {m.sum()} labelled frames {np.bincount(lab[m], minlength=5).tolist()} x{weight}")
    return np.repeat(feats[m], weight, axis=0), np.repeat(lab[m], weight)


def train(X, Y, epochs=1500, lr=0.05, seed=0, l2=1e-4):
    rng = np.random.default_rng(seed)
    mu, sd = X.mean(0), X.std(0) + 1e-6
    Xn = (X - mu) / sd
    W1 = rng.normal(0, 0.3, (X.shape[1], HIDDEN)); b1 = np.zeros(HIDDEN)
    W2 = rng.normal(0, 0.3, (HIDDEN, 5)); b2 = np.zeros(5)
    T = np.eye(5)[Y]
    # class balancing: weight frames inversely to class frequency
    cw = (len(Y) / (5 * np.bincount(Y, minlength=5)))[Y][:, None]
    for _ in range(epochs):
        H = np.maximum(Xn @ W1 + b1, 0)
        z = H @ W2 + b2
        z -= z.max(1, keepdims=True)
        p = np.exp(z); p /= p.sum(1, keepdims=True)
        g = (p - T) * cw / len(Xn)
        gW2 = H.T @ g; gb2 = g.sum(0)
        gH = g @ W2.T; gH[H <= 0] = 0
        gW1 = Xn.T @ gH; gb1 = gH.sum(0)
        W2 -= lr * (gW2 + l2 * W2); b2 -= lr * gb2
        W1 -= lr * (gW1 + l2 * W1); b1 -= lr * gb1
    return {"mu": mu, "sd": sd, "W1": W1, "b1": b1, "W2": W2, "b2": b2}


def predict(m, X):
    H = np.maximum((X - m["mu"]) / m["sd"] @ m["W1"] + m["b1"], 0)
    return (H @ m["W2"] + m["b2"]).argmax(1)


def write_header(m, path):
    def arr(name, a, dims):
        flat = np.asarray(a, dtype=np.float32).ravel()
        body = ",\n    ".join(", ".join(f"{v:.7g}f" for v in flat[i:i + 8]) for i in range(0, len(flat), 8))
        return f"inline constexpr std::array<float, {len(flat)}> {name}{{{{\n    {body}}}}};  // {dims}\n"
    txt = ["// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>",
           "// SPDX-License-Identifier: BSL-1.0",
           "// GENERATED by tools/vowel-lipsync/train.py — do not edit by hand.",
           "#pragma once", "#include <array>", "#include <cstddef>", "",
           "namespace stackchan::lipsync::model {", "",
           f"inline constexpr std::size_t kInputs = {len(m['mu'])};",
           f"inline constexpr std::size_t kHidden = {HIDDEN};",
           "inline constexpr std::size_t kOutputs = 5;", "",
           arr("kMu", m["mu"], "[kInputs]"), arr("kSd", m["sd"], "[kInputs]"),
           arr("kW1", m["W1"], "[kInputs][kHidden] row-major"), arr("kB1", m["b1"], "[kHidden]"),
           arr("kW2", m["W2"], "[kHidden][kOutputs] row-major"), arr("kB2", m["b2"], "[kOutputs]"),
           "} // namespace stackchan::lipsync::model", ""]
    pathlib.Path(path).write_text("\n".join(txt))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset", type=pathlib.Path)
    ap.add_argument("--out", required=True)
    ap.add_argument("--header")
    ap.add_argument("--no-aug", action="store_true")
    ap.add_argument("--from-json", help="skip training; write --header from this model.json")
    ap.add_argument("--real", nargs=2, action="append", metavar=("WAV", "LABELS"), default=[],
                    help="device-mic recording + segment labels to mix into the final fit")
    ap.add_argument("--real-weight", type=int, default=20)
    args = ap.parse_args()
    if args.from_json:
        m = {k: np.array(v) for k, v in json.loads(pathlib.Path(args.from_json).read_text()).items()}
        write_header(m, args.header)
        print("wrote", args.header)
        return
    idx = json.loads((args.dataset / "index.json").read_text())
    rates = (1.0,) if args.no_aug else AUG_RATES
    X, Y, S = collect(args.dataset, idx, rates)
    Xc, Yc, Sc = collect(args.dataset, idx, (1.0,)) if not args.no_aug else (X, Y, S)
    print(f"frames: train={len(X)} (aug x{len(rates)}) eval={len(Xc)}")
    grp = np.array([0, 1, 2, 1, 2])
    hit = hit3 = tot = 0
    for sp in sorted(set(S)):
        m = train(X[S != sp], Y[S != sp])
        te = Sc == sp
        p = predict(m, Xc[te])
        acc = (p == Yc[te]).mean()
        hit += (p == Yc[te]).sum(); hit3 += (grp[p] == grp[Yc[te]]).sum(); tot += te.sum()
        print(f"  leave-out {sp:>10}: {acc:.3f}")
    print(f"LOSO frame accuracy: 5-class={hit / tot:.3f}  mouth-shape={hit3 / tot:.3f}")
    for wav, labels in args.real:
        Xr, Yr = collect_real(wav, labels, args.real_weight)
        X = np.vstack([X, Xr]); Y = np.concatenate([Y, Yr])
    m = train(X, Y)
    pathlib.Path(args.out).write_text(json.dumps({k: np.asarray(v).tolist() for k, v in m.items()}))
    if args.header:
        write_header(m, args.header)
    print("wrote", args.out, args.header or "")


if __name__ == "__main__":
    main()
