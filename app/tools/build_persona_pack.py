#!/usr/bin/env python3
"""Build an SD-card art pack for a Hermes 3DS persona (optional: personas work without art).

    python3 tools/build_persona_pack.py <persona-folder> [out-dir]

Input (a persona folder, e.g. personas/pirate/):
    persona.md            front matter `display:` = the name the app shows
    screen.txt            optional screen copy, key=value (see personas/README.md)
    sprites/stills/<pose>.png    400x240 RGBA in screen position. Poses: hello listen think code reply asleep
                                 attention error battery. hello is required; a missing pose borrows another one.
    sprites/frames/<role>.png    optional 400x240 RGBA animation frames: <pose>_blink, reply_talk, reply_closed,
                                 code_type, code_wipe_a, code_wipe_b, asleep_snore, battery_chew

Output (copy to sdmc:/3ds/hermes/personas/<id>/):
    pack.txt   manifest: names, screen copy, sprite positions        edges.bin  text-wrap silhouette per pose
    s0.t3x..   sprite sheets (rgba8, compressed)

Each frame ships as a small opaque PATCH over its still (only changed pixels), so the figure can never jitter.
Pixels a frame makes more transparent than its still are baked as the screen background behind that pose
(paper, or the dark field on attention/error/battery). tex3ds runs in the devkitpro/devkitarm Docker image."""
import os, re, shutil, subprocess, sys, tempfile
import numpy as np
from PIL import Image

POSES = ["hello", "listen", "think", "code", "reply", "asleep", "attention", "error", "battery"]
FALLBACK = {"listen": "hello", "think": "hello", "code": "think", "reply": "hello", "asleep": "hello",
            "attention": "hello", "error": "attention", "battery": "hello"}
ROLES = [p + "_blink" for p in POSES] + ["reply_talk", "reply_closed", "code_type", "code_wipe_a", "code_wipe_b",
                                       "asleep_snore", "battery_chew"]
ROLE_POSE = {r: (r[:-6] if r.endswith("_blink") else r.split("_")[0]) for r in ROLES}
DARK = {"attention", "error", "battery"}
PAPER, FIELD, INK = (0xF2, 0xEF, 0xE8), (0x14, 0x14, 0x14), (0x1A, 0x1A, 0x1A)
SCREEN_KEYS = re.compile(r"^(name|label|label_w|greet|head\.\w+|sub\.\w+|pose\.\w+)$")
SHEET_BUDGET = 900_000          # px per atlas before we start a new sheet (1024x1024 max)


def screen_bg(dark):
    """What sits behind the figure on the top screen (chrome() in screens.c), no text."""
    bg = np.zeros((240, 400, 3), np.int32); bg[:] = PAPER
    if dark:
        bg[21:231, 9:391] = FIELD
    bg[20, 8:392] = INK; bg[231, 8:392] = INK; bg[20:232, 8] = INK; bg[20:232, 391] = INK
    return bg


def load400(path):
    im = Image.open(path).convert("RGBA")
    if im.size != (400, 240):
        sys.exit(f"{path}: must be 400x240 (got {im.size[0]}x{im.size[1]})")
    return im


def front_matter(md):
    m = re.match(r"---\n(.*?)\n---", open(md, encoding="utf-8").read(), re.S)
    return dict(l.split(":", 1) for l in m.group(1).splitlines() if ":" in l) if m else {}


def tex3ds(work, name, files):
    open(os.path.join(work, name + ".t3s"), "w").write("--atlas -f rgba8888 -z auto\n" + "\n".join(files) + "\n")
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{work}:/w", "-w", "/w", "devkitpro/devkitarm:latest",
                        "/opt/devkitpro/tools/bin/tex3ds", "-i", name + ".t3s", "-o", name + ".t3x"],
                       capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or r.stdout).strip()


def build(src, out):
    src = os.path.abspath(os.path.expanduser(src))
    pid = os.path.basename(src.rstrip("/")).lower()
    meta = front_matter(os.path.join(src, "persona.md")) if os.path.isfile(os.path.join(src, "persona.md")) else {}
    screen = {}
    sp = os.path.join(src, "screen.txt")
    if os.path.isfile(sp):
        for line in open(sp, encoding="utf-8"):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            k, _, v = line.partition("=")
            k = k.strip()
            if not SCREEN_KEYS.match(k):
                sys.exit(f"screen.txt: unknown key {k!r}")
            screen[k] = v.strip()
    name = screen.pop("name", None) or meta.get("display", "").strip() or pid.title()
    label = screen.pop("label", None) or name.upper()

    sd = os.path.join(src, "sprites", "stills")
    fd = os.path.join(src, "sprites", "frames")
    if not os.path.isfile(os.path.join(sd, "hello.png")):
        sys.exit(f"{sd}/hello.png is required")
    wroot = os.path.expanduser("~/.hermes-3ds/.packwork")    # under $HOME: Docker Desktop can't mount /var/folders
    os.makedirs(wroot, exist_ok=True)
    work = tempfile.mkdtemp(prefix="pack-", dir=wroot)
    items, still_img, pose_box, edges, notes = [], {}, {}, [], []

    for p in POSES:                                  # stills: crop, position, text stop, silhouette edge
        path = os.path.join(sd, p + ".png")
        if not os.path.isfile(path):
            continue
        im = load400(path)
        a = im.getchannel("A")
        bb = a.point(lambda v: 255 if v > 4 else 0).getbbox()
        if not bb:
            sys.exit(f"{path}: empty")
        im.crop(bb).save(os.path.join(work, f"p_{p}.png"))
        al = a.point(lambda v: 255 if v > 8 else 0)
        band = al.crop((0, 28, 400, 160)).getbbox()
        A = al.load()
        edge = [next((x for x in range(400) if A[x, y]), 400) for y in range(240)]
        still_img[p] = np.asarray(im).astype(np.int32)
        pose_box[p] = (bb, (band[0] if band else 400) - 8, edge)
        items.append(("p_" + p, (bb[2] - bb[0]) * (bb[3] - bb[1])))

    def resolve(p):
        seen = p
        while p not in still_img:
            p = FALLBACK.get(p, "hello")
        if p != seen:
            notes.append(f"{seen}: no still, uses {p}")
        return p
    pose_src = {p: resolve(p) for p in POSES}
    for p in POSES:
        edges.append(pose_box[pose_src[p]][2])

    anims = []
    pm = lambda a: np.dstack([a[:, :, :3] * a[:, :, 3:4] // 255, a[:, :, 3]])
    for role in ROLES:                               # frames: changed pixels only, made opaque
        path = os.path.join(fd, role + ".png")
        if not os.path.isfile(path):
            continue
        pose = ROLE_POSE[role]
        if pose not in still_img:
            notes.append(f"{role}: skipped, its pose {pose} has no still of its own"); continue
        b, f = still_img[pose], np.asarray(load400(path)).astype(np.int32)
        ch = np.abs(pm(f) - pm(b)).max(2) > 0
        if not ch.any():
            notes.append(f"{role}: identical to its still, skipped"); continue
        ys, xs = np.nonzero(ch)
        x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        bg = screen_bg(pose in DARK)
        al = f[:, :, 3:4] / 255.0
        over_bg = (f[:, :, :3] * al + bg * (1 - al)).round().astype(np.int32)
        o = np.zeros((240, 400, 4), np.int32)
        full, soft = ch & (f[:, :, 3] == 255), ch & (f[:, :, 3] < 255)
        o[full] = f[full]
        o[soft, :3] = over_bg[soft]; o[soft, 3] = 255
        def comp(layer, under):
            la = layer[:, :, 3:4] / 255.0
            return layer[:, :, :3] * la + under * (1 - la)
        err = np.abs(comp(f, bg) - comp(o, comp(b, bg))).max()
        assert err < 1.01, (role, err)
        Image.fromarray(o[y0:y1, x0:x1].astype(np.uint8), "RGBA").save(os.path.join(work, f"a_{role}.png"))
        anims.append((role, int(x0), int(y0)))
        items.append(("a_" + role, int((x1 - x0) * (y1 - y0))))

    # pack into as few atlases as fit; split a sheet in half if tex3ds can't place it
    groups, cur, used = [], [], 0
    for n, area in items:
        if cur and used + area > SHEET_BUDGET:
            groups.append(cur); cur, used = [], 0
        cur.append(n); used += area
    if cur:
        groups.append(cur)
    placed, sheet = {}, 0
    queue = list(groups)
    while queue:
        g = queue.pop(0)
        ok, msg = tex3ds(work, f"s{sheet}", [n + ".png" for n in g])
        if not ok:
            if len(g) == 1:
                sys.exit(f"tex3ds failed on {g[0]}: {msg}")
            half = len(g) // 2
            queue[:0] = [g[:half], g[half:]]
            continue
        for i, n in enumerate(g):
            placed[n] = (sheet, i)
        sheet += 1
    if sheet > 6:
        sys.exit(f"{sheet} sheets; the app loads at most 6")

    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if re.match(r"(s\d+\.t3x|pack\.txt|edges\.bin)$", f):
            os.remove(os.path.join(out, f))
    for i in range(sheet):
        shutil.copy(os.path.join(work, f"s{i}.t3x"), os.path.join(out, f"s{i}.t3x"))
    lines = ["v=1", f"id={pid}", f"name={name}", f"label={label}", f"sheets={sheet}"]
    lines += [f"{k}={v}" for k, v in screen.items()]
    for p in POSES:
        q = pose_src[p]
        (bb, tmax, _) = pose_box[q]
        s, i = placed["p_" + q]
        lines.append(f"still={p} {s} {i} {bb[0]} {bb[1]} {tmax}")
    for role, x, y in anims:
        s, i = placed["a_" + role]
        lines.append(f"frame={role} {s} {i} {x} {y}")
    open(os.path.join(out, "pack.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    np.array(edges, dtype="<u2").tofile(os.path.join(out, "edges.bin"))
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print(f"{pid}: {len(still_img)} stills, {len(anims)} frames, {sheet} sheets, {size // 1024} KB -> {out}")
    for n in notes:
        print("  note:", n)
    if not os.environ.get("PACK_KEEP"): shutil.rmtree(work, ignore_errors=True)
    else: print("  work:", work)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
        f"~/.hermes-3ds/packs/{os.path.basename(os.path.abspath(os.path.expanduser(src)).rstrip('/'))}")
    build(src, out)
