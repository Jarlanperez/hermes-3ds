# Setup

About 20 minutes. Tested with the relay on macOS (Apple Silicon). The relay is plain Python, so Linux and Windows should work, but they haven't been tested yet.

## What you need

- A Nintendo 3DS, 2DS, New 3DS or New 2DS with custom firmware ([3ds.hacks.guide](https://3ds.hacks.guide)). FBI is needed for the Home Menu version.
- [Hermes Agent](https://github.com/NousResearch/hermes-agent) on a computer on the same Wi-Fi.
- Python 3.9 or newer on that computer.

## 1. Turn on the Hermes API server

Add these to `~/.hermes/.env`:

```
API_SERVER_ENABLED=true
API_SERVER_KEY=<a long random string>
```

Restart Hermes (`hermes gateway`). You should see `API server listening on http://127.0.0.1:8642`.

## 2. Speech to text (needed for push to talk)

The relay sends your voice to any OpenAI-compatible `/v1/audio/transcriptions` server. The simplest is the small Whisper server in this repo:

```
pip install faster-whisper
python3 relay/whisper_server.py
```

It listens on `127.0.0.1:8178`, which is the relay's default. The first run downloads the `base` model (about 150 MB). For better accuracy at a small speed cost, start it with `WHISPER_MODEL=small`.

Without speech to text you can still type with Y.

## 3. Voice (optional)

For spoken replies, run any OpenAI-compatible `/v1/audio/speech` server that can return WAV. Two that work:

- [Kokoro-FastAPI](https://github.com/remsky/Kokoro-FastAPI): defaults match the relay (`http://127.0.0.1:8880`, model `kokoro`, voice `af_heart`).
- [VoiceStudio](https://github.com/debpalash/VoiceStudio): set `voice_url` to its address and `voice_model` to `mlx-audio`.

No voice server? Set `"voice_url": ""` and the app stays text only.

## 4. Configure and run the relay

Copy `relay/config.example.json` to `~/.hermes-3ds/config.json` and edit it. Every key is optional:

| Key | What it's for |
|---|---|
| `name` | Your name. The 3DS greets you with HEY NAME, and Hermes knows whose 3DS it is |
| `say_name_as` | How the voice should say your name, if it gets it wrong (`"Shiv-awn"` for Siobhan, or Kokoro IPA like `"[Siobhan](/ʃɪˈvɔːn/)"`) |
| `port` | Relay port, default 8766 |
| `hermes_url` | Hermes API server, default `http://127.0.0.1:8642` |
| `api_key` | Leave empty to read `API_SERVER_KEY` from `~/.hermes/.env` |
| `stt_url`, `stt_model` | Speech to text server |
| `voice_url`, `voice_model`, `voice_name`, `voice_key` | Voice server. Empty `voice_url` turns voice off |

Check everything, then start it:

```
python3 relay/relay.py doctor
python3 relay/relay.py
```

It prints the address to type on the 3DS, for example `192.168.1.20:8766`.

To keep it running after a restart: on macOS, `relay/install-macos.sh` installs it as a LaunchAgent. On Linux, run it as a systemd user service.

## 5. Install the 3DS app

From [Releases](../../releases):

- **Home Menu:** copy `hermes-3ds.cia` to the SD card, then open FBI → SD → the file → **Install CIA**.
- **Homebrew Launcher:** copy `hermes-3ds.3dsx` to `sd:/3ds/`.

Optional: unzip `pip-sd-pack.zip` from the same Releases page onto the root of the SD card for Pip's art. Personas are covered in [personas/README.md](../personas/README.md).

## 6. Pair

1. Open the app. The first time, it asks for the relay address. Type what the relay printed.
2. The top screen shows a 4-letter code.
3. On the computer, run `python3 relay/relay.py pair CODE`. Or, if you installed the skill, tell Hermes: "Pair my 3DS, the code is CODE."

The 3DS connects within a few seconds and greets you.

Settings live on the SD card in `sd:/3ds/hermes/`: `server.txt` (relay address), `token.txt` (pairing), `voice.txt` (voice on or off). Delete `token.txt` to pair again.

## Troubleshooting

| You see | Try |
|---|---|
| LOST SIGNAL, "Can't reach the relay" | Relay not running, wrong address, or the 3DS is on a different network. Press Y to retype the address. Check the computer's firewall allows the port |
| "Hermes didn't take it (HTTP 401)" | The API key doesn't match. Run `relay.py doctor` |
| "Couldn't transcribe that" | Speech to text server not running. Run `relay.py doctor` |
| Replies show but no voice | Voice server down, `voice_url` empty, or voice turned off with SELECT |
| The code expired | Codes last 10 minutes. Close and reopen the app for a new one |
| Wrong words heard | Speak closer to the mic, or use a larger Whisper model |
