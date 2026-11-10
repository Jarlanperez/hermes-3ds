#!/usr/bin/env python3
"""Hermes for Nintendo 3DS: the relay between the 3DS app and your Hermes Agent.

The 3DS can't talk to Hermes directly (no modern TLS, tiny RAM), so this small server runs on the
computer next to Hermes and does the heavy lifting:
- Pairing instead of passwords: a new 3DS shows a 4-letter code; approve it with
  `python3 relay.py pair CODE` (or ask Hermes to, with the hermes-3ds skill).
- Talks to the Hermes API server (/v1/runs) with your API key. The key never leaves this computer.
- Voice in: forwards push-to-talk audio to any OpenAI-compatible speech-to-text server (local Whisper).
- Voice out (optional): fetches speech from any OpenAI-compatible TTS server (Kokoro, VoiceStudio).
- Home network only: requests from outside private address ranges are refused. Plain HTTP because the
  3DS can't do modern TLS, so never expose this port to the internet (no port forward, no tunnel).

Usage:  python3 relay.py              run the relay
        python3 relay.py pair CODE    approve a 3DS showing CODE
        python3 relay.py devices      list paired and waiting devices
        python3 relay.py doctor       check Hermes, speech-to-text and voice
Settings: ~/.hermes-3ds/config.json (see config.example.json). Every key is optional.
"""
import sys, base64, html, http.server, io, ipaddress, json, os, re, secrets, socket, socketserver, threading, time, urllib.error, urllib.parse, urllib.request, uuid, wave

DIR = os.path.expanduser(os.environ.get("HERMES3DS_DIR", "~/.hermes-3ds"))
DEFAULTS = {
    "name": "",                         # shown as HEY <NAME> on the 3DS and used in the prompt
    "say_name_as": "",                  # how the voice should say it, e.g. "Shiv-awn" or Kokoro IPA
    "port": 8766,
    "hermes_url": "http://127.0.0.1:8642",
    "api_key": "",                      # empty: read API_SERVER_KEY from hermes_env
    "hermes_env": "~/.hermes/.env",
    "stt_url": "http://127.0.0.1:8178/v1/audio/transcriptions",
    "stt_model": "whisper-1",
    "voice_url": "http://127.0.0.1:8880/v1/audio/speech",   # empty string turns talk back off
    "voice_model": "kokoro",
    "voice_name": "af_heart",
    "voice_key": "local",
}

def load_config():
    c = dict(DEFAULTS)
    try:
        c.update(json.load(open(os.path.join(DIR, "config.json"))))
    except FileNotFoundError:
        pass
    return c

CONF = load_config()
PORT = int(os.environ.get("HERMES3DS_PORT", CONF["port"]))
DEVICES = os.path.join(DIR, "devices.json")
HERMES_BASE = CONF["hermes_url"].rstrip("/")
HERMES = HERMES_BASE + "/v1/chat/completions"
WHISPER = CONF["stt_url"]
MIC_RATE = 16360
OWNER = CONF["name"].strip()
WHO = f"{OWNER}'s" if OWNER else "the user's"
SYSTEM = (f"You are replying on {WHO} Nintendo 3DS through a tiny web page (400x240 screen, old browser). "
          "Plain text only: no Markdown, no tables, no code blocks, no emoji. Keep replies under 500 characters "
          "unless they ask for more. Same Hermes, same memory; just a very small screen.")
APP_SYSTEM = (f"You are replying on {WHO} Nintendo 3DS in the Hermes 3DS homebrew app. They usually talk to you by "
              "push-to-talk, so the message is a speech transcript and may have small errors; read for intent. "
              "Your reply appears in a 256px-wide dialogue box beside the Hermes Girl sprite. Plain text only: no Markdown, "
              "no tables, no emoji. Be fast and brief: one to three short sentences, under 240 characters, "
              "unless they ask for more. Answer directly with no preamble. Use a tool only when the question needs one. "
              "When asked for a drawing or ASCII art, draw it yourself (no tools) inside one ``` code block, plain ASCII "
              "only, at most 40 characters wide and 12 lines tall, plus one short line of text; the 3DS shows it in a "
              "monospace font. Never use code blocks for anything else.")

def api_key():
    k = os.environ.get("HERMES3DS_API_KEY") or CONF["api_key"]
    if k:
        return k
    try:
        for line in open(os.path.expanduser(CONF["hermes_env"])):
            if line.startswith("API_SERVER_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    except FileNotFoundError:
        pass
    raise SystemExit("No Hermes API key. Set API_SERVER_KEY in ~/.hermes/.env (and API_SERVER_ENABLED=true), "
                     "or api_key in ~/.hermes-3ds/config.json.")

KEY = None   # read at startup (not needed for `pair` / `devices`)
lock = threading.Lock()
pending = {}   # code -> {"token", "ua", "t"}
jobs = {}      # token -> {"state": idle|thinking|done|error, "q", "a", "t"}

def load():
    try:
        return json.load(open(DEVICES))
    except Exception:
        return {}

def save(d):
    tmp = DEVICES + ".tmp"
    with open(tmp, "w") as f:
        json.dump(d, f, indent=1)
    os.chmod(tmp, 0o600)
    os.replace(tmp, DEVICES)

devices = load()   # token -> {"name", "session", "history": [[q, a, t], ...], "paired"}

def private(ip):
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return a.is_loopback or (a.is_private and not a in ipaddress.ip_network("100.64.0.0/10"))

def plain(text):
    # code blocks are ASCII art on the 3DS: keep them verbatim, each line marked \x1e so the app draws it
    # unwrapped in the mono font (the markdown cleanup below must not touch art)
    arts = []
    def keep(m):
        body = m.group(1).strip("\n")
        arts.append("\n".join("\x1e" + ln.rstrip() for ln in body.split("\n")))
        return f"\x00ART{len(arts) - 1}\x00"
    t = re.sub(r"```[^\n]*\n(.*?)(?:```|$)", keep, text, flags=re.S)
    t = t.replace("```", "")
    t = re.sub(r"\*\*(.+?)\*\*", r"\1", t)
    t = re.sub(r"(?m)^#+\s*", "", t)
    t = re.sub(r"`([^`]+)`", r"\1", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)
    for i, a in enumerate(arts):
        t = t.replace(f"\x00ART{i}\x00", "\n" + a + "\n")
    return re.sub(r"\n{3,}", "\n\n", t).strip(" \t\r\n")   # not .strip(): Python counts \x1e as whitespace

def ask(token, q, system=SYSTEM):
    dev = devices[token]
    body = json.dumps({"model": "hermes", "messages": [{"role": "system", "content": system}, {"role": "user", "content": q}]}).encode()
    headers = {"Authorization": f"Bearer {KEY}", "Content-Type": "application/json"}
    if dev.get("session"):
        headers["X-Hermes-Session-Id"] = dev["session"]
    try:
        req = urllib.request.Request(HERMES, data=body, headers=headers)
        with urllib.request.urlopen(req, timeout=600) as r:
            sid = r.headers.get("X-Hermes-Session-Id")
            data = json.load(r)
        a = plain(data["choices"][0]["message"]["content"] or "(no reply)")
        with lock:
            if sid:
                dev["session"] = sid
            dev.setdefault("history", []).append([q, a, int(time.time())])
            dev["history"] = dev["history"][-20:]
            save(devices)
            jobs[token] = {"state": "done", "t": time.time()}
    except Exception as e:
        with lock:
            jobs[token] = {"state": "error", "err": str(e)[:200], "q": q, "t": time.time()}

def voice(token, pcm):
    """Raw PCM16 mono from the 3DS mic -> WAV -> local Whisper -> ask()."""
    try:
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(MIC_RATE)
            w.writeframes(pcm[: len(pcm) // 2 * 2])
        b = uuid.uuid4().hex
        body = (f"--{b}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n{CONF['stt_model']}\r\n"
                f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"3ds.wav\"\r\n"
                f"Content-Type: audio/wav\r\n\r\n").encode() + buf.getvalue() + f"\r\n--{b}--\r\n".encode()
        req = urllib.request.Request(WHISPER, data=body, headers={"Content-Type": f"multipart/form-data; boundary={b}"})
        with urllib.request.urlopen(req, timeout=120) as r:
            text = (json.load(r).get("text") or "").strip()
    except Exception as e:
        with lock:
            jobs[token] = {"state": "error", "err": "transcription: " + str(e)[:160], "q": "", "t": time.time()}
        return
    if not text:
        with lock:
            jobs[token] = {"state": "error", "err": "I didn't catch anything. Hold A while you talk.", "q": "", "t": time.time()}
        return
    with lock:
        jobs[token] = {"state": "thinking", "q": text, "t": time.time()}
    ask(token, text, APP_SYSTEM)

CSS = ("body{margin:0;padding:6px 8px;background:#1c1d1d;color:#e8ebe5;font-family:sans-serif;font-size:14px}"
       "h1{font-size:13px;letter-spacing:2px;margin:0 0 6px;padding-bottom:4px;border-bottom:1px solid #e8ebe5}"
       ".me{color:#a4aaa2;margin:8px 0 2px}.me b{color:#e8ebe5}.a{margin:0 0 8px;white-space:pre-wrap;line-height:1.35}"
       ".dim{color:#a4aaa2;font-size:12px}.live{color:#f05a28}"
       "input.t{width:300px;font-size:14px;padding:4px;border:1px solid #e8ebe5;background:#2a2b2b;color:#e8ebe5}"
       "input.b{font-size:14px;padding:4px 10px;background:#e8ebe5;color:#1c1d1d;border:0}"
       ".code{font-size:40px;letter-spacing:10px;margin:14px 0}a{color:#e8ebe5}")

def page(body, refresh=None):
    meta = f'<meta http-equiv="refresh" content="{refresh}">' if refresh else ""
    return ('<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=400">'
            f'{meta}<title>Hermes</title><style>{CSS}</style></head><body><h1>HERMES &middot; 3DS</h1>{body}</body></html>')

class H(http.server.BaseHTTPRequestHandler):
    server_version = "hermes-3ds"

    def log_message(self, fmt, *args):
        pass

    def send(self, code, html_text, cookie=None, location=None):
        b = html_text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        if cookie:
            self.send_header("Set-Cookie", f"hh={cookie}; Path=/; Max-Age=31536000; HttpOnly")
        if location:
            self.send_header("Location", location)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def token(self):
        m = re.search(r"(?:^|;\s*)hh=([A-Za-z0-9_-]+)", self.headers.get("Cookie", ""))
        return m.group(1) if m else None

    def gate(self):
        if not private(self.client_address[0]):
            self.send(403, page("<p>Home network only.</p>"))
            return False
        return True

    def do_GET(self):
        if not self.gate():
            return
        path = urllib.parse.urlparse(self.path).path
        if path.startswith("/app/"):
            return self.app_get(path)
        if path == "/devices" and self.client_address[0] == "127.0.0.1":
            with lock:
                out = {"paired": [{"name": v.get("name"), "paired": v.get("paired")} for v in devices.values()],
                       "pending": [{"code": c, "ua": p["ua"][:80], "age_s": int(time.time() - p["t"])} for c, p in pending.items()]}
            b = json.dumps(out).encode()
            self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
            return
        tok = self.token()
        with lock:
            dev = devices.get(tok) if tok else None
            job = jobs.get(tok, {}) if tok else {}
        if not dev:
            return self.pairing(tok)
        hist = dev.get("history", [])[-6:]
        convo = "".join(f'<p class="me">You: <b>{html.escape(q)}</b></p><p class="a">{html.escape(a)}</p>' for q, a, _ in hist)
        if job.get("state") == "thinking":
            secs = int(time.time() - job["t"])
            body = (convo + f'<p class="me">You: <b>{html.escape(job["q"])}</b></p>'
                    f'<p class="live">Hermes is thinking&hellip; {secs}s</p><p class="dim">This page refreshes on its own.</p>')
            return self.send(200, page(body, refresh=3))
        err = f'<p class="live">That one failed: {html.escape(job.get("err", ""))}</p>' if job.get("state") == "error" else ""
        if not hist and not err:
            convo = '<p class="dim">Same Hermes, on a 3DS. Ask anything.</p>'
        form = ('<form method="post" action="/ask" accept-charset="utf-8"><input class="t" type="text" name="q" maxlength="1000" autocomplete="off"> '
                '<input class="b" type="submit" value="Send"></form><p class="dim"><a href="/">Refresh</a></p>')
        self.send(200, page(convo + err + form))

    def pairing(self, tok):
        ua = self.headers.get("User-Agent", "")
        with lock:
            for c, p in list(pending.items()):
                if time.time() - p["t"] > 600:
                    del pending[c]
            code = next((c for c, p in pending.items() if p["token"] == tok), None)
            if not code:
                if len(pending) >= 5:
                    return self.send(429, page("<p>Too many pairing requests. Try again in a few minutes.</p>"))
                tok = secrets.token_urlsafe(24)
                code = "".join(secrets.choice("ACDEFHJKMNPRTWXY") for _ in range(4))
                pending[code] = {"token": tok, "ua": ua, "t": time.time()}
        body = ('<p>New device. Tell Hermes this code to pair it:</p>'
                f'<p class="code">{code}</p><p class="dim">Expires in 10 minutes. This page checks every 5 seconds.</p>')
        self.send(200, page(body, refresh=5), cookie=tok)

    def do_POST(self):
        if not self.gate():
            return
        u = urllib.parse.urlparse(self.path)
        if u.path.startswith("/app/"):
            return self.app_post(u.path)
        if u.path == "/approve":
            if self.client_address[0] != "127.0.0.1":
                return self.send(403, page("<p>Approve from the Mac only.</p>"))
            code = urllib.parse.parse_qs(u.query).get("code", [""])[0].upper()
            name = urllib.parse.parse_qs(u.query).get("name", ["3DS"])[0][:40]
            with lock:
                p = pending.pop(code, None)
                if p:
                    devices[p["token"]] = {"name": name, "paired": time.strftime("%Y-%m-%d %H:%M"), "history": []}
                    save(devices)
            return self.send(200 if p else 404, "approved" if p else "no such code")
        if u.path == "/ask":
            tok = self.token()
            n = min(int(self.headers.get("Content-Length", "0") or 0), 8000)
            q = urllib.parse.parse_qs(self.rfile.read(n).decode("utf-8", "replace")).get("q", [""])[0].strip()[:1000]
            with lock:
                ok = tok in devices
                busy = jobs.get(tok, {}).get("state") == "thinking"
                if ok and q and not busy:
                    jobs[tok] = {"state": "thinking", "q": q, "t": time.time()}
            if ok and q and not busy:
                threading.Thread(target=ask, args=(tok, q), daemon=True).start()
            self.send_response(303); self.send_header("Location", "/"); self.send_header("Content-Length", "0"); self.end_headers()
            return
        self.send(404, page("<p>Not found.</p>"))

    # ---------- native 3DS app (push to talk) ----------
    # Token travels in X-HH-Token. Bodies are plain text so the 3DS needs no JSON parser.
    # GET /app/poll -> "state\nquestion\nanswer" (state: pending|unpaired|idle|transcribing|thinking|done|error)

    def plain_send(self, code, text):
        b = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def app_get(self, path):
        if path.startswith("/app/v2/"):
            return self.app2_get(path)
        if path != "/app/poll":
            return self.plain_send(404, "not found")
        tok = self.headers.get("X-HH-Token", "")
        with lock:
            if tok and any(p["token"] == tok for p in pending.values()):
                return self.plain_send(200, "pending\n\n")
            dev = devices.get(tok) if tok else None
            if not dev:
                return self.plain_send(200, "unpaired\n\n")
            job = dict(jobs.get(tok, {}))
            last = (dev.get("history") or [[None, None, 0]])[-1]
        state = job.get("state", "idle")
        if state == "done":
            q, a = last[0] or "", last[1] or ""
        elif state == "error":
            q, a = job.get("q", ""), "That one failed: " + job.get("err", "")
        else:
            q, a = job.get("q", ""), ""
        q = q.replace("\n", " ")
        self.plain_send(200, f"{state}\n{q}\n{a}")

    def app_post(self, path):
        if path == "/app/pair":
            ua = self.headers.get("User-Agent", "")
            with lock:
                for c, p in list(pending.items()):
                    if time.time() - p["t"] > 600:
                        del pending[c]
                if len(pending) >= 5:
                    return self.plain_send(429, "busy")
                tok = secrets.token_urlsafe(24)
                code = "".join(secrets.choice("ACDEFHJKMNPRTWXY") for _ in range(4))
                pending[code] = {"token": tok, "ua": ua or "3DS app", "t": time.time()}
            return self.plain_send(200, f"{code}\n{tok}")
        tok = self.headers.get("X-HH-Token", "")
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > 2_000_000:
            return self.plain_send(413, "too big")
        data = self.rfile.read(n)
        with lock:
            if tok not in devices:
                return self.plain_send(403, "unpaired")
        if path.startswith("/app/v2/"):
            return self.app2_post(path, tok, data)
        with lock:
            if jobs.get(tok, {}).get("state") in BUSY:
                return self.plain_send(409, "busy")
        if path == "/app/ask":
            q = data.decode("utf-8", "replace").strip()[:1000]
            if not q:
                return self.plain_send(400, "empty")
            with lock:
                jobs[tok] = {"state": "thinking", "q": q, "t": time.time()}
            threading.Thread(target=ask, args=(tok, q, APP_SYSTEM), daemon=True).start()
            return self.plain_send(202, "ok")
        if path == "/app/voice":
            with lock:
                jobs[tok] = {"state": "transcribing", "q": "", "t": time.time()}
            threading.Thread(target=voice, args=(tok, data), daemon=True).start()
            return self.plain_send(202, "ok")
        self.plain_send(404, "not found")

    # ---------- native app v2: /v1/runs (streamed reply, live tool state, approvals, stop, follow-up) ----------
    # GET /app/v2/poll -> state\njob_id\nkind\nelapsed_s\nhelpers\ntool_label\nquestion\napproval_command\napproval_description\nanswer...
    #   state: pending|unpaired|idle|transcribing|thinking|working|approval|replying|done|error
    #   kind:  think|coding|research|making|delegate
    # GET /app/v2/earlier -> one row per turn, newest first: HH:MM \t question \t answer (newlines as \x1f)

    def app2_get(self, path):
        tok = self.headers.get("X-HH-Token", "")
        with lock:
            if tok and any(p["token"] == tok for p in pending.values()):
                return self.plain_send(200, "pending\n")
            dev = devices.get(tok) if tok else None
            if not dev:
                return self.plain_send(200, "unpaired\n")
            job = dict(jobs.get(tok, {}))
            hist = list(dev.get("history") or [])
        if path == "/app/v2/earlier":
            rows = []
            for q, a, t in reversed(hist[-6:]):
                one = lambda s: (s or "").replace("\t", " ").replace("\r", "").replace("\n", "\x1f")
                rows.append(f"{time.strftime('%H:%M', time.localtime(t))}\t{one(q)}\t{one(a)}")
            return self.plain_send(200, fit("\n".join(rows)))
        if path == "/app/v2/hello":   # the name for the HEY <NAME> screen (empty = HELLO)
            return self.plain_send(200, fit(OWNER[:12].upper()))
        if path == "/app/v2/cue":   # short spoken beat while a photo is studied, in the persona's voice
            pcm = cue_pcm(job.get("persona", "hermes"))
            if not pcm:
                return self.plain_send(204, "")
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(pcm)))
            self.end_headers()
            self.wfile.write(pcm)
            return
        if path == "/app/v2/speech":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            part = int(q["part"][0]) if q.get("part", [""])[0].isdigit() else None
            return self.speech_send(tok, job, part)
        if path != "/app/v2/poll":
            return self.plain_send(404, "not found")
        state = job.get("state", "idle")
        appr = job.get("approval") or {}
        one = lambda s: (s or "").replace("\r", "").replace("\n", " ")
        end = job.get("t_end") if state in ("done", "error") else None
        ans = job.get("a", "")
        if state == "replying":
            ans = plain(ans)
        fields = [state, "%.3f" % job.get("t", 0), job.get("kind", "think"), str(int((end or time.time()) - job.get("t", time.time()))),
                  str(job.get("helpers", 0)), one(job.get("tool", "")), one(job.get("q", "")),
                  one(appr.get("command", "")), one(appr.get("description", "")), ans]
        self.plain_send(200, fit("\n".join(fields)))

    def speech_send(self, tok, job, part=None):
        """The finished reply as raw PCM16 mono at SPEECH_RATE (Kokoro on local VoiceStudio), cached per job.
        204 = nothing to say yet, 503 = voice server down."""
        if job.get("state") != "done" or not job.get("a"):
            return self.plain_send(204, "")
        key = "%.3f" % job.get("t", 0)
        e = voice_job(tok, key, job["a"])
        deadline = time.time() + 30
        with e["cv"]:
            want = range(len(e["parts"])) if part is None else [part]
            while time.time() < deadline and any(i < len(e["parts"]) and e["parts"][i] is None for i in want) and not e["failed"]:
                e["cv"].wait(0.5)
            if part is not None and part >= len(e["parts"]):
                return self.plain_send(204, "")
            got = [e["parts"][i] for i in want if i < len(e["parts"])]
        if not got or any(g is None for g in got):
            return self.plain_send(503, "voice offline")
        pcm = b"".join(got)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(pcm)))
        self.end_headers()
        self.wfile.write(pcm)

    def app2_post(self, path, tok, data):
        with lock:
            job = jobs.get(tok, {})
            run_id = job.get("run")
            busy = job.get("state") in BUSY
        text = data.decode("utf-8", "replace").strip()
        if path == "/app/v2/photo":   # raw 400x240 RGB565 from the 3DS camera; empty body = discard
            if not data:
                with lock:
                    photos.pop(tok, None)
                print("PHOTO dropped", flush=True)
                return self.plain_send(200, "dropped")
            if data[:2] == b"\xff\xd8" and len(data) < 2_000_000:   # v2.8.1+/v2.6+: JPEG encoded on the 3DS
                img = ("image/jpeg", data)
            elif len(data) == PHOTO_W * PHOTO_H * 2:              # older apps: raw RGB565
                try:
                    img = ("image/png", rgb565_png(data))
                except Exception:
                    print("PHOTO encode failed", flush=True)
                    return self.plain_send(500, "encode failed")
            else:
                print(f"PHOTO bad size {len(data)}", flush=True)
                return self.plain_send(400, "bad size")
            with lock:
                photos[tok] = (time.time(), img)
            print(f"PHOTO stored {img[0]} {len(img[1])} bytes", flush=True)
            return self.plain_send(200, "ok")
        if path == "/app/v2/ask":
            if not text:
                return self.plain_send(400, "empty")
            if busy:
                return self.plain_send(202 if run_id and steer(run_id, text) else 409, "steered")
            with lock:
                jobs[tok] = {"state": "thinking", "kind": "think", "q": text[:1000], "t": time.time(), "a": ""}
            threading.Thread(target=run_turn, args=(tok, text[:1000]), daemon=True).start()
            return self.plain_send(202, "ok")
        if path == "/app/v2/voice":
            if busy:   # push-to-talk while working = follow-up on the same task
                threading.Thread(target=voice_steer, args=(tok, run_id, data), daemon=True).start()
                return self.plain_send(202, "steer")
            with lock:
                jobs[tok] = {"state": "transcribing", "kind": "think", "q": "", "t": time.time(), "a": ""}
            threading.Thread(target=voice2, args=(tok, data), daemon=True).start()
            return self.plain_send(202, "ok")
        if path == "/app/v2/approve":
            choice = text if text in ("once", "session", "deny") else "deny"
            if not run_id or job.get("state") != "approval":
                return self.plain_send(409, "no approval pending")
            code, _ = hermes_call("POST", f"{RUNS}/{run_id}/approval", {"choice": choice})
            return self.plain_send(200 if code == 200 else 409, "ok" if code == 200 else "failed")
        if path == "/app/v2/reset":   # finish the conversation: clear the screen state, next turn starts a new session
            if busy:
                return self.plain_send(409, "busy")
            with lock:
                jobs[tok] = {}
                devices[tok].pop("session", None)
                save(devices)
            return self.plain_send(200, "reset")
        if path == "/app/v2/stop":
            if not run_id or not busy:
                return self.plain_send(409, "nothing running")
            code, _ = hermes_call("POST", f"{RUNS}/{run_id}/stop", {})
            return self.plain_send(200 if code == 200 else 409, "stopping")
        self.plain_send(404, "not found")


# ---------- camera: a snapshot rides along with the next question ----------
PHOTO_W, PHOTO_H, PHOTO_TTL = 400, 240, 600
photos = {}          # token -> (time, png bytes)

def rgb565_png(raw):
    """3DS camera frame (little-endian RGB565) -> PNG, stdlib only (the LaunchAgent python has no PIL)."""
    import array, struct, zlib
    px = array.array("H")
    px.frombytes(raw)
    if sys.byteorder != "little":
        px.byteswap()
    rows = bytearray()
    for y in range(PHOTO_H):
        rows.append(0)
        for v in px[y * PHOTO_W:(y + 1) * PHOTO_W]:
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", PHOTO_W, PHOTO_H, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))

def take_photo(token):
    with lock:
        t, img = photos.pop(token, (0, None))
    return img if img and time.time() - t < PHOTO_TTL else None

# ---------- talk back: Kokoro "Heart" on local VoiceStudio ----------
VOICE_URL = CONF["voice_url"]
VOICE_NAME = CONF["voice_name"]
SPEECH_RATE = 24000          # the 3DS app plays mono PCM16 at this rate
speech_lock = threading.Lock()
speech_cache = {}

def spoken(text):
    """What she says: the reply minus ASCII-art rows, with the owner's name said the way they want."""
    rows = [r.strip() for r in (text or "").split("\n") if r.strip() and not r.startswith("\x1e")]
    t = re.sub(r"\s+", " ", " ".join(rows)).strip()
    if OWNER and CONF["say_name_as"]:
        t = re.sub(r"\b%s\b" % re.escape(OWNER), lambda m: CONF["say_name_as"], t)
    return t[:700]

def chunks(text):
    """Her reply in speakable pieces: the first sentence alone (so she starts fast), then ~220-char groups."""
    t = spoken(text)
    if not t:
        return []
    sents = [x for x in re.split(r"(?<=[.!?])\s+", t) if x]
    out = [sents[0]]
    i = 1
    while len(out[0]) < 24 and i < len(sents):   # "Sure." alone is too short to be worth a round trip
        out[0] += " " + sents[i]; i += 1
    cur = ""
    for x in sents[i:]:
        if cur and len(cur) + len(x) > 220:
            out.append(cur); cur = x
        else:
            cur = (cur + " " + x).strip()
    if cur:
        out.append(cur)
    return out

def to_mono_rate(frames, ch, rate):
    """PCM16 -> mono at SPEECH_RATE. Pure Python (audioop is gone in Python 3.13)."""
    import array
    x = array.array("h"); x.frombytes(frames)
    if sys.byteorder != "little":
        x.byteswap()
    if ch > 1:
        x = array.array("h", (sum(x[i:i + ch]) // ch for i in range(0, len(x) - ch + 1, ch)))
    if rate != SPEECH_RATE and len(x) > 1:
        n = int(len(x) * SPEECH_RATE / rate)
        step = (len(x) - 1) / max(1, n - 1)
        y = array.array("h", bytes(2 * n))
        for i in range(n):
            f = i * step; j = int(f); t = f - j
            y[i] = int(x[j] * (1 - t) + x[min(j + 1, len(x) - 1)] * t)
        x = y
    if sys.byteorder != "little":
        x.byteswap()
    return x.tobytes()

def speak(text):
    return speak_raw(spoken(text))

def speak_raw(t):
    if not t or not VOICE_URL:
        return None
    body = json.dumps({"model": CONF["voice_model"], "voice": VOICE_NAME, "input": t, "response_format": "wav"}).encode()
    req = urllib.request.Request(VOICE_URL, body, {"Authorization": f"Bearer {CONF['voice_key']}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        with wave.open(io.BytesIO(data)) as w:
            ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            frames = w.readframes(w.getnframes())
    except Exception:
        return None
    if sw != 2:
        return None
    if ch != 1 or rate != SPEECH_RATE:
        frames = to_mono_rate(frames, ch, rate)
    return frames[: SPEECH_RATE * 2 * 30]   # 30 s cap

# ---------- v2 run driver ----------
RUNS = HERMES_BASE + "/v1/runs"
HANDHELD_REASONING = os.environ.get("HANDHELD_REASONING", "low")
BUSY = ("transcribing", "thinking", "working", "approval", "replying")
CODING = {"terminal", "execute_code", "patch", "write_file", "read_file", "search_files", "process", "process_manage"}
MAKING = {"image_generate", "text_to_speech", "video_generate", "music_generate"}
LABEL = {"terminal": "Running a command", "execute_code": "Running code", "patch": "Editing a file",
         "write_file": "Writing a file", "read_file": "Reading a file", "search_files": "Searching files",
         "web_search": "Searching the web", "web_extract": "Reading a page", "browser_exec": "Using the browser",
         "session_search": "Checking past chats", "image_generate": "Making an image",
         "text_to_speech": "Making audio", "delegate_task": "Sending helpers", "vision_analyze": "Looking at an image"}

GLYPHS = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | {0x2190, 0x2191, 0x2192, 0x2193, 0x25BC, 0x2022, 0x2026,
                                                                0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D}
SWAP = {"\u2212": "-", "\u2010": "-", "\u2011": "-", "\u00ad": "", "\u200b": "", "\u2032": "'", "\u2033": '"',
        "\u2713": "ok", "\u2714": "ok", "\u2717": "x", "\u2192": "\u2192", "\u21d2": "\u2192", "\u2265": ">=", "\u2264": "<="}

def fit(text):
    """Keep only characters the 3DS app's bitmap fonts carry (tools/build_fonts.sh whitelist)."""
    import unicodedata
    out = []
    for ch in text or "":
        if ch in ("\n", "\t", "\x1e") or ord(ch) in GLYPHS:
            out.append(ch)
        elif ch in SWAP:
            out.append(SWAP[ch])
        else:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if ord(c) in GLYPHS)
            out.append(base)
    return "".join(out)

def tool_kind(tool):
    t = tool or ""
    if t in CODING: return "coding"
    if t in MAKING or "image" in t: return "making"
    if t == "delegate_task": return "delegate"
    if t.startswith("web_") or "browser" in t or t in ("session_search", "vision_analyze", "skill_view"): return "research"
    return "think"

def hermes_call(method, url, body=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {KEY}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, {}
    except Exception:
        return 0, {}

def steer(run_id, text):
    code, _ = hermes_call("POST", f"{RUNS}/{run_id}/steer", {"input": text})
    return code == 200

def transcribe(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(MIC_RATE)
        w.writeframes(pcm[: len(pcm) // 2 * 2])
    b = uuid.uuid4().hex
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n{CONF['stt_model']}\r\n"
            f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"3ds.wav\"\r\n"
            f"Content-Type: audio/wav\r\n\r\n").encode() + buf.getvalue() + f"\r\n--{b}--\r\n".encode()
    req = urllib.request.Request(WHISPER, data=body, headers={"Content-Type": f"multipart/form-data; boundary={b}"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return (json.load(r).get("text") or "").strip()

def mark(token, k):
    """Turn timing: seconds since the 3DS request arrived, logged per step (speed work)."""
    with lock:
        j = jobs.get(token) or {}
        tm = j.setdefault("tm", {})
        tm.setdefault(k, round(time.time() - j.get("t", time.time()), 2))

def voice2(token, pcm):
    try:
        text = transcribe(pcm)
        mark(token, "whisper")
    except Exception as e:
        with lock:
            jobs[token] = {"state": "error", "q": "", "t": time.time(), "a": "Couldn't transcribe that: " + str(e)[:120]}
        return
    if not text:
        with lock:
            jobs[token] = {"state": "error", "q": "", "t": time.time(), "a": "I didn't catch anything. Hold A while you talk."}
        return
    with lock:
        jobs[token].update(state="thinking", q=text)
    run_turn(token, text)

def voice_steer(token, run_id, pcm):
    try:
        text = transcribe(pcm)
    except Exception:
        return
    if text and run_id:
        steer(run_id, text)

def _job(token, **kw):
    with lock:
        j = jobs.setdefault(token, {})
        j.update(kw)

def run_turn(token, q):
    """Start a /v1/runs turn for this device and mirror its event stream into jobs[token]."""
    with lock:
        dev = devices[token]
        session = dev.get("session")
    body = {"input": q, "instructions": APP_SYSTEM}   # no "model": the API server would pass it to the provider as a real id
    body["model_options"] = {"reasoning": {"effort": HANDHELD_REASONING}}   # handheld turns are short: low reasoning ~halves latency
    png = take_photo(token)
    print(f"PHOTO {'attached ' + str(len(png[1])) + ' bytes' if png else 'none'} for: {q[:50]}", flush=True)
    if png:   # they snapped a photo first: send it with this question
        note = q + "\n\n(Attached: a photo they just took with the 3DS outer camera, 0.1 MP, often dim or soft.)"
        body["input"] = [{"role": "user", "content": [
            {"type": "text", "text": note},
            {"type": "image_url", "image_url": {"url": "data:" + png[0] + ";base64," + base64.b64encode(png[1]).decode()}}]}]
        _job(token, q="(photo) " + q[:990])
    if session:
        body["session_id"] = session
    code, r = hermes_call("POST", RUNS, body)
    mark(token, "accepted")
    run_id = r.get("run_id") if code == 202 else None
    if not run_id:
        _job(token, state="error", a=f"Hermes didn't take it (HTTP {code}).")
        return
    _job(token, run=run_id, state="thinking", kind="looking" if png else "think", helpers=0, a="")
    final = None
    try:
        req = urllib.request.Request(f"{RUNS}/{run_id}/events", headers={"Authorization": f"Bearer {KEY}"})
        with urllib.request.urlopen(req, timeout=900) as stream:
            for raw in stream:
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                try:
                    ev = json.loads(line[5:].strip())
                except ValueError:
                    continue
                name = ev.get("event", "")
                if name == "tool.started":
                    mark(token, "first_tool")
                elif name == "message.delta":
                    mark(token, "first_text")
                with lock:
                    j = jobs.setdefault(token, {})
                    if name == "tool.started":
                        k = tool_kind(ev.get("tool"))
                        if k == "think" and j.get("state") == "working":
                            k = j.get("kind", "think")      # a generic tool keeps the current pose
                        if j.get("helpers", 0) > 0:
                            k = "delegate"
                        j.update(state="working", kind=k, tool=LABEL.get(ev.get("tool"), ""), a="")
                    elif name == "subagent.start":
                        j.update(state="working", kind="delegate", helpers=j.get("helpers", 0) + 1, a="")
                    elif name == "subagent.complete":
                        j["helpers"] = max(0, j.get("helpers", 0) - 1)
                    elif name == "message.delta":
                        if j.get("state") != "approval":
                            j["state"] = "replying"
                        j["a"] = j.get("a", "") + (ev.get("delta") or "")
                    elif name == "approval.request":
                        j.update(state="approval", approval={"command": ev.get("command", ""),
                                                             "description": ev.get("description", "")})
                    elif name == "approval.responded":
                        j.update(state="working", approval=None)
                    elif name.startswith("run.") and name not in ("run.steered", "run.stopping"):
                        final = ev
        if final is None:   # stream closed without a terminal event: read the pollable status
            _, final = hermes_call("GET", f"{RUNS}/{run_id}")
            final = {"event": "run." + str(final.get("status", "failed")), **final}
    except Exception as e:
        final = {"event": "run.failed", "error": str(e)[:200]}
    name = final.get("event", "run.failed")
    _, status = hermes_call("GET", f"{RUNS}/{run_id}")
    with lock:
        if status.get("session_id"):
            devices[token]["session"] = status["session_id"]
        j = jobs.setdefault(token, {})
        if name == "run.completed":
            a = plain(final.get("output") or j.get("a") or "(no reply)")
            j.update(state="done", a=a, approval=None)
        elif name in ("run.cancelled", "run.interrupted"):
            a = plain(j.get("a") or "") or "Stopped."
            j.update(state="done", a=a, approval=None)
        else:
            a = "That one failed: " + str(final.get("error") or "unknown error")[:200]
            j.update(state="error", a=a, approval=None)
        j["t_end"] = time.time()
        j.setdefault("tm", {})["done"] = round(j["t_end"] - j.get("t", j["t_end"]), 2)
        devices[token].setdefault("history", []).append([q, a, int(time.time())])
        devices[token]["history"] = devices[token]["history"][-20:]
        save(devices)
        key, said = "%.3f" % j.get("t", 0), j.get("a", "")
    if name == "run.completed":   # voice it now, so the 3DS fetch is instant
        threading.Thread(target=_prewarm, args=(token, key, said), daemon=True).start()

def voice_job(token, key, text):
    """Start (once per reply) voicing the reply chunk by chunk; returns the shared entry."""
    with speech_lock:
        e = speech_cache.get(token)
        if e and e["key"] == key:
            return e
        parts = chunks(text)
        e = {"key": key, "texts": parts, "parts": [None] * len(parts), "failed": False, "cv": threading.Condition()}
        speech_cache[token] = e
    threading.Thread(target=_voice_run, args=(token, e), daemon=True).start()
    return e

def _voice_run(token, e):
    for i, t in enumerate(e["texts"]):
        pcm = speak_raw(t)
        with e["cv"]:
            if pcm is None:
                e["failed"] = True; e["cv"].notify_all(); return
            e["parts"][i] = pcm
            e["cv"].notify_all()
        if i == 0:
            mark(token, "voice1")
    mark(token, "voice")

def _prewarm(token, key, text):
    e = voice_job(token, key, text)
    deadline = time.time() + 60
    with e["cv"]:
        while time.time() < deadline and not e["failed"] and any(x is None for x in e["parts"]):
            e["cv"].wait(0.5)


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

def lan_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
            u.connect(("10.255.255.255", 1))
            return u.getsockname()[0]
    except OSError:
        return "this-computer's-ip"

def local(method, path):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=b"" if method == "POST" else None, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except OSError:
        raise SystemExit(f"The relay isn't running on port {PORT}. Start it with: python3 relay.py")

def probe(name, url, method="GET", body=None, headers=None):
    try:
        req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
        with urllib.request.urlopen(req, timeout=10) as r:
            print(f"  ok    {name}  ({r.status})")
            return True
    except urllib.error.HTTPError as e:
        print(f"  FAIL  {name}  HTTP {e.code} at {url}")
    except OSError as e:
        print(f"  FAIL  {name}  can't connect to {url} ({getattr(e, 'reason', e)})")
    return False

def doctor():
    print(f"config: {os.path.join(DIR, 'config.json')}" + ("" if os.path.exists(os.path.join(DIR, "config.json")) else " (not found, using defaults)"))
    try:
        k = api_key()
        probe("Hermes API server", HERMES_BASE + "/v1/models", headers={"Authorization": f"Bearer {k}"})
    except SystemExit as e:
        print("  FAIL  Hermes API key:", e)
    probe("speech-to-text (required for push to talk)", WHISPER.split("/v1/")[0] + "/v1/models")
    if VOICE_URL:
        body = json.dumps({"model": CONF["voice_model"], "voice": VOICE_NAME, "input": "Hello.", "response_format": "wav"}).encode()
        probe("voice (optional talk back)", VOICE_URL, "POST", body,
              {"Authorization": f"Bearer {CONF['voice_key']}", "Content-Type": "application/json"})
    else:
        print("  off   voice (voice_url is empty)")
    try:
        code, _ = local("GET", "/devices")
        print(f"  ok    relay running on port {PORT}. On the 3DS enter: {lan_ip()}:{PORT}")
    except SystemExit:
        print(f"  --    relay not running (python3 relay.py). On the 3DS you'll enter: {lan_ip()}:{PORT}")

# ---------- "let me take a look": spoken the moment a photo turn starts, rendered once ----------
LOOK_CUE = "Let me take a look."
cue_cache = {}

def cue_pcm(who="hermes"):
    if "pcm" not in cue_cache:
        pcm = speak_raw(LOOK_CUE)
        if not pcm:
            return None          # voice server down: try again next time
        cue_cache["pcm"] = pcm
    return cue_cache["pcm"]

def _warm_cues():
    try:
        cue_pcm()
    except Exception:
        pass


if __name__ == "__main__":
    threading.Thread(target=_warm_cues, daemon=True).start()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd == "pair":
        if len(sys.argv) < 3:
            raise SystemExit("usage: python3 relay.py pair CODE [device name]")
        name = urllib.parse.quote(" ".join(sys.argv[3:]) or "3DS")
        code, out = local("POST", f"/approve?code={urllib.parse.quote(sys.argv[2].upper())}&name={name}")
        print("Paired. The 3DS connects within a few seconds." if code == 200 else f"No 3DS is waiting with that code ({out}).")
        sys.exit(0 if code == 200 else 1)
    if cmd == "devices":
        print(local("GET", "/devices")[1])
        sys.exit(0)
    if cmd == "doctor":
        doctor()
        sys.exit(0)
    if cmd != "run":
        raise SystemExit(__doc__)
    KEY = api_key()
    os.makedirs(DIR, exist_ok=True)
    os.chmod(DIR, 0o700)
    print(f"Hermes 3DS relay on port {PORT}. On the 3DS enter: {lan_ip()}:{PORT}", flush=True)
    S(("0.0.0.0", PORT), H).serve_forever()
