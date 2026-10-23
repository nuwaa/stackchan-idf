#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""Reference implementation of the formant-based vowel detector.

Written to map 1:1 onto the C++ implementation in components/lipsync_dsp:
fixed-size frames, float32-friendly arithmetic, no dynamic allocation needs.

Pipeline per 16 ms mic chunk (256 samples @ 16 kHz):
  1. half-band FIR + decimate to 8 kHz (F1/F2 live below 3 kHz)
  2. 32 ms analysis window (256 samples @ 8 kHz, 50 % overlap)
  3. voicing: frame energy vs. tracked noise floor + NCC pitch periodicity
  4. pre-emphasis, Hann, autocorrelation, Levinson-Durbin -> LPC(10)
  5. formant structure:
       a) LPC spectral envelope sampled on a 24-point log grid 200..3500 Hz
          (per-frame mean removed = gain independent)
       b) F1 / F2 from the LPC polynomial roots, speaker-normalised by a slow
          running centroid, plus the centroid's offset from the reference
          (tells the classifier how deep / high the speaker is)
  6. tiny MLP (29 -> 16 -> 5) gives vowel probabilities (weights trained by
     train.py on VOICEVOX speech); probabilities are smoothed over time and
     turned into a mouth (open, form) pose.

  python3 proto.py <dataset-dir> [--model model.json] [--dump id]
"""
import argparse, json, pathlib, sys, wave
import numpy as np

FS = 16000           # capture rate (mic task)
HOP = 256            # 16 ms chunk at 16 kHz
AFS = 8000           # analysis rate
AHOP = HOP // 2
WIN = 256            # 32 ms at 8 kHz
LPC_ORDER = 10
PREEMPH = 0.95
N_ENV = 24
ENV_LO, ENV_HI = 200.0, 3500.0
PITCH_LAG_LO, PITCH_LAG_HI = 20, 133      # 400 Hz .. 60 Hz
F_MIN, F_MAX, BW_MAX = 150.0, 3500.0, 500.0
F1_LO, F1_HI, F2_HI, F2_GAP = 200.0, 1100.0, 3200.0, 150.0
ADAPT_FRAMES = 150                        # speaker centroid EMA (~2.4 s voiced)
SNR_VOICED_DB = 10.0
PERIODICITY_VOICED = 0.5
NOISE_FLOOR_MIN_DB = -70.0
NOISE_DOWN_DB_S, NOISE_UP_DB_S = 6.0, 0.5
DECIM_TAPS = 15
VOWELS = ["a", "i", "u", "e", "o"]
# Mouth pose per vowel (open, form) — same table as main/speech.cpp.
MOUTH = {"a": (1.00, 0.30), "i": (0.25, 0.00), "u": (0.30, 1.00), "e": (0.55, 0.15), "o": (0.75, 0.85)}
# Default (untrained) formant prototypes, only used for the speaker centroid.
PROTO_F = {"a": (800.0, 1300.0), "i": (300.0, 2300.0), "u": (350.0, 1300.0), "e": (500.0, 1950.0), "o": (480.0, 850.0)}
CENTROID = (float(np.mean([np.log(p[0]) for p in PROTO_F.values()])),
            float(np.mean([np.log(p[1]) for p in PROTO_F.values()])))
N_FEAT = N_ENV + 5   # env, (lf1-cent, lf2-cent, missing), (cent - reference centroid)

# ---------------------------------------------------------------------------
# tables
# ---------------------------------------------------------------------------
_window = np.hanning(WIN).astype(np.float32)
_env_hz = np.exp(np.linspace(np.log(ENV_LO), np.log(ENV_HI), N_ENV))
_env_w = 2.0 * np.pi * _env_hz / AFS
_cos = np.cos(np.outer(_env_w, np.arange(LPC_ORDER + 1)))
_sin = np.sin(np.outer(_env_w, np.arange(LPC_ORDER + 1)))
_t = np.arange(DECIM_TAPS) - (DECIM_TAPS - 1) / 2
_decim_fir = (0.5 * np.sinc(0.5 * _t) * np.hamming(DECIM_TAPS))
_decim_fir = (_decim_fir / _decim_fir.sum()).astype(np.float32)


def read_wav(p):
    with wave.open(str(p)) as w:
        assert w.getframerate() == FS and w.getnchannels() == 1
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0


def read_lab(p):
    out = []
    for line in p.read_text().splitlines():
        a, b, l = line.split()
        out.append((float(a), float(b), l))
    return out


def label_at(labs, t):
    for a, b, l in labs:
        if a <= t < b:
            return l, (t - a, b - t)
    return "sil", (0, 0)


class Decimator:
    def __init__(self):
        self.hist = np.zeros(DECIM_TAPS - 1, dtype=np.float32)

    def push(self, chunk):
        x = np.concatenate([self.hist, chunk])
        y = np.convolve(x, _decim_fir, mode="valid")
        self.hist = x[-(DECIM_TAPS - 1):]
        return y[::2]


def levinson(r, p):
    a = np.zeros(p + 1)
    a[0] = 1.0
    err = r[0]
    if err <= 0:
        return a, 0.0
    for i in range(1, p + 1):
        acc = r[i] + np.dot(a[1:i], r[i - 1:0:-1])
        k = -acc / err
        a_new = a.copy()
        a_new[1:i] = a[1:i] + k * a[i - 1:0:-1]
        a_new[i] = k
        a = a_new
        err *= (1.0 - k * k)
        if err <= 1e-12:
            break
    return a, err


def analyse_frame(x):
    """x: WIN samples @ 8 kHz. Returns energy, periodicity, env(N_ENV), formants."""
    # voicing (raw frame, NCC over pitch lags)
    best = 0.0
    for k in range(PITCH_LAG_LO, PITCH_LAG_HI + 1):
        a_, b_ = x[:WIN - k], x[k:]
        den = np.sqrt(np.dot(a_, a_) * np.dot(b_, b_)) + 1e-12
        best = max(best, np.dot(a_, b_) / den)
    # LPC on pre-emphasised, windowed frame
    y = np.empty_like(x)
    y[0] = x[0]
    y[1:] = x[1:] - PREEMPH * x[:-1]
    y *= _window
    r = np.array([np.dot(y[:WIN - k], y[k:]) for k in range(LPC_ORDER + 1)], dtype=np.float64)
    energy = np.dot(x, x) / WIN
    r[0] *= 1.0001
    a, _ = levinson(r, LPC_ORDER)
    re = _cos @ a
    im = -(_sin @ a)
    env = -10.0 * np.log10(np.maximum(re * re + im * im, 1e-9))
    env -= env.mean()
    formants = []
    for rt in np.roots(a):
        if rt.imag <= 0:
            continue
        f = np.angle(rt) * AFS / (2 * np.pi)
        bw = -np.log(max(abs(rt), 1e-9)) * AFS / np.pi
        if F_MIN <= f <= F_MAX and bw < BW_MAX:
            formants.append((f, bw))
    formants.sort()
    return {"energy": energy, "periodicity": best, "env": env, "formants": formants, "lpc": a}


def pick_formants(formants):
    f1 = f2 = None
    for f, _ in formants:
        if f1 is None and F1_LO <= f <= F1_HI:
            f1 = f
        elif f1 is not None and f >= f1 + F2_GAP and f <= F2_HI:
            f2 = f
            break
    return f1, f2


class Model:
    """MLP weights from train.py (JSON) — or None for 'no classifier'."""

    def __init__(self, path=None):
        self.ok = path is not None
        if self.ok:
            d = json.loads(pathlib.Path(path).read_text())
            self.mu = np.array(d["mu"]); self.sd = np.array(d["sd"])
            self.W1 = np.array(d["W1"]); self.b1 = np.array(d["b1"])
            self.W2 = np.array(d["W2"]); self.b2 = np.array(d["b2"])

    def probs(self, feat):
        if not self.ok:
            return None
        h = np.maximum((feat - self.mu) / self.sd @ self.W1 + self.b1, 0.0)
        z = h @ self.W2 + self.b2
        z -= z.max()
        p = np.exp(z)
        return p / p.sum()


class Detector:
    """Streaming detector: feed 256-sample 16 kHz chunks."""

    def __init__(self, model=None, prob_smooth=0.5, min_conf=0.4):
        self.buf = np.zeros(WIN, dtype=np.float32)
        self.decim = Decimator()
        self.noise_db = -60.0
        self.cent = list(CENTROID)
        self.model = model or Model()
        self.p_s = np.full(5, 0.2)
        self.prob_smooth = prob_smooth
        self.min_conf = min_conf

    def features(self, fr, voiced):
        f1, f2 = pick_formants(fr["formants"]) if voiced else (None, None)
        if f1 is not None and f2 is not None:
            lf1, lf2 = np.log(f1), np.log(f2)
            k = 1.0 / ADAPT_FRAMES
            self.cent[0] += (lf1 - self.cent[0]) * k
            self.cent[1] += (lf2 - self.cent[1]) * k
            fmt = [lf1 - self.cent[0], lf2 - self.cent[1], 0.0]
        else:
            fmt = [0.0, 0.0, 1.0]
        cent_off = [self.cent[0] - CENTROID[0], self.cent[1] - CENTROID[1]]
        return np.concatenate([fr["env"], fmt, cent_off]), f1, f2

    def push(self, chunk):
        self.buf = np.concatenate([self.buf[AHOP:], self.decim.push(chunk)])
        fr = analyse_frame(self.buf)
        e_db = 10 * np.log10(fr["energy"] + 1e-12)
        dt = HOP / FS
        if e_db < self.noise_db:
            self.noise_db = max(self.noise_db - NOISE_DOWN_DB_S * dt, e_db)
        else:
            self.noise_db += NOISE_UP_DB_S * dt
        self.noise_db = max(self.noise_db, NOISE_FLOOR_MIN_DB)
        snr_db = e_db - self.noise_db
        voiced = snr_db > SNR_VOICED_DB and fr["periodicity"] > PERIODICITY_VOICED
        feat, f1, f2 = self.features(fr, voiced)
        probs = self.model.probs(feat) if voiced else None
        if probs is not None:
            self.p_s += (probs - self.p_s) * self.prob_smooth
        else:
            self.p_s += (np.full(5, 0.2) - self.p_s) * self.prob_smooth
        vowel = None
        if voiced and self.p_s.max() > self.min_conf:
            vowel = VOWELS[int(self.p_s.argmax())]
        # mouth pose: probability-weighted blend of the per-vowel poses
        if voiced:
            w = self.p_s / self.p_s.sum()
            open_ = sum(w[i] * MOUTH[v][0] for i, v in enumerate(VOWELS))
            form = sum(w[i] * MOUTH[v][1] for i, v in enumerate(VOWELS))
        else:
            open_, form = 0.0, -1.0
        return {"voiced": voiced, "snr_db": snr_db, "periodicity": fr["periodicity"], "f1": f1, "f2": f2,
                "feat": feat, "probs": probs, "p_s": self.p_s.copy(), "vowel": vowel, "open": open_, "form": form}


# ---------------------------------------------------------------------------
# scoring
# ---------------------------------------------------------------------------
GRP = {"a": "A", "i": "IE", "e": "IE", "u": "UO", "o": "UO"}


def score(ds, ids, model, dump=False):
    conf = {v: {w: 0 for w in VOWELS + ["none"]} for v in VOWELS}
    sil_frames = sil_false = 0
    per_speaker = {}
    for uid in ids:
        x = read_wav(ds / f"{uid}.wav")
        labs = read_lab(ds / f"{uid}.lab")
        det = Detector(model)
        sp = uid.rsplit("_", 1)[0]
        ps = per_speaker.setdefault(sp, [0, 0])
        for i in range(len(x) // HOP):
            r = det.push(x[i * HOP:(i + 1) * HOP])
            t = ((i + 1) * HOP - WIN) / FS
            lab, (into, left) = label_at(labs, t)
            if dump:
                pr = " ".join(f"{p:.2f}" for p in r["probs"]) if r["probs"] is not None else "-"
                print(f"{t:6.3f} {lab:>4} v={r['voiced']:d} snr={r['snr_db']:5.1f} per={r['periodicity']:4.2f} "
                      f"f1={r['f1'] or 0:5.0f} f2={r['f2'] or 0:5.0f} p=[{pr}] out={r['vowel'] or '-'} "
                      f"open={r['open']:.2f} form={r['form']:.2f}")
            if lab in VOWELS and into > 0.02 and left > 0.02:
                out = r["vowel"] or "none"
                conf[lab][out] += 1
                ps[1] += 1
                ps[0] += out == lab
            elif lab == "sil" and into > 0.05 and left > 0.05:
                sil_frames += 1
                sil_false += r["vowel"] is not None
    if dump:
        return
    tot = sum(sum(c.values()) for c in conf.values())
    hit = sum(conf[v][v] for v in VOWELS)
    hit3 = sum(conf[v][w] for v in VOWELS for w in VOWELS if GRP[v] == GRP[w])
    print(f"vowel frames: {tot}  accuracy: {hit / max(tot, 1):.3f}  mouth-shape (a / i,e / u,o): {hit3 / max(tot, 1):.3f}")
    print(f"silence frames: {sil_frames}  false vowel: {sil_false / max(sil_frames, 1):.3f}")
    print("confusion (rows=truth, cols=pred):")
    print("     " + "".join(f"{w:>6}" for w in VOWELS + ["none"]))
    for v in VOWELS:
        row = conf[v]
        s = sum(row.values()) or 1
        print(f"{v:>4} " + "".join(f"{row[w] / s:6.2f}" for w in VOWELS + ["none"]) + f"   n={s}")
    print("per speaker: " + "  ".join(f"{sp}={h / max(n, 1):.3f}" for sp, (h, n) in sorted(per_speaker.items())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset", type=pathlib.Path)
    ap.add_argument("--model")
    ap.add_argument("--dump")
    ap.add_argument("--dump-feat", help="utterance id: print one line per chunk for the C++ comparison")
    ap.add_argument("--speaker")
    args = ap.parse_args()
    idx = json.loads((args.dataset / "index.json").read_text())
    ids = [e["id"] for e in idx if not args.speaker or e["speaker"] == args.speaker]
    model = Model(args.model)
    if args.dump_feat:
        x = read_wav(args.dataset / f"{args.dump_feat}.wav")
        det = Detector(model)
        for i in range(len(x) // HOP):
            r = det.push(x[i * HOP:(i + 1) * HOP])
            t = ((i + 1) * HOP - WIN) / FS
            pr = r["probs"] if r["probs"] is not None else np.zeros(5)
            print(f"{t:.4f} {int(r['voiced'])} {r['f1'] or 0:.2f} {r['f2'] or 0:.2f} {r['open']:.5f} {r['form']:.5f} "
                  + " ".join(f"{v:.5f}" for v in r["feat"]) + " " + " ".join(f"{v:.5f}" for v in r["p_s"]))
        return
    if args.dump:
        score(args.dataset, [args.dump], model, dump=True)
    else:
        score(args.dataset, ids, model)


if __name__ == "__main__":
    main()
