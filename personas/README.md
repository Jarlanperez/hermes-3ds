# Personas

A persona is someone else answering on your 3DS: their own personality, voice and conversation, with the same
Hermes, tools and memory underneath. On the 3DS, D-pad left and right switch between Hermes and every persona.
Switching starts a fresh conversation with them.

Each folder here is one persona. The relay reads them when it starts, so restart the relay after adding or
editing one. Folders starting with `_` are ignored; `_template` is the starting point and `captain` is a small
working example.

## Make one in a few minutes (no art needed)

1. Copy `_template` to a new folder. The folder name is the persona's id: lowercase letters, numbers, `-` or
   `_`, up to 23 characters.
2. Edit `persona.md`: set `display` (the name on screen) and `greet` (the toast when you switch to them), then
   describe how they talk.
3. Pick a voice in `voice.json`, or delete the file to use the relay's default voice.
4. Restart the relay, then restart the app on the 3DS. Press right on the D-pad.

Without art, a persona uses the Hermes Girl sprites with its own name in the top-left label.

```
personas/<id>/
  persona.md        front matter (display, greet) + the personality, in plain sentences
  knowledge/*.md    optional fact sheets, added in filename order
  voice.json        optional: which voice speaks the replies
```

### persona.md

Front matter keys, all optional:

- `display`: the name the app shows, up to 23 characters. Default: the folder name.
- `greet`: the toast when you switch to them. Default: "<display> is in."
- `sprite_label`: what the reply box sits beside, for the model's sense of the screen. Default: "the Hermes
  Girl sprite".

The body is the personality. Line breaks inside a paragraph don't matter; blank lines separate paragraphs. What
the agent gets is the app's own rules (short replies, plain text, the small screen), then this body, then every
`knowledge/*.md`. All of it is sent with every message, so keep it tight.

### voice.json

```json
{ "voice": "bm_george", "look_cue": "Let me have a look, sailor." }
```

- `voice`: a voice name your TTS server knows. Kokoro has many (`af_heart`, `af_bella`, `am_adam`,
  `bm_george`, ...).
- `url`, `model`: a different OpenAI-compatible `/v1/audio/speech` server for this persona only.
- `look_cue`: the line spoken while a photo is being studied. Default: "Let me take a look."

## Give them their own art (optional)

Art lives on the SD card as a pack built from 400×240 PNGs, drawn in screen position on a transparent
background (the figure sits on the right, the text runs to its left).

```
personas/<id>/sprites/stills/<pose>.png    hello listen think code reply asleep attention error battery
personas/<id>/sprites/frames/<role>.png    optional animation: <pose>_blink, reply_talk, reply_closed,
                                           code_type, code_wipe_a, code_wipe_b, asleep_snore, battery_chew
personas/<id>/screen.txt                   optional screen copy, see below
```

Only `hello.png` is required; a missing pose borrows another one. A frame only needs to differ from its still
where something moves: the builder keeps just the changed pixels, so the figure never jitters. Leave a frame out
and that motion doesn't play.

Build the pack (needs Docker for `tex3ds`, plus Python with Pillow and NumPy) and copy it to the card:

```
python3 app/tools/build_persona_pack.py personas/<id> /Volumes/<SD>/3ds/hermes/personas/<id>
```

The app matches the pack to the relay persona by folder name. A pack with no matching relay persona still shows
up, but answers as Hermes.

### screen.txt

`key=value` lines. Keys: `name`, `label`, `label_w` (px), `greet`, `head.<mode>` (up to 3 lines split by `|`),
`sub.<mode>` (`{t}` = time on this turn), `pose.<mode>`. Modes: boot offline pair hello ready asleep listen
think coding research making delegate approval reply done error looking battery. Anything left out uses the
Hermes default.

```
head.hello=AHOY,|SAILOR
sub.think=Plotting a course · {t}
pose.research=think
```
