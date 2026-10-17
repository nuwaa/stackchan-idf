# xiaozhi-mon — 会話スタックのモニター

XiaoZhi ローカル サーバーのログと、周辺 (Strata / VOICEVOX / Cloudflare Tunnel /
stackchan-agent) の稼働状態をブラウザで見る小さなダッシュボード。標準ライブラリだけで動く。

```sh
python3 tools/xiaozhi-mon/mon.py --bind 0.0.0.0 --port 8790
```

- `/` — 状態カード (サーバー / 機体の WebSocket 本数 / LLM / TTS / トンネル / agent、最後の音声認識・発話・エラーからの経過)、
  会話状態 (聞き取り中・考え中・話し中。40 秒以上動かなければ「固まっている可能性」)、Strata の直近ログ、ライブ ログ
- `/api/status` — 上記カードの JSON
- `/api/log?n=200` — 直近 n 行 (整形済み)
- `/events` — Server-Sent Events (ライブ ログ)

ログは `docker logs -f xiaozhi-esp32-server` を読んでいるだけで、中国語のタグは日本語ラベルに置き換えて表示する
(ASR / LLM← / TOOL / 発話 / ターン終了 / ABORT / 接続 / 切断 / ERROR など)。マイク統計などの雑音行は捨てる。

## systemd (ユーザー ユニット)

```ini
# ~/.config/systemd/user/stackchan-mon.service
[Service]
ExecStart=/usr/bin/python3 %h/repos/stackchan-idf/tools/xiaozhi-mon/mon.py --bind 0.0.0.0 --port 8790
Restart=on-failure
```

```sh
systemctl --user enable --now stackchan-mon.service
```

認証は無いので、0.0.0.0 で待ち受けるときはファイアウォールで LAN と Tailscale に限定する
(Grafana と同じ扱い)。LAN 内や Tailscale 越しのブラウザから `http://<ホストの IP>:8790/` を開く。
