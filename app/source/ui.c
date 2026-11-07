// Drawing primitives: romfs bitmap fonts at their baked size (1 font px = 1 screen px), tracked text,
// rectangles, and the word-wrap that lets text stop beside Hermes Girl's silhouette.
#include "app.h"
#include <3ds/util/utf.h>

Font fonts[F_COUNT];
C2D_TextBuf tb;
u32 PAPER, INK, DIM, FIELD, PAPERDIM;

static const char* FONT_FILES[F_COUNT] = {
    "romfs:/fonts/head.bcfnt", "romfs:/fonts/big.bcfnt", "romfs:/fonts/btn.bcfnt", "romfs:/fonts/body.bcfnt",
    "romfs:/fonts/bodyb.bcfnt", "romfs:/fonts/small.bcfnt", "romfs:/fonts/label.bcfnt", "romfs:/fonts/mono.bcfnt",
};

bool ui_init(void) {
    PAPER    = C2D_Color32(0xF2, 0xEF, 0xE8, 0xFF);
    INK      = C2D_Color32(0x1A, 0x1A, 0x1A, 0xFF);
    DIM      = C2D_Color32(0x58, 0x5E, 0x56, 0xFF);
    FIELD    = C2D_Color32(0x14, 0x14, 0x14, 0xFF);
    PAPERDIM = C2D_Color32(0xB8, 0xB5, 0xAE, 0xFF);
    tb = C2D_TextBufNew(12000);   // the top screen is drawn twice in 3D
    bool ok = true;
    for (int i = 0; i < F_COUNT; i++) {
        fonts[i].f = C2D_FontLoad(FONT_FILES[i]);
        if (!fonts[i].f) { ok = false; continue; }
        C2D_FontSetFilter(fonts[i].f, GPU_NEAREST, GPU_NEAREST);
        FINF_s* fi = C2D_FontGetInfo(fonts[i].f);
        // citro2d scales every font so its cell height is 30 units; undo that to draw at the baked size
        fonts[i].s = fi->tglp->cellHeight / 30.0f;
        fonts[i].k = 1.0f;
        fonts[i].lf = fi->lineFeed;
    }
    return ok;
}

void ui_exit(void) {
    for (int i = 0; i < F_COUNT; i++) if (fonts[i].f) C2D_FontFree(fonts[i].f);
    C2D_TextBufDelete(tb);
}

// next UTF-8 code point; returns bytes consumed (>= 1)
static int cp_at(const char* s, int n, u32* cp) {
    if (n <= 0) { *cp = 0; return 1; }
    ssize_t k = decode_utf8(cp, (const uint8_t*)s);
    if (k < 1 || k > n) { *cp = '?'; return 1; }
    return (int)k;
}

static float cw(int f, u32 cp) {
    if (!fonts[f].f) return 6;
    int gi = C2D_FontGlyphIndexFromCodePoint(fonts[f].f, cp);
    charWidthInfo_s* w = C2D_FontGetCharWidthInfo(fonts[f].f, gi);
    return w ? w->charWidth * fonts[f].k : 6;
}

float tw(int f, const char* s, int n, float track) {
    if (n < 0) n = strlen(s);
    float w = 0;
    int count = 0;
    for (int i = 0; i < n;) {
        u32 cp;
        i += cp_at(s + i, n - i, &cp);
        if (cp == '\n') break;
        w += cw(f, cp);
        count++;
    }
    return w + (count > 1 ? track * (count - 1) : 0);
}

static void draw_run(int f, const char* s, float x, float y, u32 col) {
    C2D_Text t;
    if (fonts[f].f) C2D_TextFontParse(&t, fonts[f].f, tb, s);
    else            C2D_TextParse(&t, tb, s);
    float sc = fonts[f].f ? fonts[f].s : 0.5f;
    C2D_DrawText(&t, C2D_WithColor, (int)x, (int)y, 0.5f, sc, sc, col);
}

void dt(int f, const char* s, int n, float x, float y, u32 col, float track) {
    if (n < 0) n = strlen(s);
    if (n <= 0) return;
    if (track == 0) {
        char buf[1024];
        if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
        memcpy(buf, s, n);
        buf[n] = 0;
        draw_run(f, buf, x, y, col);
        return;
    }
    for (int i = 0; i < n;) {           // tracked caps: one glyph at a time
        u32 cp;
        int k = cp_at(s + i, n - i, &cp);
        char g[8] = {0};
        memcpy(g, s + i, k < 7 ? k : 7);
        if (cp != ' ') draw_run(f, g, x, y, col);
        x += cw(f, cp) + track;
        i += k;
    }
}

void dtc(int f, const char* s, float cx, float y, u32 col, float track) {
    dt(f, s, -1, (int)(cx - tw(f, s, -1, track) / 2), y, col, track);
}

void dtr(int f, const char* s, float rx, float y, u32 col, float track) {
    dt(f, s, -1, (int)(rx - tw(f, s, -1, track)), y, col, track);
}

void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }

void box(float x, float y, float w, float h, float t, u32 c) {
    rect(x, y, w, t, c); rect(x, y + h - t, w, t, c); rect(x, y, t, h, c); rect(x + w - t, y, t, h, c);
}

void ellipsize(int f, const char* s, float maxw, char* out, size_t cap) {
    snprintf(out, cap, "%s", s);
    char* nl = strchr(out, '\n');
    if (nl) *nl = 0;
    if (tw(f, out, -1, 0) <= maxw) return;
    const float ell = tw(f, "\xE2\x80\xA6", -1, 0);
    int n = strlen(out);
    while (n > 0 && tw(f, out, n, 0) + ell > maxw) {
        n--;
        while (n > 0 && ((u8)out[n] & 0xC0) == 0x80) n--;   // stay on a code point boundary
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    snprintf(out + n, cap - n, "\xE2\x80\xA6");
}

static int right_edge(const unsigned short* edge, int y, int lh, int xr) {
    if (!edge) return xr;
    int r = xr;
    for (int row = y; row < y + lh && row < 240; row++) {
        if (row < 0) continue;
        int e = edge[row] - 8;
        if (e < r) r = e;
    }
    return r;
}

int layout(int f, const char* s, const unsigned short* edge, int x0, int xr, int y0, int y1,
           int lh, int pgap, Line* out, int cap) {
    int n = strlen(s), cnt = 0, page = 0, y = y0;
    int i = 0;
    const float space = cw(f, ' ');
    while (i < n && cnt < cap) {
        if (s[i] == '\n') {                       // blank line = paragraph gap
            int nls = 0;
            while (i < n && s[i] == '\n') { nls++; i++; }
            if (nls > 1 && y != y0) y += pgap;
            continue;
        }
        if ((u8)s[i] == 0x1e) {                    // ASCII art line: one row, never wrapped, mono line height
            const int alh = fonts[F_MONO].lf ? fonts[F_MONO].lf : lh;
            if (y + alh > y1) { page++; y = y0; }
            int start = i + 1, end = start;
            while (end < n && s[end] != '\n') end++;
            out[cnt].off = start; out[cnt].len = end - start; out[cnt].y = y; out[cnt].page = page; out[cnt].art = 1;
            cnt++;
            y += alh;
            i = end;
            if (i < n) i++;                        // the newline ending the art row
            if (i < n && s[i] == '\n') { y += pgap; while (i < n && s[i] == '\n') i++; }
            continue;
        }
        if (y + lh > y1) { page++; y = y0; }
        int maxw = right_edge(edge, y, lh, xr) - x0;
        if (maxw < 40) maxw = 40;
        int start = i, end = i, lastFit = -1;
        float w = 0;
        while (i < n && s[i] != '\n') {
            int ws = i;                            // measure the next word
            float ww = 0;
            while (i < n && s[i] != ' ' && s[i] != '\n') {
                u32 cp;
                int k = cp_at(s + i, n - i, &cp);
                ww += cw(f, cp);
                i += k;
            }
            float add = (end > start ? space : 0) + ww;
            if (w + add <= maxw) { w += add; end = i; lastFit = i; }
            else if (end == start) {               // a single word wider than the line: hard split
                i = ws;
                float hw = 0;
                while (i < n && s[i] != ' ' && s[i] != '\n') {
                    u32 cp;
                    int k = cp_at(s + i, n - i, &cp);
                    if (hw + cw(f, cp) > maxw && i > ws) break;
                    hw += cw(f, cp);
                    i += k;
                }
                end = i;
                break;
            } else { i = ws; break; }
            while (i < n && s[i] == ' ') i++;
        }
        (void)lastFit;
        out[cnt].off = start;
        out[cnt].len = end - start;
        out[cnt].y = y;
        out[cnt].page = page;
        out[cnt].art = 0;
        cnt++;
        y += lh;
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] == '\n') {               // a single newline ends the line; more = paragraph
            int j = i, nls = 0;
            while (j < n && s[j] == '\n') { nls++; j++; }
            i = j;
            if (nls > 1) y += pgap;
        }
    }
    return cnt;
}
