# vowel-lipsync — マイク口パク用 母音検出のリファレンス実装

`components/lipsync_dsp` (機体側 C++) と同じアルゴリズムの Python 版と、学習・評価用の
スクリプト。機体側のコードは Python 版と 1:1 対応していて、ホスト テスト
(`components/lipsync_dsp/test/host`) で同じ入力に対する特徴量と出力が一致することを確認する。

## アルゴリズム

16 kHz のマイク入力を 16 ms (256 サンプル) ごとに処理する。

1. 半帯域 FIR で 8 kHz にデシメーション (F1 / F2 は 3 kHz 以下)
2. 32 ms 窓 (8 kHz で 256 サンプル、50 % 重複)
3. 有声判定: フレーム エネルギーが追従中の雑音床より 10 dB 以上高く、かつピッチ
   ラグ範囲 (60〜400 Hz) の正規化自己相関ピークが 0.5 以上
4. プリエンファシス + Hann 窓 → 自己相関 → Levinson-Durbin → 10 次 LPC
5. フォルマント構造の特徴量
   - LPC スペクトル包絡を 200〜3500 Hz の対数等間隔 24 点でサンプリング (フレーム平均を引いて音量に無関係にする)
   - LPC 多項式の根から F1 / F2 (帯域幅 500 Hz 未満の極) を取り、話者ごとの重心 (ゆっくり追従) で正規化した対数値
   - その重心の基準値からのずれ 2 次元 (声の低さ / 高さを分類器に教える。低い男声で +10 ポイント)
6. 29 → 16 → 5 の小さな MLP で「あいうえお」の確率を出し、時間方向に平滑化 (EMA)
7. 確率で各母音の口形状 (開き, 横幅) を加重平均 → 機体の `mouth_open` / `mouth_form`

F1 / F2 だけで最近傍判定する古典的な方法は、話者別に最適な中心を与えても 68 % が上限だった
(高い声で極が倍音に引かれる、あ の F1/F2 が融合する)。包絡の形をそのまま使う方が頑健。

## 使い方

```sh
# 1. VOICEVOX (localhost:50021) でラベル付きデータを合成 (20 話者 × 24 文、16 kHz wav + .lab)
python3 gen_dataset.py /path/to/dataset

# 2. 学習 (話者 1 人抜き交差検証の精度を表示) → model.json + 機体用ヘッダ
OMP_NUM_THREADS=1 python3 train.py /path/to/dataset --out model.json \
    --header ../../components/lipsync_dsp/vowel_model.hpp

# 3. ストリーミング パイプラインの採点 / 1 発話のフレーム ダンプ
python3 proto.py /path/to/dataset --model model.json
python3 proto.py /path/to/dataset --model model.json --dump metan_00

# 4. C++ 実装との一致確認
python3 proto.py /path/to/dataset --model model.json --dump-feat metan_00 > metan_00.tsv
(cd ../../components/lipsync_dsp/test/host && cmake -S . -B build && cmake --build build \
    && ./build/vowel_test /path/to/dataset --compare metan_00 metan_00.tsv \
    && ./build/vowel_test /path/to/dataset)
```

ラベルは VOICEVOX の audio_query が返すモーラ長 (子音長 / 母音長) から作る。母音区間の
両端 20 ms を除いたフレームで採点する。無声化母音 (VOICEVOX の大文字) は採点対象外。

## 精度 (VOICEVOX 合成音声、フレーム単位)

| 条件 | 5 母音 | 口形状 3 クラス (あ / い・え / う・お) |
|---|---|---|
| 話者 1 人抜き交差検証 (20 話者) | 約 70 % | 約 77 % |
| 長い母音の中央部のみ | 80〜100 % (話者による) | — |

誤りの多くは母音の遷移部 (フレーム単位の正解自体があいまい) と、低い男声 / 高い女声の
極端な話者。合成音声での数字なので、実機ではマイク特性と部屋の雑音でずれる。機体の
`mic-lip` タグを DEBUG にすると 0.5 秒ごとに検出母音・F1/F2・確率がログに出るので、それで
しきい値 (`VowelDetector::Tuning`) を調整する。
