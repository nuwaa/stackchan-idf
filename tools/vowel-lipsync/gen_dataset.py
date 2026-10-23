#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""Synthesise a labelled Japanese vowel dataset with a local VOICEVOX engine.

For every (speaker, sentence) pair we run /audio_query, keep the mora timing
(consonant_length / vowel_length per mora, pauses, pre/post silence) and
synthesise the wave. Output per utterance:

  <out>/<id>.wav      16 kHz mono int16 (resampled from VOICEVOX 24 kHz)
  <out>/<id>.lab      "start_s end_s label" lines; label in {a,i,u,e,o,N,cl,sil,cons}

Only the vowel portion of each mora is labelled with the vowel; the consonant
portion is "cons" and is ignored by the scorer. Devoiced vowels (VOICEVOX
upper-case A/I/U/E/O) are labelled "sil"-like ("dv") since no formants exist.
"""
import argparse, io, json, pathlib, sys, urllib.request, wave
import numpy as np

SENTENCES = [
    "あいうえお、かきくけこ、さしすせそ。",
    "おはようございます。きょうはいいてんきですね。",
    "スタックチャンはちいさなロボットです。",
    "あめがふってきたので、かさをもっていきましょう。",
    "いぬもあるけばぼうにあたる。",
    "うえをみてあるくと、あおいそらがみえます。",
    "えきまでのみちをおしえてください。",
    "おにぎりとおちゃをかってきました。",
    "こんにちは、なにかおてつだいできることはありますか。",
    "ふゆのあさはさむいけれど、ゆきがきれいです。",
    "はい、わかりました。すぐにやります。",
    "いいえ、それはちがうとおもいます。",
    "きのうはともだちとえいがをみにいきました。",
    "このりょうりはとてもおいしいですね。",
    "でんしゃがおくれているので、すこしまってください。",
    "なつやすみにうみへいって、およぎました。",
    "ぼくのなまえはスタックチャンです。よろしくね。",
    "おおきなこえでうたをうたいましょう。",
    "あしたのてんきはくもりのちはれでしょう。",
    "いまなんじですか。もうおひるですね。",
    "えんぴつとけしごむをかしてください。",
    "おとうさんはまいあさコーヒーをのみます。",
    "ゆっくりはなしてくれると、よくわかります。",
    "そのほんはどこにありますか。つくえのうえです。",
]
SPEAKERS = {"metan": 2, "zundamon": 3, "tsumugi": 8, "ritsu": 9, "takehiro": 11,
            "ryusei": 13, "himari": 14, "shiyu": 21, "hau": 10, "kotaro": 12, "sora": 16,
            "mochiko": 20, "whitecul": 23, "goki": 27, "no7": 29, "jii": 42, "sayo": 46,
            "akashi": 52, "sorin": 53, "nana": 54}


def vv(path, params, body=None):
    url = "http://localhost:50021" + path + "?" + "&".join(f"{k}={urllib.request.quote(str(v))}" for k, v in params.items())
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else b"",
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    return json.loads(data) if path != "/synthesis" else data


def resample_24k_to_16k(x):
    # Polyphase 2/3: upsample by 2 (zero-stuff), FIR low-pass, take every 3rd.
    n = len(x)
    up = np.zeros(n * 2)
    up[::2] = x
    taps = 96
    t = np.arange(-taps // 2, taps // 2) + 0.5
    fc = 1.0 / 3.0  # cutoff at 8 kHz of the 48 kHz intermediate
    h = np.sinc(fc * t) * fc * np.hanning(taps) * 2.0
    y = np.convolve(up, h, mode="same")
    return y[::3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--speakers", default=",".join(SPEAKERS))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = []
    for sp in args.speakers.split(","):
        sid = SPEAKERS[sp]
        for si, text in enumerate(SENTENCES):
            uid = f"{sp}_{si:02d}"
            q = vv("/audio_query", {"text": text, "speaker": sid})
            q["outputSamplingRate"] = 24000
            q["outputStereo"] = False
            wav = vv("/synthesis", {"speaker": sid}, q)
            with wave.open(io.BytesIO(wav)) as w:
                assert w.getframerate() == 24000 and w.getnchannels() == 1
                pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
            # VOICEVOX timing: prePhonemeLength, then moras (consonant+vowel), pause_mora after phrase.
            t = q["prePhonemeLength"]
            labs = [(0.0, t, "sil")]
            for ph in q["accent_phrases"]:
                moras = list(ph["moras"])
                if ph.get("pause_mora"):
                    moras.append(ph["pause_mora"])
                for m in moras:
                    cl = m.get("consonant_length") or 0.0
                    vl = m["vowel_length"]
                    v = m["vowel"]
                    if cl > 0:
                        labs.append((t, t + cl, "cons")); t += cl
                    if v == "pau":
                        lab = "sil"
                    elif v == "N":
                        lab = "N"
                    elif v == "cl":
                        lab = "cl"
                    elif v in ("A", "I", "U", "E", "O"):
                        lab = "dv"
                    else:
                        lab = v
                    labs.append((t, t + vl, lab)); t += vl
            labs.append((t, t + q["postPhonemeLength"], "sil"))
            # speedScale=1 → timings are as-is. Sanity check vs actual length.
            dur = len(pcm) / 24000.0
            if abs(dur - labs[-1][1]) > 0.05:
                print(f"warn {uid}: label end {labs[-1][1]:.3f} vs audio {dur:.3f}", file=sys.stderr)
            y = resample_24k_to_16k(pcm)
            y = np.clip(y, -32768, 32767).astype(np.int16)
            with wave.open(str(args.out / f"{uid}.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(y.tobytes())
            (args.out / f"{uid}.lab").write_text("".join(f"{a:.4f} {b:.4f} {l}\n" for a, b, l in labs))
            index.append({"id": uid, "speaker": sp, "text": text})
            print(uid, f"{dur:.2f}s")
    (args.out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
