#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""ローカル会話スタック (XiaoZhi サーバー / Strata / VOICEVOX / トンネル / stackchan-agent) の
状態とログをブラウザで見るための小さなダッシュボード。標準ライブラリのみ。

  python3 mon.py --bind 0.0.0.0 --port 8790

 * GET /            ダッシュボード (状態カード + 会話タイムライン + ライブログ)
 * GET /api/status  JSON (状態カードの中身)
 * GET /events      Server-Sent Events。XiaoZhi コンテナのログを整形して流す
 * GET /api/log?n=  直近 n 行 (整形済み) を JSON で

ログは `docker logs -f` を読むだけで、サーバー本体には手を入れない。
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

XIAOZHI_CONTAINER = "xiaozhi-esp32-server"
VOICEVOX_CONTAINER = "voicevox-engine"
USER_UNITS = ["llm-125b.service", "cloudflared-stackchan.service", "stackchan-agent.service"]
LOG_RING = 2000

# ログ行 → 種別。順番に試して最初に当たったもの。
LINE_KINDS = [
    ("asr", "识别文本:", "ASR"),
    ("llm_in", "大模型收到用户消息:", "LLM←"),
    ("tool", "执行工具:", "TOOL"),
    ("tool_req", "发送客户端mcp工具调用请求:", "TOOL→機体"),
    ("tool_ok", "客户端mcp工具调用", "TOOL結果"),
    ("tts_first", "发送第一段语音:", "TTS開始"),
    ("tts_sent", "发送音频消息: SentenceType.FIRST", "発話"),
    ("turn_end", "发送音频消息: SentenceType.LAST", "ターン終了"),
    ("tts_stop", "tts stop 送信", "stop送信"),
    ("tts_gen", "语音生成成功:", "合成"),
    ("abort", "abort消息", "ABORT"),
    ("abort", "Abort message received", "ABORT"),
    ("connect", "收到hello消息", "接続"),
    ("listen", "收到listen消息", "listen"),
    ("disconnect", "客户端断开连接", "切断"),
    ("tools", "客户端设备支持的工具数量", "機体ツール"),
    ("watchdog", "強制送信", "番犬"),
    ("uncaught", "未捕捉例外", "例外"),
    ("error", "-ERROR-", "ERROR"),
    ("warn", "-WARNING-", "WARN"),
]
NOISE = ("mic:", "it/s]", "识别语言", "识别情绪", "禁用思考模式", "收到mcp消息", "chat_history")
TS_RE = re.compile(r"(\d{6}) (\d{2}:\d{2}:\d{2})\[")
TAG_RE = re.compile(r"\[[0-9.]+_[^\]]*\]\[([^\]]+)\]-(\w+)-")


def classify(raw: str):
    if any(n in raw for n in NOISE):
        return None
    m = TS_RE.search(raw)
    ts = f"{m.group(2)}" if m else ""
    body = raw.split("]-", 2)[-1] if "]-" in raw else raw
    body = body.split("-", 1)[-1] if body.startswith(("INFO", "ERROR", "WARNING", "DEBUG")) else body
    kind, label = "other", ""
    for k, needle, lab in LINE_KINDS:
        if needle in raw:
            kind, label = k, lab
            break
    if kind == "other" and "-INFO-" in raw:
        return None  # 興味のない INFO は捨てる
    # 本文を短く: 種別キーワードより後ろだけ残す
    text = body
    for _, needle, _ in LINE_KINDS:
        if needle in text:
            idx = text.find(needle)
            text = text[idx + len(needle):].strip(" ：:，,")
            break
    return {"ts": ts, "kind": kind, "label": label, "text": text[:300], "raw": raw[:600]}


class LogTailer(threading.Thread):
    """docker logs -f を読んで整形済み行をリングに貯め、購読者へ配る。"""

    def __init__(self):
        super().__init__(daemon=True)
        self.ring = collections.deque(maxlen=LOG_RING)
        self.subs: list[collections.deque] = []  # deque はハッシュ不可なので list
        self.lock = threading.Lock()
        self.last = {}  # kind -> epoch (状態カード用)
        self.turn = {"state": "idle", "since": None, "last_user": "", "last_reply": ""}

    def run(self):
        while True:
            try:
                p = subprocess.Popen(
                    ["docker", "logs", "-f", "--since", "30m", XIAOZHI_CONTAINER],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                )
                for line in p.stdout:
                    ev = classify(line.rstrip("\n"))
                    if ev is None:
                        continue
                    self._push(ev)
                p.wait()
            except Exception as e:  # docker が居ない / コンテナ再作成中
                self._push({"ts": time.strftime("%H:%M:%S"), "kind": "error", "label": "mon",
                            "text": f"docker logs 失敗: {e}", "raw": ""})
            time.sleep(3)

    def _push(self, ev):
        now = time.time()
        ev["t"] = now
        with self.lock:
            self.ring.append(ev)
            self.last[ev["kind"]] = now
            k = ev["kind"]
            if k == "asr":
                self.turn.update(state="thinking", since=now, last_user=ev["text"])
            elif k in ("tts_first", "tts_sent"):
                if self.turn["state"] != "speaking":
                    self.turn.update(state="speaking", since=now)
                if k == "tts_sent" and ev["text"]:
                    self.turn["last_reply"] = ev["text"]
            elif k in ("turn_end", "tts_stop", "abort", "watchdog"):
                self.turn.update(state="listening", since=now)
            elif k in ("connect",):
                self.turn.update(state="listening", since=now)
            elif k == "disconnect":
                self.turn.update(state="disconnected", since=now)
            for q in list(self.subs):
                q.append(ev)

    def subscribe(self):
        q = collections.deque(maxlen=500)
        with self.lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.subs = [s for s in self.subs if s is not q]

    def recent(self, n):
        with self.lock:
            return list(self.ring)[-n:]


def sh(cmd, timeout=5):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception as e:
        return f"ERR {e}"


def container_state(name):
    out = sh(["docker", "inspect", "-f", "{{.State.Status}} {{.State.StartedAt}}", name])
    if not out or out.startswith("ERR") or "No such" in out:
        return {"up": False, "detail": out or "not found"}
    st, started = (out.split(" ", 1) + [""])[:2]
    return {"up": st == "running", "detail": st, "since": started[:19].replace("T", " ")}


def ws_connections():
    # コンテナ内の /proc/net/tcp で :8000 (0x1F40) の ESTABLISHED を数える
    out = sh(["docker", "exec", XIAOZHI_CONTAINER, "cat", "/proc/net/tcp"])
    n = 0
    for line in out.splitlines()[1:]:
        f = line.split()
        if len(f) > 3 and f[1].endswith(":1F40") and f[3] == "01":
            n += 1
    return n


def unit_state(unit):
    return sh(["systemctl", "--user", "is-active", unit]) or "unknown"


def strata_tail():
    out = sh(["journalctl", "--user", "-u", "llm-125b.service", "-n", "6", "--no-pager", "-o", "cat"])
    lines = [l for l in out.splitlines() if "[strata]" in l]
    return lines[-3:]


class Status(threading.Thread):
    def __init__(self, tailer: LogTailer):
        super().__init__(daemon=True)
        self.tailer = tailer
        self.snapshot = {}
        self.lock = threading.Lock()

    def run(self):
        while True:
            try:
                snap = {
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "xiaozhi": container_state(XIAOZHI_CONTAINER),
                    "voicevox": container_state(VOICEVOX_CONTAINER),
                    "units": {u: unit_state(u) for u in USER_UNITS},
                    "ws_connections": ws_connections(),
                    "strata_tail": strata_tail(),
                }
                with self.tailer.lock:
                    snap["last"] = {k: round(time.time() - v) for k, v in self.tailer.last.items()}
                    snap["turn"] = dict(self.tailer.turn)
                if snap["turn"]["since"]:
                    snap["turn"]["age_s"] = round(time.time() - snap["turn"]["since"])
                with self.lock:
                    self.snapshot = snap
            except Exception as e:
                with self.lock:
                    self.snapshot = {"error": str(e), "time": time.strftime("%H:%M:%S")}
            time.sleep(5)

    def get(self):
        with self.lock:
            return dict(self.snapshot)


PAGE = r"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>スタックチャン会話モニター</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e6e6;--mut:#8b93a7;--ok:#3fb950;--bad:#f85149;--warn:#d29922;--acc:#58a6ff}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,sans-serif}
header{padding:10px 16px;border-bottom:1px solid #262a33;display:flex;gap:16px;align-items:baseline;flex-wrap:wrap}
h1{font-size:16px;margin:0}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;padding:12px 16px}
.card{background:var(--card);border-radius:8px;padding:10px 12px}
.card .k{color:var(--mut);font-size:12px}.card .v{font-size:18px;font-weight:600}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}
.turn{padding:0 16px 8px;color:var(--mut)}.turn b{color:var(--fg)}
#log{margin:0 16px 16px;background:#0b0d11;border:1px solid #262a33;border-radius:8px;height:60vh;overflow:auto;font:12.5px/1.45 ui-monospace,Menlo,monospace;padding:6px 8px}
.l{white-space:pre-wrap;word-break:break-all}.l .ts{color:var(--mut)}.l .lb{display:inline-block;min-width:68px;color:var(--acc)}
.l.asr .lb{color:#e3b341}.l.tts_sent .lb,.l.tts_first .lb{color:#3fb950}.l.tool .lb,.l.tool_req .lb,.l.tool_ok .lb{color:#bc8cff}
.l.error .lb,.l.uncaught .lb,.l.disconnect .lb{color:var(--bad)}.l.warn .lb,.l.abort .lb,.l.watchdog .lb{color:var(--warn)}
.l.turn_end,.l.tts_stop,.l.listen,.l.tts_gen{color:var(--mut)}
.strata{font:12px ui-monospace,monospace;color:var(--mut);padding:0 16px 10px;white-space:pre-wrap}
label{color:var(--mut);font-size:12px;margin-left:auto}
</style></head><body>
<header><h1>スタックチャン会話モニター</h1><span id="clock" style="color:var(--mut)"></span>
<label><input type="checkbox" id="quiet" checked> 合成/ターン終了を隠す</label>
<label><input type="checkbox" id="follow" checked> 自動スクロール</label></header>
<div class="cards" id="cards"></div>
<div class="turn" id="turn"></div>
<div class="strata" id="strata"></div>
<div id="log"></div>
<script>
const $=s=>document.querySelector(s);
function card(k,v,cls){return `<div class="card"><div class="k">${k}</div><div class="v ${cls||''}">${v}</div></div>`}
function age(s){return s==null?'-':(s<60?s+' 秒前':s<3600?Math.floor(s/60)+' 分前':Math.floor(s/3600)+' 時間前')}
async function status(){
  try{const r=await fetch('/api/status');const s=await r.json();
  $('#clock').textContent=s.time||'';
  const u=s.units||{};
  const cls=x=>x==='active'?'ok':'bad';
  let h='';
  h+=card('XiaoZhi サーバー',s.xiaozhi?.up?'稼働':'停止',s.xiaozhi?.up?'ok':'bad');
  h+=card('機体 WebSocket',(s.ws_connections??'-')+' 本',s.ws_connections>0?'ok':'bad');
  h+=card('Strata (LLM)',u['llm-125b.service']||'-',cls(u['llm-125b.service']));
  h+=card('VOICEVOX',s.voicevox?.up?'稼働':'停止',s.voicevox?.up?'ok':'bad');
  h+=card('Cloudflare Tunnel',u['cloudflared-stackchan.service']||'-',cls(u['cloudflared-stackchan.service']));
  h+=card('stackchan-agent',u['stackchan-agent.service']||'-',cls(u['stackchan-agent.service']));
  const L=s.last||{};
  h+=card('最後の音声認識',age(L.asr),L.asr>300?'warn':'');
  h+=card('最後の発話',age(L.tts_sent),'');
  h+=card('最後のエラー',age(L.error??L.uncaught),(L.error??L.uncaught)<600?'bad':'');
  $('#cards').innerHTML=h;
  const t=s.turn||{};const st={idle:'待機',listening:'聞き取り中',thinking:'考え中',speaking:'話し中',disconnected:'切断'}[t.state]||t.state;
  const stuck=(t.state==='thinking'||t.state==='speaking')&&t.age_s>40;
  $('#turn').innerHTML=`会話状態: <b class="${stuck?'bad':''}">${st}</b> (${age(t.age_s)}${stuck?' — 固まっている可能性':''})　最後の聞き取り: <b>${t.last_user||'-'}</b>　最後の返答: <b>${t.last_reply||'-'}</b>`;
  $('#strata').textContent=(s.strata_tail||[]).join('\n');
  }catch(e){$('#clock').textContent='status 取得失敗: '+e}
}
const log=$('#log');const QUIET=new Set(['tts_gen','turn_end','tts_stop','listen','tool_req','tool_ok']);
function add(ev){const d=document.createElement('div');d.className='l '+ev.kind;if(QUIET.has(ev.kind))d.classList.add('q');
  d.innerHTML=`<span class="ts">${ev.ts}</span> <span class="lb">${ev.label||ev.kind}</span> ${ev.text.replace(/[<&]/g,c=>({'<':'&lt;','&':'&amp;'}[c]))}`;
  log.appendChild(d);while(log.children.length>1500)log.removeChild(log.firstChild);
  if($('#follow').checked)log.scrollTop=log.scrollHeight;}
function applyQuiet(){document.body.classList.toggle('quiet',$('#quiet').checked)}
$('#quiet').onchange=applyQuiet;applyQuiet();
const sty=document.createElement('style');sty.textContent='.quiet .l.q{display:none}';document.head.appendChild(sty);
fetch('/api/log?n=400').then(r=>r.json()).then(a=>{a.forEach(add);connect()});
function connect(){const es=new EventSource('/events');es.onmessage=e=>add(JSON.parse(e.data));es.onerror=()=>{es.close();setTimeout(connect,3000)}}
status();setInterval(status,5000);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    tailer: LogTailer
    status: Status

    def log_message(self, *a):  # 静かに
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self):
        self._send(200, "")

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if u.path == "/api/status":
            return self._send(200, json.dumps(self.status.get(), ensure_ascii=False))
        if u.path == "/api/log":
            n = int(parse_qs(u.query).get("n", ["200"])[0])
            return self._send(200, json.dumps(self.tailer.recent(min(n, LOG_RING)), ensure_ascii=False))
        if u.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            q = self.tailer.subscribe()
            try:
                last_beat = time.time()
                while True:
                    while q:
                        ev = q.popleft()
                        self.wfile.write(f"data: {json.dumps(ev, ensure_ascii=False)}\n\n".encode())
                    self.wfile.flush()
                    if time.time() - last_beat > 15:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        last_beat = time.time()
                    time.sleep(0.3)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                self.tailer.unsubscribe(q)
            return
        self._send(404, "{}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8790)
    a = ap.parse_args()
    tailer = LogTailer()
    tailer.start()
    st = Status(tailer)
    st.start()
    Handler.tailer = tailer
    Handler.status = st
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    print(f"listening on http://{a.bind}:{a.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
