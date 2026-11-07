#!/usr/bin/env python3
"""Local speech to text for the Hermes 3DS relay: an OpenAI-compatible /v1/audio/transcriptions
endpoint backed by faster-whisper. Audio never leaves this computer.

    pip install faster-whisper
    python3 whisper_server.py            # 127.0.0.1:8178, model "base"
    WHISPER_MODEL=small python3 whisper_server.py
"""
import json
import os
import re
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from faster_whisper import WhisperModel

PORT = int(os.environ.get("WHISPER_PORT", "8178"))
MODEL = os.environ.get("WHISPER_MODEL", "base")
model = WhisperModel(MODEL, device="cpu", compute_type="int8")


def multipart_file(body, ctype):
    """The 'file' part of a multipart/form-data body (stdlib cgi is gone in Python 3.13)."""
    m = re.search(r'boundary="?([^";]+)"?', ctype or "")
    if not m:
        return None, None
    for part in body.split(b"--" + m.group(1).encode()):
        head, _, data = part.partition(b"\r\n\r\n")
        if b'name="file"' in head:
            fn = re.search(rb'filename="([^"]*)"', head)
            return (fn.group(1).decode() if fn else "audio.wav"), data.rstrip(b"\r\n-")
    return None, None


class Handler(BaseHTTPRequestHandler):
    def _json(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.rstrip("/") in ("", "/health", "/v1/models"):
            return self._json(200, {"ok": True, "model": MODEL})
        self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self.path.rstrip("/").endswith("/audio/transcriptions"):
            return self._json(404, {"error": "not found"})
        n = int(self.headers.get("content-length", "0") or 0)
        name, audio = multipart_file(self.rfile.read(n), self.headers.get("content-type"))
        if audio is None:
            return self._json(400, {"error": {"message": "missing file"}})
        suffix = os.path.splitext(name)[1] or ".wav"
        with tempfile.NamedTemporaryFile(suffix=suffix) as tmp:
            tmp.write(audio)
            tmp.flush()
            try:
                segments, info = model.transcribe(tmp.name, vad_filter=True)
                text = " ".join(s.text.strip() for s in segments).strip()
            except Exception as e:  # bad or empty audio
                return self._json(400, {"error": {"message": str(e)}})
        self._json(200, {"text": text, "language": info.language})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print(f"whisper on 127.0.0.1:{PORT} (model {MODEL})", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
