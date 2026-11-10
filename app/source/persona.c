// Personas: who answers. Hermes is persona 0 and always there. Two sources add more:
//   - the relay: every folder in its personas/ dir (personality + voice). GET /app/v2/personas after pairing.
//     No art needed: these use the Hermes Girl sprites with their own name on the label.
//   - the SD card: sdmc:/3ds/hermes/personas/<id>/ art packs (tools/build_persona_pack.py). A pack whose id
//     matches a relay persona gives it its own sprites, screen copy and animation.
// Only the active persona's sprite sheets are in memory; switching frees the old ones one frame later (the GPU
// may still be drawing them), see persona_gc().
#include "persona.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define PDIR DIRP "/personas"
#define WHO_MAX 16

typedef struct { char id[24], name[24], label[24], greet[48]; int pack; } Who;   // pack: index into packs, -1 = Hermes art

static Pack packs[PK_MAX];
static int npacks;
static Who who[WHO_MAX];
static int nwho;
Pack* PK = NULL;
volatile int persona = 0;
static C2D_SpriteSheet grave[PK_SHEETS];          // sheets to free after the next C3D_FrameBegin
static char want[32] = "";                         // persona.txt: the last one used, restored once it shows up
static char relayList[1024];
static volatile bool relayReady = false, merged = false;

static const char* POSE_NAME[PO_COUNT] = {"hello", "listen", "think", "code", "reply", "asleep", "attention",
                                          "error", "battery"};
static const char* FRAME_NAME[FR_COUNT] = {"hello_blink", "listen_blink", "think_blink", "code_blink", "reply_blink",
                                           "asleep_blink", "attention_blink", "error_blink", "battery_blink",
                                           "reply_talk", "reply_closed", "code_type", "code_wipe_a", "code_wipe_b",
                                           "asleep_snore", "battery_chew"};
static const char* MODE_NAME[M_COUNT] = {"boot", "offline", "pair", "hello", "ready", "asleep", "listen", "think",
                                         "coding", "research", "making", "delegate", "approval", "reply", "done",
                                         "error", "looking", "battery"};
static const signed char DEF_POSE[M_COUNT] = {PO_HELLO, PO_ERROR, PO_HELLO, PO_HELLO, PO_HELLO, PO_ASLEEP, PO_LISTEN,
                                              PO_THINK, PO_CODE, PO_THINK, PO_THINK, PO_THINK, PO_ATTENTION, PO_REPLY,
                                              PO_REPLY, PO_ERROR, PO_THINK, PO_BATTERY};

static int find(const char* const* names, int n, const char* s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}

static char* keep(Pack* p, const char* s) {
    size_t n = strlen(s) + 1;
    if (p->used + n > sizeof p->arena) return NULL;
    char* d = p->arena + p->used;
    memcpy(d, s, n);
    p->used += n;
    return d;
}

static void upper(char* out, size_t n, const char* s) {
    size_t i = 0;
    for (; s[i] && i < n - 1 && i < 14; i++) out[i] = toupper((unsigned char)s[i]);   // 14: room beside the clock
    out[i] = 0;
}

static bool parse(Pack* p, const char* id) {
    char path[160], line[320];
    snprintf(path, sizeof path, PDIR "/%s/pack.txt", id);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    memcpy(p->pose, DEF_POSE, sizeof p->pose);
    int ver = 0;
    unsigned stills = 0;
    bool bad = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "v")) ver = atoi(v);
        else if (!strcmp(k, "name")) snprintf(p->name, sizeof p->name, "%s", v);
        else if (!strcmp(k, "label")) snprintf(p->label, sizeof p->label, "%s", v);
        else if (!strcmp(k, "greet")) snprintf(p->greet, sizeof p->greet, "%s", v);
        else if (!strcmp(k, "sheets")) p->nsheets = atoi(v);
        else if (!strcmp(k, "label_w")) p->label_w = atoi(v);
        else if (!strcmp(k, "still")) {
            char n[24]; int s, i, x, y, t;
            if (sscanf(v, "%23s %d %d %d %d %d", n, &s, &i, &x, &y, &t) != 6) { bad = true; continue; }
            int q = find(POSE_NAME, PO_COUNT, n);
            if (q < 0) continue;
            p->st[q] = (PStill){(unsigned char)s, (unsigned char)i, (short)x, (short)y, (short)t};
            stills |= 1u << q;
            if (s >= PK_SHEETS) bad = true;
        } else if (!strcmp(k, "frame")) {
            char n[24]; int s, i, x, y;
            if (sscanf(v, "%23s %d %d %d %d", n, &s, &i, &x, &y) != 5) { bad = true; continue; }
            int q = find(FRAME_NAME, FR_COUNT, n);
            if (q < 0 || s >= PK_SHEETS) continue;
            p->fr[q] = (PFrame){(unsigned char)s, (unsigned char)i, (short)x, (short)y, true};
        } else if (!strncmp(k, "head.", 5)) {
            int m = find(MODE_NAME, M_COUNT, k + 5);
            char* s = m >= 0 ? keep(p, v) : NULL;
            for (int j = 0; s && j < 3; j++) {         // "HEY,|THERE" -> two lines
                p->head[m][j] = s;
                char* bar = strchr(s, '|');
                if (!bar) break;
                *bar = 0; s = bar + 1;
            }
        } else if (!strncmp(k, "sub.", 4)) {
            int m = find(MODE_NAME, M_COUNT, k + 4);
            if (m >= 0) p->sub[m] = keep(p, v);
        } else if (!strncmp(k, "pose.", 5)) {
            int m = find(MODE_NAME, M_COUNT, k + 5), q = find(POSE_NAME, PO_COUNT, v);
            if (m >= 0 && q >= 0) p->pose[m] = q;
        }
    }
    fclose(f);
    if (ver != 1 || bad || p->nsheets < 1 || p->nsheets > PK_SHEETS || stills != (1u << PO_COUNT) - 1) return false;
    for (int q = 0; q < PO_COUNT; q++) if (p->st[q].sheet >= p->nsheets) return false;
    for (int q = 0; q < FR_COUNT; q++) if (p->fr[q].ok && p->fr[q].sheet >= p->nsheets) p->fr[q].ok = false;
    snprintf(path, sizeof path, PDIR "/%s/edges.bin", id);
    f = fopen(path, "rb");
    size_t got = f ? fread(p->edge, 1, sizeof p->edge, f) : 0;
    if (f) fclose(f);
    if (got != sizeof p->edge) return false;
    for (int i = 0; i < p->nsheets; i++) {
        struct stat st;
        snprintf(path, sizeof path, PDIR "/%s/s%d.t3x", id, i);
        if (stat(path, &st) != 0) return false;
    }
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "%s", id);
    if (!p->label[0]) upper(p->label, sizeof p->label, p->name);
    if (!p->greet[0]) snprintf(p->greet, sizeof p->greet, "%s is in.", p->name);
    return true;
}

static int who_find(const char* id) {
    for (int i = 0; i < nwho; i++) if (!strcmp(who[i].id, id)) return i;
    return -1;
}

int persona_count(void) { return nwho; }
const char* persona_name(void) { return who[persona].id; }
const char* persona_display(void) { return who[persona].name; }
const char* persona_label(void) { return who[persona].label; }
const char* persona_greet(void) { return who[persona].greet; }

static void unload(Pack* p, bool now) {
    for (int i = 0; i < PK_SHEETS; i++) {
        if (!p->sh[i]) continue;
        if (now) C2D_SpriteSheetFree(p->sh[i]);
        else {
            if (grave[i]) C2D_SpriteSheetFree(grave[i]);   // only if two switches landed in one frame
            grave[i] = p->sh[i];
        }
        p->sh[i] = NULL;
    }
}

void persona_gc(void) {
    for (int i = 0; i < PK_SHEETS; i++) if (grave[i]) { C2D_SpriteSheetFree(grave[i]); grave[i] = NULL; }
}

void persona_set(int p) {
    p = (p % nwho + nwho) % nwho;
    if (PK) { unload(PK, false); PK = NULL; }
    if (who[p].pack >= 0) {
        Pack* k = &packs[who[p].pack];
        bool ok = true;
        for (int i = 0; i < k->nsheets && ok; i++) {
            char path[160];
            snprintf(path, sizeof path, PDIR "/%s/s%d.t3x", k->id, i);
            k->sh[i] = C2D_SpriteSheetLoad(path);
            if (!k->sh[i]) ok = false;
        }
        if (ok) PK = k;
        else { unload(k, true); say("Couldn't load that art, using Hermes"); }   // same persona, Hermes Girl art
    }
    persona = p;
    FILE* f = fopen(DIRP "/persona.txt", "w");
    if (f) { fputs(persona_name(), f); fclose(f); }
}

static int by_id(const void* a, const void* b) { return strcmp(((const Pack*)a)->id, ((const Pack*)b)->id); }

static void add_who(const char* id, const char* name, const char* greet, int pack) {
    if (nwho >= WHO_MAX) return;
    Who* w = &who[nwho++];
    memset(w, 0, sizeof *w);
    snprintf(w->id, sizeof w->id, "%s", id);
    snprintf(w->name, sizeof w->name, "%s", name && name[0] ? name : id);
    if (pack >= 0) snprintf(w->label, sizeof w->label, "%s", packs[pack].label);
    else upper(w->label, sizeof w->label, w->name);
    if (greet && greet[0]) snprintf(w->greet, sizeof w->greet, "%s", greet);
    else snprintf(w->greet, sizeof w->greet, "%s is in.", w->name);
    w->pack = pack;
}

void persona_boot(void) {
    nwho = 0;
    add_who("hermes", "Hermes", "Hermes is back.", -1);
    snprintf(who[0].label, sizeof who[0].label, "HERMES");
    mkdir(PDIR, 0777);
    DIR* d = opendir(PDIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) && npacks < PK_MAX) {
            const char* n = e->d_name;
            if (n[0] == '.' || n[0] == '_' || strlen(n) >= sizeof packs[0].id) continue;
            char id[24];
            int i = 0;
            for (; n[i]; i++) id[i] = tolower((unsigned char)n[i]);
            id[i] = 0;
            if (!strcmp(id, "hermes")) continue;
            if (parse(&packs[npacks], id)) npacks++;
        }
        closedir(d);
    }
    qsort(packs, npacks, sizeof packs[0], by_id);
    for (int i = 0; i < npacks; i++) add_who(packs[i].id, packs[i].name, packs[i].greet, i);
    FILE* f = fopen(DIRP "/persona.txt", "r");
    if (f) { if (fgets(want, sizeof want, f)) want[strcspn(want, "\r\n ")] = 0; fclose(f); }
    int t = want[0] ? who_find(want) : -1;
    if (t > 0) persona_set(t);
}

// worker thread: keep the relay's list until the UI thread merges it (once per app run)
void persona_relay(const char* list) {
    if (merged || relayReady) return;
    snprintf(relayList, sizeof relayList, "%s", list ? list : "");
    relayReady = true;
}

// UI thread, every frame: relay personas the card has no art for are added with the Hermes Girl sprites
void persona_sync(void) {
    if (!relayReady || merged) return;
    merged = true;
    char* p = relayList;
    while (*p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char* t1 = strchr(p, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1) *t1 = 0;
        if (t2) *t2 = 0;
        char id[24];
        int i = 0;
        for (; p[i] && i < (int)sizeof id - 1; i++) id[i] = tolower((unsigned char)p[i]);
        id[i] = 0;
        if (id[0] && strcmp(id, "hermes") && who_find(id) < 0) add_who(id, t1 ? t1 + 1 : id, t2 ? t2 + 1 : "", -1);
        if (!nl) break;
        p = nl + 1;
    }
    if (persona == 0 && want[0]) {
        int t = who_find(want);
        if (t > 0) persona_set(t);
    }
}

void personas_free(void) {
    persona_gc();
    for (int i = 0; i < npacks; i++) unload(&packs[i], true);
    PK = NULL;
}
