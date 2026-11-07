# Build from source

## The 3DS app

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group (devkitARM, libctru, citro2d, tex3ds).
2. `cd app && make`

That gives `hermes-3ds.3dsx`. Or build with Docker, no install needed:

```
docker run --rm -v "$PWD/app":/src -w /src devkitpro/devkitarm:latest make
```

### Home Menu `.cia`

Needs [makerom](https://github.com/3DSGuy/Project_CTR) and [bannertool](https://github.com/carstene1ns/3ds-bannertool) on your PATH (or set `MAKEROM` and `BANNERTOOL`):

```
cd app && VERSION=1.0.0 sh tools/build_cia.sh
```

The title ID is `000400000F483300`. Change `APP_UNIQUE_ID` in `tools/build_cia.sh` if you ship a fork, so the two don't overwrite each other.

## Layout

```
app/source/main.c     UI loop, input, modes
app/source/net.c      pairing, polling, requests (worker thread)
app/source/screens.c  top and bottom screen drawing, state copy and poses
app/source/ui.c       fonts, text layout
app/source/audio.c    talk back playback, LED, voice toggle
app/source/camera.c   viewfinder capture thread
app/gfx/              sprite atlases (la8), built by tex3ds
app/romfs/fonts/      Inter and JetBrains Mono baked to .bcfnt
relay/relay.py        the relay (Python standard library only)
```

## Relay protocol

Plain-text bodies so the 3DS needs no JSON parser. The device token goes in `X-HH-Token`.

| Endpoint | |
|---|---|
| `POST /app/pair` | returns `CODE\nTOKEN` |
| `GET /app/v2/poll` | `state`, job id, kind, elapsed, helpers, tool label, question, approval command, approval description, then the answer; one per line |
| `GET /app/v2/earlier` | recent turns, `HH:MM \t question \t answer` |
| `GET /app/v2/hello` | the configured name |
| `GET /app/v2/speech?part=N` | reply audio, PCM16 mono 24 kHz; 204 = no more parts |
| `POST /app/v2/voice` | raw PCM16 mono 16360 Hz from the mic |
| `POST /app/v2/ask` | typed text |
| `POST /app/v2/photo` | raw 400×240 RGB565; empty body drops it |
| `POST /app/v2/approve` | `once` or `deny` |
| `POST /app/v2/stop`, `/app/v2/reset` | stop the run, start a new chat |

States: `pending unpaired idle transcribing thinking working approval replying done error`. Kinds: `think looking coding research making delegate`.
