// Top screen (headline states, reply pages, Hermes Girl over live text) and bottom screen (push to talk
// + EARLIER, approval A/B). Geometry follows tools/device_mock.py (the Figma v3 frames at 1x).
#include "app.h"
#include "sprites.h"
#include "hg_a_t3x.h"
#include "hg_b_t3x.h"
#include "hg_c_t3x.h"
#include "hg_d_t3x.h"
#include <time.h>
#include <math.h>

static C2D_SpriteSheet sheets[HG_SHEETS];
int apprY = 121;

void sprites_load(void) {
    sheets[0] = C2D_SpriteSheetLoadFromMem(hg_a_t3x, hg_a_t3x_size);
    sheets[1] = C2D_SpriteSheetLoadFromMem(hg_b_t3x, hg_b_t3x_size);
    sheets[2] = C2D_SpriteSheetLoadFromMem(hg_c_t3x, hg_c_t3x_size);
    sheets[3] = C2D_SpriteSheetLoadFromMem(hg_d_t3x, hg_d_t3x_size);
}

void sprites_free(void) {
    for (int i = 0; i < HG_SHEETS; i++) if (sheets[i]) C2D_SpriteSheetFree(sheets[i]);
}

static void sprite(int id, int dy) {
    const SpriteRef* r = &SPRITES[id];
    if (!sheets[r->sheet]) return;
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheets[r->sheet], r->idx), r->x, r->y + dy, 0.5f, NULL, 1.0f, 1.0f);
}

// ---------- reply layout (shared with main.c for paging) ----------
#define RX0 22
#define RY0 36
#define RY1 206
#define RLH 15
int reply_layout(const char* text, Line* out, int cap) {
    return layout(F_BODY, text, EDGE[SP_TALK_OPEN], RX0, 383, RY0, RY1, RLH, 6, out, cap);
}

// how far she has to step right so the art on this page clears her silhouette (0 = no art)
int art_slide(const Line* lines, int nlines, int page, const char* text) {
    int need = 0;
    for (int i = 0; i < nlines; i++) {
        const Line* ln = &lines[i];
        if (ln->page != page || !ln->art) continue;
        int right = RX0 + (int)tw(F_MONO, text + ln->off, ln->len, 0) + 10;
        int lh = fonts[F_MONO].lf ? fonts[F_MONO].lf : 13;
        for (int row = ln->y; row < ln->y + lh && row < 240; row++) {
            int d = right - EDGE[SP_TALK_OPEN][row];
            if (d > need) need = d;
        }
    }
    return need > 170 ? 170 : need;
}

// ---------- per-mode copy and pose ----------
typedef struct { const char* l[3]; int base, blink; bool dark; } Look;

static const char* greeting(void) {
    static char g[24];
    if (helloName[0]) snprintf(g, sizeof g, "HEY %s", helloName);
    else snprintf(g, sizeof g, "HELLO");
    return g;
}

static Look look(int m) {
    switch (m) {
        case M_BOOT:     return (Look){{greeting()}, SP_HELLO, SP_HELLO_BLINK, false};
        case M_HELLO:    return (Look){{greeting()}, SP_HELLO, SP_HELLO_BLINK, false};
        case M_PAIR:     return (Look){{"PAIR CODE"}, SP_HELLO, SP_HELLO_BLINK, false};
        case M_LISTEN:   return (Look){{"LISTENING"}, SP_LISTEN, SP_LISTEN_BLINK, false};
        case M_THINK:    return (Look){{"DEEP IN", "THOUGHT"}, SP_THINK, -1, false};
        case M_CODING:   return (Look){{"LOCKED IN", "WRITING", "CODE"}, SP_CODING, SP_CODING_BLINK, false};
        case M_RESEARCH: return (Look){{"DIGGING IN"}, SP_RESEARCH, SP_RESEARCH_BLINK, false};
        case M_MAKING:   return (Look){{"MAKING", "SOMETHING"}, SP_MAKING, -1, false};
        case M_DELEGATE: return (Look){{"HELPERS", "OUT"}, SP_DELEGATE, SP_DELEGATE_BLINK, false};
        case M_DONE:     return (Look){{"DONE"}, SP_DONE, -1, false};
        case M_ASLEEP:   return (Look){{"ASLEEP"}, SP_ASLEEP, -1, false};
        case M_APPROVAL: return (Look){{"NEEDS", "ATTENTION"}, SP_ATTENTION, SP_ATTENTION_BLINK, true};
        case M_ERROR:    return (Look){{"LOST", "SIGNAL"}, SP_ERROR, -1, true};
        case M_OFFLINE:  return (Look){{"LOST", "SIGNAL"}, SP_ERROR, -1, true};
        case M_REPLY:    return (Look){{NULL}, SP_TALK_CLOSED, SP_TALK_BLINKC, false};
        case M_LOOKING:  return (Look){{"LOOKING"}, SP_LOOKING, -1, false};
        case M_BATTERY:  return (Look){{"LOW", "BATTERY"}, SP_BATTERY, -1, true};
        default:         return (Look){{"READY"}, SP_IDLE, SP_IDLE_BLINK, false};
    }
}

static void dur(int s, char* out, size_t n) {
    if (s < 0) s = 0;
    if (s < 60) snprintf(out, n, "%ds", s);
    else if (s < 3600) snprintf(out, n, "%dm %02ds", s / 60, s % 60);
    else snprintf(out, n, "%dh %02dm", s / 3600, (s / 60) % 60);
}

static void sub_line(const View* v, char* out, size_t n) {
    char t[24];
    dur(v->elapsed, t, sizeof t);
    switch (v->mode) {
        case M_BOOT:    snprintf(out, n, "Connecting to the relay..."); break;
        case M_PAIR:    snprintf(out, n, "Run: relay.py pair CODE, or tell Hermes"); break;
        case M_HELLO: case M_READY: snprintf(out, n, "Hold A to talk"); break;
        case M_LISTEN:  snprintf(out, n, "Let go to send"); break;
        case M_ASLEEP:  snprintf(out, n, "Press any button"); break;
        case M_APPROVAL: snprintf(out, n, "Approve below"); break;
        case M_LOOKING: snprintf(out, n, "Studying your photo \xC2\xB7 %s", t); break;
        case M_BATTERY: snprintf(out, n, "Plug me in"); break;
        case M_DONE:    snprintf(out, n, "Took %s", t); break;
        case M_OFFLINE: snprintf(out, n, "Can't reach the relay \xC2\xB7 Y to change it"); break;
        case M_ERROR:   snprintf(out, n, "%s", v->err && v->err[0] ? v->err : "Something broke"); break;
        case M_DELEGATE:
            if (v->helpers > 0) { snprintf(out, n, "%d running \xC2\xB7 %s", v->helpers, t); break; }
            /* fallthrough */
        default:
            if (v->tool && v->tool[0]) snprintf(out, n, "%s \xC2\xB7 %s", v->tool, t);
            else snprintf(out, n, "Working %s", t);
    }
}

static void chrome(bool dark) {
    if (dark) rect(9, 21, 382, 210, FIELD);
    box(8, 20, 384, 212, 1, INK);
    rect(320, 20, 32, 1, PAPER);                 // top rule ends under her hair, never as a stub beside the clock
    rect(8, 13, 61, 16, INK);
    dt(F_LABEL, "HERMES", -1, 14, 15, PAPER, 1.6f);
}

// 3D: px of parallax per layer at full slider. + sits behind the screen, - pops out of it.
static void depth(const View* v, float px) {
    C2D_ViewReset();
    if (v->eye != 0.0f && px != 0.0f) {
        float d = roundf(v->eye * px);              // whole pixels: a fractional shift resamples the sprite soft
        if (d != 0.0f) C2D_ViewTranslate(d, 0);
    }
}

// R: live viewfinder (or, after the snap, the photo waiting for his question), full top screen
static void draw_viewfinder(const View* v, const char* label) {
    C2D_ViewReset();
    if (cam_has_frame()) C2D_DrawImageAt(cam_image(), 0, 0, 0.5f, NULL, 1.0f, 1.0f);
    else { rect(0, 0, 400, 240, INK); dtc(F_BODY, "Waking the camera...", 200, 112, PAPER, 0); }
    depth(v, -2.0f);                              // the frame marks float just in front of the picture
    for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? 372 : 12, y = (i & 2) ? 212 : 12;
        rect(x, y, 16, 3, PAPER); rect((i & 1) ? x + 13 : x, y, 3, 16, PAPER);
    }
    float w = tw(F_LABEL, label, -1, 1.2f) + 20;
    rect(200 - w / 2, 216, w, 18, INK);
    dtc(F_LABEL, label, 200, 219, PAPER, 1.2f);
    C2D_ViewReset();
}

void draw_top(const View* v) {
    if (v->cam) { draw_viewfinder(v, "A SNAP  \xC2\xB7  B CANCEL"); return; }
    if (v->photo && !v->reader && cam_has_frame()) {   // the shot he is about to ask about
        draw_viewfinder(v, v->rec ? "LISTENING  \xC2\xB7  LET GO TO SEND" : "PHOTO READY  \xC2\xB7  HOLD A AND ASK");
        return;
    }
    Look L = look(v->mode);
    bool dark = L.dark;
    u32 fg = dark ? PAPER : INK;
    depth(v, 3.0f);                              // the frame sits back
    chrome(dark);
    depth(v, 0.0f);                              // text on the glass

    // text first; the figure is drawn over it
    if (v->mode == M_REPLY) {
        for (int i = 0; i < v->nlines; i++) {
            const Line* ln = &v->lines[i];
            if (ln->page != v->page) continue;
            int n = ln->len;
            if (ln->off >= v->reveal) break;
            if (ln->off + n > v->reveal) n = v->reveal - ln->off;
            dt(ln->art ? F_MONO : F_BODY, v->text + ln->off, n, RX0, ln->y, INK, 0);
        }
        if (v->pagelabel[0]) dt(F_LABEL, v->pagelabel, -1, RX0, 212, DIM, 1.2f);
        if (v->page < v->npages - 1 && v->reveal >= (int)strlen(v->text)) {
            float x = RX0 + tw(F_LABEL, v->pagelabel, -1, 1.2f) + 8;
            float b = (osGetTime() / 400) % 2;          // nudge the down-triangle like an RPG box
            C2D_DrawTriangle(x, 213 + b, INK, x + 8, 213 + b, INK, x + 4, 219 + b, INK, 0.5f);
        }
    } else {
        int y = 39;
        if (v->mode == M_APPROVAL || v->mode == M_ERROR) depth(v, -3.0f);   // alerts pop forward
        for (int i = 0; i < 3 && L.l[i]; i++) { dt(F_HEAD, L.l[i], -1, 30, y, fg, 0.6f); y += 32; }
        if (v->mode == M_PAIR && v->code) { dt(F_HEAD, v->code, -1, 30, y, fg, 6.0f); y += 32; }
        char sub[160], cut[160];
        sub_line(v, sub, sizeof sub);
        int base = L.base;
        int tmax = SPRITES[base].tmax;
        ellipsize(F_BODY, sub, tmax - 30, cut, sizeof cut);
        dt(F_BODY, cut, -1, 30, y + 1, fg, 0);
        depth(v, 0.0f);
    }
    depth(v, v->mode == M_ASLEEP ? 1.5f : -2.5f);   // she floats in front (sinks back when asleep)

    int id = L.base;
    if (v->mode == M_REPLY) {
        id = v->talk ? (v->blink ? SP_TALK_BLINK : SP_TALK_OPEN) : (v->blink ? SP_TALK_BLINKC : SP_TALK_CLOSED);
    } else if (v->blink && L.blink >= 0) {
        id = L.blink;
    }
    if (v->mode == M_REPLY && v->slide) {      // step aside for the art, off the right edge
        const SpriteRef* r = &SPRITES[id];
        if (sheets[r->sheet])
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheets[r->sheet], r->idx), r->x + v->slide, r->y + v->bob, 0.5f, NULL, 1.0f, 1.0f);
    } else sprite(id, v->bob);

    // the clock always sits above her hair
    depth(v, 3.0f);
    rect(352, 12, 42, 14, PAPER);
    dt(F_LABEL, v->clock, -1, 356, 14, INK, 1.2f);
    C2D_ViewReset();
}

// ---------- bottom ----------
static void header(const char* left, const char* right) {
    dt(F_LABEL, left, -1, 13, 5, INK, 1.4f);
    dtr(F_LABEL, right, 307, 5, DIM, 1.2f);
    rect(13, 19, 294, 1, INK);
}

static void footer(const char* s) {
    rect(13, 217, 294, 1, DIM);
    dtc(F_SMALL, s, 160, 224, DIM, 0);
}

static void glyph_btn(float x, float y, const char* k, u32 bg, u32 fg) {
    C2D_DrawCircleSolid(x + 8, y + 8, 0.5f, 8, bg);
    dtc(F_BODYB, k, x + 8, y + 1, fg, 0);
}

static void toast(const View* v) {
    if (!v->toast || !v->toast[0]) return;
    float w = tw(F_BODYB, v->toast, -1, 0) + 20;
    rect(160 - w / 2, 192, w, 20, INK);
    dtc(F_BODYB, v->toast, 160, 195, PAPER, 0);
}

static void draw_approval(const View* v) {
    header("APPROVAL", "CONNECTED");
    dt(F_BODYB, "Hermes wants to run:", -1, 13, 28, INK, 0);
    static Line ml[16], dl[6];
    int n = layout(F_MONO, v->cmd ? v->cmd : "", NULL, 19, 301, 0, 5 * 13, 13, 0, ml, 16);
    int shown = 0;
    while (shown < n && ml[shown].page == 0) shown++;
    int bh = 10 + shown * 13;
    if (bh < 36) bh = 36;
    box(13, 46, 294, bh, 1, INK);
    for (int i = 0; i < shown; i++) {
        bool last = i == shown - 1 && shown < n;     // more than 5 lines: end on an ellipsis
        if (last) {
            char cut[200];
            snprintf(cut, sizeof cut, "%.*s", ml[i].len, v->cmd + ml[i].off);
            char e[220];
            ellipsize(F_MONO, cut, 270, e, sizeof e);
            dt(F_MONO, e, -1, 19, 51 + ml[i].y, INK, 0);
        } else {
            dt(F_MONO, v->cmd + ml[i].off, ml[i].len, 19, 51 + ml[i].y, INK, 0);
        }
    }
    int y = 46 + bh + 6;
    if (v->desc && v->desc[0]) {
        int dn = layout(F_BODY, v->desc, NULL, 13, 307, 0, 2 * 14, 14, 0, dl, 6);
        for (int i = 0; i < dn && dl[i].page == 0; i++) {
            bool more = i + 1 < dn && dl[i + 1].page > 0;   // last row that fits, with text left over
            if (more) {
                char e[320];
                int n = dl[i].len < 300 ? dl[i].len : 300;
                const float ell = tw(F_BODY, "\xE2\x80\xA6", -1, 0);
                while (n > 0 && tw(F_BODY, v->desc + dl[i].off, n, 0) + ell > 294) {   // make room for "…"
                    n--;
                    while (n > 0 && ((u8)v->desc[dl[i].off + n] & 0xC0) == 0x80) n--;
                }
                while (n > 0 && v->desc[dl[i].off + n - 1] == ' ') n--;
                snprintf(e, sizeof e, "%.*s\xE2\x80\xA6", n, v->desc + dl[i].off);
                dt(F_BODY, e, -1, 13, y + dl[i].y, INK, 0);
            } else dt(F_BODY, v->desc + dl[i].off, dl[i].len, 13, y + dl[i].y, INK, 0);
        }
        y += (dn > 1 ? 2 : 1) * 14 + 4;
    }
    apprY = y < 121 ? 121 : y;
    int h = 211 - apprY;
    if (h > 62) h = 62;
    rect(13, apprY, 142, h, INK);
    dtc(F_BTN, "APPROVE", 84, apprY + h / 2 - 13, PAPER, 0.8f);
    dtc(F_BODYB, "A", 84, apprY + h / 2 + 6, PAPER, 0);
    box(165, apprY, 142, h, 3, INK);
    dtc(F_BTN, "DENY", 236, apprY + h / 2 - 13, INK, 0.8f);
    dtc(F_BODYB, "B", 236, apprY + h / 2 + 6, INK, 0);
    footer("A approve \xC2\xB7 B deny");
    toast(v);
}

void draw_bottom(const View* v) {
    if (v->mode == M_APPROVAL) { draw_approval(v); return; }
    const char* link = v->mode == M_OFFLINE ? "OFFLINE" : v->mode == M_PAIR ? "PAIRING" :
                       v->mode == M_BOOT ? "CONNECTING" : "CONNECTED";
    header("ACTIONS", link);

    if (v->rec) {
        rect(13, 33, 294, 90, INK);
        dtc(F_BIG, "LISTENING", 160, 60, PAPER, 1.0f);
        char m[48];
        snprintf(m, sizeof m, "let go to send \xC2\xB7 %.1fs", v->recSec);
        dtc(F_BODY, m, 160, 92, PAPER, 0);
    } else {
        bool live = v->micOK && v->mode != M_PAIR && v->mode != M_OFFLINE && v->mode != M_BOOT;
        u32 c = live ? INK : PAPERDIM;
        box(13, 33, 294, 90, 3, c);
        const char* lab = v->micOK ? (v->busy ? "FOLLOW UP" : v->photo ? "ASK ABOUT PHOTO" : "PUSH TO TALK") : "NO MIC";
        float w = tw(F_BTN, lab, -1, 0.8f) + (v->micOK ? 24 : 0);
        float x = 160 - w / 2;
        dt(F_BTN, lab, -1, x, 68, c, 0.8f);
        if (v->micOK) glyph_btn(x + w - 16, 68, "A", c, PAPER);
    }

    dt(F_LABEL, "EARLIER", -1, 13, 133, DIM, 1.2f);
    int y = 147;
    if (!v->nrows) dt(F_SMALL, "Nothing yet.", -1, 13, y + 1, DIM, 0);
    for (int i = 0; i < v->nrows && i < 3; i++) {
        char q[220];
        ellipsize(F_BODY, v->rows[i].q[0] ? v->rows[i].q : "(voice)", 250, q, sizeof q);
        dt(F_BODY, q, -1, 13, y, INK, 0);
        dtr(F_SMALL, v->rows[i].t, 307, y + 2, DIM, 0);
        rect(13, y + 18, 294, 1, DIM);
        y += 21;
    }

    const char* f;
    switch (v->mode) {
        case M_PAIR: case M_OFFLINE: case M_BOOT: f = "Y relay address \xC2\xB7 START exit"; break;
        case M_LISTEN: f = "Let go of A to send"; break;
        case M_REPLY: f = v->reader ? "A talk \xC2\xB7 B close \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 page \xC2\xB7 Y type"
                                    : v->busy ? "A follow up \xC2\xB7 B skip/stop \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 page"
                                    : "A talk \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 page \xC2\xB7 Y type \xC2\xB7 X new chat"; break;
        default: f = v->busy ? "A follow up \xC2\xB7 B stop \xC2\xB7 Y type"
                             : v->photo ? "A ask about it \xC2\xB7 X drop photo \xC2\xB7 R retake"
                             : v->mode == M_DONE || v->mode == M_ERROR ? "A talk \xC2\xB7 Y type \xC2\xB7 X new chat"
                             : "A talk \xC2\xB7 Y type \xC2\xB7 R camera"; break;
    }
    if (v->cam) f = "A or R snap \xC2\xB7 B cancel";
    footer(f);
    toast(v);
}
