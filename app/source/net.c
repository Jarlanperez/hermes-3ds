// Network worker: pairing, relay v2 polling (/app/v2/poll, /app/v2/earlier) and the UI's requests
// (voice, typed text, approve/deny, stop). Plain-text bodies only; the relay does the JSON.
#include "app.h"
#include <turbojpeg.h>
#include <sys/stat.h>

Shared sh;
LightLock lk;
volatile int  cmd = CMD_NONE;
volatile bool photoReady = false;
volatile bool photoSending = false;
volatile bool alive = true, sending = false, asleepFlag = false;
char server[128] = "";                 // sdmc:/3ds/hermes/server.txt, set on first run
char helloName[16] = "";
char token[64], typed[512];
u8*  rec;
u32  recLen;

static void nap(int ms) { svcSleepThread((s64)ms * 1000000LL); }

bool busy_state(const char* s) {
    return !strcmp(s, "transcribing") || !strcmp(s, "thinking") || !strcmp(s, "working") ||
           !strcmp(s, "approval") || !strcmp(s, "replying");
}

void say(const char* msg) {
    LightLock_Lock(&lk);
    snprintf(sh.toast, sizeof sh.toast, "%s", msg);
    sh.toastAt = osGetTime();
    LightLock_Unlock(&lk);
}

static void read_first_line(const char* path, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char tmp[256];
    if (fgets(tmp, sizeof tmp, f)) {
        tmp[strcspn(tmp, "\r\n ")] = 0;
        if (tmp[0]) snprintf(out, n, "%s", tmp);
    }
    fclose(f);
}

// The relay address: typed once on the 3DS keyboard ("192.168.1.20:8766"), saved to server.txt.
bool ask_server(void) {
    SwkbdState kb;
    char buf[100] = "";
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof buf - 1);
    swkbdSetHintText(&kb, "Relay address, like 192.168.1.20:8766");
    const char* cur = server;
    if (!strncmp(cur, "http://", 7)) cur += 7;
    swkbdSetInitialText(&kb, cur);
    if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM || !buf[0]) return false;
    buf[strcspn(buf, " /\r\n")] = 0;
    if (!strncmp(buf, "http:", 5)) {               // pasted a full URL: keep it as is
        snprintf(server, sizeof server, "%s", buf);
    } else {
        snprintf(server, sizeof server, "http://%s%s", buf, strchr(buf, ':') ? "" : ":8766");
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIRP, 0777);
    FILE* f = fopen(DIRP "/server.txt", "w");
    if (f) { fputs(server, f); fputc('\n', f); fclose(f); }
    token[0] = 0;                                  // a new relay means pairing again
    remove(DIRP "/token.txt");
    return true;
}

static void save_token(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIRP, 0777);
    FILE* f = fopen(DIRP "/token.txt", "w");
    if (f) { fputs(token, f); fclose(f); }
}

void net_boot(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(DIRP, 0777);
    read_first_line(DIRP "/server.txt", server, sizeof server);
    read_first_line(DIRP "/token.txt", token, sizeof token);
    LightLock_Init(&lk);
    memset(&sh, 0, sizeof sh);
    snprintf(sh.state, sizeof sh.state, "idle");
}

static u32 lastSize;
static int http_req(HTTPC_RequestMethod m, const char* path, const void* body, u32 len, char** out, u64 timeout_ns) {
    char url[256];
    snprintf(url, sizeof url, "%s%s", server, path);
    httpcContext c;
    u32 status = 0;
    if (out) *out = NULL;
    if (R_FAILED(httpcOpenContext(&c, m, url, 0))) return -1;
    httpcSetKeepAlive(&c, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&c, "User-Agent", "Hermes-3DS/2.0");
    if (token[0]) httpcAddRequestHeaderField(&c, "X-HH-Token", token);
    if (body && len) {
        httpcAddRequestHeaderField(&c, "Content-Type", "application/octet-stream");
        httpcAddPostDataRaw(&c, (const u32*)body, len);
    }
    if (R_FAILED(httpcBeginRequest(&c))) { httpcCloseContext(&c); return -2; }
    if (R_FAILED(httpcGetResponseStatusCodeTimeout(&c, &status, timeout_ns))) { httpcCloseContext(&c); return -3; }
    u32 cap = 8192, size = 0, rd = 0;
    char* buf = malloc(cap + 1);
    Result r;
    do {
        if (size + 4096 > cap) { cap *= 2; buf = realloc(buf, cap + 1); }
        r = httpcDownloadData(&c, (u8*)buf + size, 4096, &rd);
        size += rd;
    } while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    buf[size] = 0;
    lastSize = size;
    httpcCloseContext(&c);
    if (out) *out = buf; else free(buf);
    return (int)status;
}
#define GET(p, o)        http_req(HTTPC_METHOD_GET, p, NULL, 0, o, 10000000000ULL)
#define POST(p, b, l, o) http_req(HTTPC_METHOD_POST, p, b, l, o, 30000000000ULL)

// cut the next '\n'-terminated field off *p into out
static void field(char** p, char* out, size_t n) {
    char* s = *p;
    char* nl = strchr(s, '\n');
    if (nl) *nl = 0;
    snprintf(out, n, "%s", s);
    *p = nl ? nl + 1 : s + strlen(s);
}

// state, job_id, kind, elapsed_s, helpers, tool_label, question, approval_command, approval_description, answer...
static void parse_poll(char* b) {
    char st[16], job[24], kind[12], el[12], hp[8], tool[48], q[400], c[600], d[300];
    char* p = b;
    field(&p, st, sizeof st); field(&p, job, sizeof job); field(&p, kind, sizeof kind);
    field(&p, el, sizeof el); field(&p, hp, sizeof hp); field(&p, tool, sizeof tool);
    field(&p, q, sizeof q); field(&p, c, sizeof c); field(&p, d, sizeof d);
    LightLock_Lock(&lk);
    snprintf(sh.state, sizeof sh.state, "%s", st);
    snprintf(sh.job, sizeof sh.job, "%s", job);
    snprintf(sh.kind, sizeof sh.kind, "%s", kind[0] ? kind : "think");
    sh.elapsed = atoi(el);
    sh.helpers = atoi(hp);
    snprintf(sh.tool, sizeof sh.tool, "%s", tool);
    snprintf(sh.q, sizeof sh.q, "%s", q);
    snprintf(sh.cmd, sizeof sh.cmd, "%s", c);
    snprintf(sh.desc, sizeof sh.desc, "%s", d);
    snprintf(sh.a, sizeof sh.a, "%s", p);
    sh.polledAt = osGetTime();
    sh.link = LINK_OK;
    LightLock_Unlock(&lk);
}

// HH:MM \t question \t answer (answer newlines arrive as \x1f)
static void fetch_earlier(void) {
    char* body = NULL;
    if (GET("/app/v2/earlier", &body) != 200 || !body) { free(body); return; }
    LightLock_Lock(&lk);
    sh.nrows = 0;
    char* p = body;
    while (*p && sh.nrows < NROWS) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char* t1 = strchr(p, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = 0; *t2 = 0;
            Row* r = &sh.rows[sh.nrows++];
            snprintf(r->t, sizeof r->t, "%s", p);
            snprintf(r->q, sizeof r->q, "%s", t1 + 1);
            snprintf(r->a, sizeof r->a, "%s", t2 + 1);
            for (char* x = r->a; *x; x++) if (*x == '\x1f') *x = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    LightLock_Unlock(&lk);
    free(body);
}

static void set_link(int l) { LightLock_Lock(&lk); sh.link = l; LightLock_Unlock(&lk); }

static bool pair_and_check(void) {
    char* body = NULL;
    if (!token[0]) {
        int c = POST("/app/pair", "p", 1, &body);
        if (c != 200 || !body) { free(body); set_link(LINK_OFFLINE); nap(3000); return false; }
        char* nl = strchr(body, '\n');
        if (nl) {
            *nl = 0;
            LightLock_Lock(&lk);
            snprintf(sh.code, sizeof sh.code, "%s", body);
            LightLock_Unlock(&lk);
            snprintf(token, sizeof token, "%s", nl + 1);
            token[strcspn(token, "\r\n ")] = 0;
        }
        free(body);
        set_link(LINK_PAIR);
    }
    int c = GET("/app/v2/poll", &body);
    if (c != 200 || !body) {
        free(body);
        LightLock_Lock(&lk);
        if (sh.link != LINK_PAIR) sh.link = LINK_OFFLINE;
        LightLock_Unlock(&lk);
        nap(3000);
        return false;
    }
    if (!strncmp(body, "pending", 7)) { free(body); set_link(LINK_PAIR); nap(2000); return false; }
    if (!strncmp(body, "unpaired", 8)) { free(body); token[0] = 0; remove(DIRP "/token.txt"); return false; }
    save_token();
    parse_poll(body);
    free(body);
    if (GET("/app/v2/hello", &body) == 200 && body) {   // the relay's configured name
        body[strcspn(body, "\r\n")] = 0;
        snprintf(helloName, sizeof helloName, "%s", body);
    }
    free(body);
    fetch_earlier();
    return true;
}

static void do_cmd(int w) {
    char* body = NULL;
    int c = 0;
    sending = true;
    switch (w) {
        case CMD_VOICE: c = POST("/app/v2/voice", rec, recLen, &body); break;
        case CMD_ASK:   c = POST("/app/v2/ask", typed, strlen(typed), &body); break;
        case CMD_ONCE:  c = POST("/app/v2/approve", "once", 4, &body); break;
        case CMD_DENY:  c = POST("/app/v2/approve", "deny", 4, &body); break;
        case CMD_STOP:  c = POST("/app/v2/stop", "stop", 4, &body); break;
        case CMD_RESET: c = POST("/app/v2/reset", "reset", 5, &body); break;
        case CMD_PHOTO: c = POST("/app/v2/photo", photo, photoLen, &body); break;
        case CMD_UNPHOTO: c = POST("/app/v2/photo", "", 0, &body); break;
    }
    bool steered = body && (!strncmp(body, "steer", 5));
    free(body);
    sending = false;
    if (c == 403) { token[0] = 0; remove(DIRP "/token.txt"); set_link(LINK_PAIR); return; }
    if (w == CMD_VOICE || w == CMD_ASK) {
        if (c == 202 && !steered) photoReady = false;   // the photo went with this question
        if (c == 202 && steered) say("Sent as a follow-up.");
        else if (c == 409) say("Still working on the last one.");
        else if (c != 202) say("Couldn't reach the relay.");
    } else if (w == CMD_RESET) {
        if (c == 200) {
            LightLock_Lock(&lk);
            snprintf(sh.state, sizeof sh.state, "idle");
            sh.a[0] = 0; sh.job[0] = 0; sh.q[0] = 0;
            LightLock_Unlock(&lk);
            say("New chat.");
        } else say(c == 409 ? "Still working. B stops it first." : "Couldn't reach the relay.");
    } else if (w == CMD_PHOTO) {
        photoReady = c == 200;
        say(c == 200 ? "Photo ready. Hold A and ask about it." : "Couldn't send the photo.");
    } else if (w == CMD_UNPHOTO) {
        photoReady = false;
        say("Photo dropped.");
    } else if (w == CMD_STOP) {
        say(c == 200 ? "Stopping..." : "Nothing to stop.");
    } else {
        say(c == 200 ? (w == CMD_ONCE ? "Approved." : "Denied.") : "That approval already closed.");
    }
}

// ---------- photo upload: JPEG on the 3DS (~15-25 KB instead of 192 KB raw), sent before any question ----------
static u8* jpg = NULL;
static unsigned long jpgLen = 0;

static bool encode_photo(void) {
    u8* rgb = malloc(400 * 240 * 3);
    if (!rgb) return false;
    const u16* px = (const u16*)photo;
    for (int i = 0; i < 400 * 240; i++) {
        u16 v = px[i];
        u8 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        rgb[i * 3] = (r << 3) | (r >> 2); rgb[i * 3 + 1] = (g << 2) | (g >> 4); rgb[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    bool ok = false;
    tjhandle h = tjInitCompress();
    if (h) {
        if (jpg) { tjFree(jpg); jpg = NULL; }
        jpgLen = 0;
        ok = tjCompress2(h, rgb, 400, 0, 240, TJPF_RGB, &jpg, &jpgLen, TJSAMP_420, 85, TJFLAG_FASTDCT) == 0 && jpgLen > 0;
        tjDestroy(h);
    }
    free(rgb);
    return ok;
}

// The worker runs this before any queued question, so a question asked while the photo is still on its way
// waits for it instead of going out alone (the old single cmd slot let the question overwrite the photo).
static void send_photo(void) {
    char* body = NULL;
    int c = encode_photo() ? POST("/app/v2/photo", jpg, (u32)jpgLen, &body)
                           : POST("/app/v2/photo", photo, photoLen, &body);
    free(body);
    photoSending = false;
    if (c == 403) { token[0] = 0; remove(DIRP "/token.txt"); set_link(LINK_PAIR); return; }
    photoReady = c == 200;
    say(c == 200 ? "Photo ready. Hold A and ask about it." : "Photo didn't send. Snap it again.");
}

void worker(void* arg) {
    (void)arg;
    bool ready = false;
    char lastJob[24] = "", lastState[16] = "";
    int fails = 0;
    u64 nextPoll = 0;
    bool firstPoll = true;              // a turn already done at launch is not spoken again
    while (alive) {
        if (!ready) { ready = pair_and_check(); continue; }
        if (photoSending) { send_photo(); nextPoll = 0; if (!token[0]) { ready = false; continue; } }
        int w = cmd;
        if (w) { cmd = CMD_NONE; do_cmd(w); nextPoll = 0; if (!token[0]) { ready = false; continue; } }
        u64 now = osGetTime();
        if (now >= nextPoll) {
            char* body = NULL;
            int c = GET("/app/v2/poll", &body);
            if (c == 200 && body && !strncmp(body, "unpaired", 8)) {
                free(body); token[0] = 0; remove(DIRP "/token.txt"); ready = false; continue;
            }
            if (c != 200 || !body) {
                free(body);
                if (++fails >= 4) set_link(LINK_OFFLINE);
                nextPoll = now + 2000;
                continue;
            }
            fails = 0;
            parse_poll(body);
            free(body);
            LightLock_Lock(&lk);
            bool b = busy_state(sh.state);
            bool newTurnDone = (strcmp(lastJob, sh.job) || strcmp(lastState, sh.state)) &&
                               (!strcmp(sh.state, "done") || !strcmp(sh.state, "error"));
            snprintf(lastJob, sizeof lastJob, "%s", sh.job);
            snprintf(lastState, sizeof lastState, "%s", sh.state);
            LightLock_Unlock(&lk);
            if (newTurnDone) fetch_earlier();
            // photo turn just started: she says a short "let me take a look" while the model studies it
            static char cueJob[24] = "";
            LightLock_Lock(&lk);
            bool cueIt = !firstPoll && !strcmp(sh.state, "thinking") && !strcmp(sh.kind, "looking") && strcmp(cueJob, sh.job);
            if (cueIt) snprintf(cueJob, sizeof cueJob, "%s", sh.job);
            LightLock_Unlock(&lk);
            if (cueIt && voiceOn && audio_ok() && !speechIn) {
                char* pcm = NULL;
                int c = http_req(HTTPC_METHOD_GET, "/app/v2/cue?kind=looking", NULL, 0, &pcm, 5000000000ULL);
                if (c == 200 && pcm && lastSize >= 2) {
                    LightLock_Lock(&lk);
                    free((void*)speechIn);
                    speechIn = (u8*)pcm; speechInLen = lastSize; speechFirst = true; speechInGen = speechGen;
                    LightLock_Unlock(&lk);
                } else free(pcm);
            }
            bool speakIt = newTurnDone && !firstPoll;
            firstPoll = false;
            if (speakIt && voiceOn && audio_ok()) {     // talk back, piece by piece: she starts on sentence one
                u32 gen = speechGen;
                for (int part = 0; part < 12 && gen == speechGen && !cmd; part++) {
                    char path[48], * pcm = NULL;
                    snprintf(path, sizeof path, "/app/v2/speech?part=%d", part);
                    int c = http_req(HTTPC_METHOD_GET, path, NULL, 0, &pcm, 35000000000ULL);
                    if (c != 200 || !pcm || lastSize < 2) { free(pcm); break; }
                    while (speechIn && gen == speechGen) nap(20);   // UI hasn't taken the last piece yet
                    LightLock_Lock(&lk);
                    free((void*)speechIn);
                    speechIn = (u8*)pcm; speechInLen = lastSize; speechFirst = part == 0; speechInGen = gen;
                    LightLock_Unlock(&lk);
                }
            }
            // fast while a turn runs (streamed reply), slow when idle, very slow when asleep
            nextPoll = now + (b ? 250 : asleepFlag ? 5000 : 2000);
        }
        nap(50);
    }
}
