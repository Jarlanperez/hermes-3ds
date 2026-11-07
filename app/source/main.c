// Hermes for Nintendo 3DS.
// Push-to-talk client for the Hermes 3DS relay (relay/relay.py, /app/v2/*). Hold A,
// talk, let go: audio goes to the relay computer as raw PCM16, Whisper transcribes it, Hermes runs the turn through
// /v1/runs and the relay mirrors live state (thinking, coding, research, helpers, approvals, streamed
// reply) for this app to poll. No secrets live on the 3DS: only the device token the relay approved.
#include "app.h"
#include <malloc.h>
#include <time.h>

#define SLEEP_MS (5 * 60 * 1000)        // idle this long -> ASLEEP
#define HELLO_MS 4000
#define CAM_HOLD_MS 300                 // R held this long opens the camera; shorter is a page turn

static Line lines[MAXLINES];
static char text[A_CAP];                // the reply (or an EARLIER answer) being read

// LOW BATTERY: level 0-5 from ptm:u, checked every 10 s; only when not charging
static bool battery_low(u64 now) {
    static u64 at = 0;
    static bool low = false, init = false, ok = false;
    if (!init) { init = true; ok = R_SUCCEEDED(ptmuInit()); }
    if (!ok) return false;
    if (at && now - at < 10000) return low;
    at = now;
    u8 lvl = 5, chg = 0;
    PTMU_GetBatteryLevel(&lvl);
    PTMU_GetBatteryChargeState(&chg);
    low = lvl <= 1 && !chg;
    return low;
}

static int mode_for(const char* state, const char* kind) {
    if (!strcmp(state, "thinking") && !strcmp(kind, "looking")) return M_LOOKING;   // a photo turn, before any tool
    if (!strcmp(state, "transcribing") || !strcmp(state, "thinking")) return M_THINK;
    if (!strcmp(state, "approval")) return M_APPROVAL;
    if (!strcmp(state, "replying") || !strcmp(state, "done")) return M_REPLY;
    if (!strcmp(state, "error")) return M_ERROR;
    if (!strcmp(state, "working")) {
        if (!strcmp(kind, "coding")) return M_CODING;
        if (!strcmp(kind, "research")) return M_RESEARCH;
        if (!strcmp(kind, "making")) return M_MAKING;
        if (!strcmp(kind, "delegate")) return M_DELEGATE;
        return M_THINK;
    }
    return M_READY;
}

static bool hit(const touchPosition* t, int x, int y, int w, int h) {
    return t->px >= x && t->px < x + w && t->py >= y && t->py < y + h;
}

static void utf8_step(int* i, int len) {
    while (*i < len && ((u8)text[*i] & 0xC0) == 0x80) (*i)++;
}

static int page_end(int nlines, int page, int len) {
    for (int i = 0; i < nlines; i++) if (lines[i].page > page) return lines[i].off;
    return len;
}

static int page_start(int nlines, int page) {
    for (int i = 0; i < nlines; i++) if (lines[i].page == page) return lines[i].off;
    return 0;
}

int main(void) {
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    gfxSet3D(true);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    ui_init();
    sprites_load();
    httpcInit(0x200000);
    net_boot();
    if (!server[0]) ask_server();        // first run: where is the relay?
    audio_init();
    char ackJob[24] = "";                // LED: the finished turn he has already seen
    bool ledPrimed = false;

    u32 micSize = 0x30000;
    u8* micbuf = memalign(0x1000, micSize);
    bool micOK = micbuf && R_SUCCEEDED(micInit(micbuf, micSize));
    u32 micData = micOK ? micGetSampleDataSize() : 0, micPos = 0;
    rec = malloc(MAX_REC);
    bool recording = false;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread th = threadCreate(worker, NULL, 64 * 1024, prio - 1, -2, false);

    char seenJob[24] = "";
    int nlines = 0, page = 0, npages = 1, reveal = 0, textLen = 0;
    bool reader = false;                 // reading an EARLIER answer instead of the live turn
    bool prevTalk = false, talkOpen = false;
    bool camUp = false;                  // viewfinder showing (R held past CAM_HOLD_MS)
    u64 rAt = 0;                         // when R went down (0 = not held)
    bool aLock = false;                  // A snapped a photo: ignore it until released
    u64 start = osGetTime(), lastInput = start, blinkAt = start + 2500, talkFlip = 0, padAt = 0;
    static Shared s;
    View v;

    while (aptMainLoop() && alive) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        u64 now = osGetTime();
        if (kd & KEY_START) break;
        touchPosition tp;
        hidTouchRead(&tp);
        bool touching = kh & KEY_TOUCH;
        if (kd || touching) lastInput = now;

        // ---- snapshot shared state ----
        LightLock_Lock(&lk);
        memcpy(&s, &sh, sizeof s);
        LightLock_Unlock(&lk);
        bool busy = busy_state(s.state);
        bool asleep = !busy && !recording && now - lastInput > SLEEP_MS;
        if (asleep && kd) { lastInput = now; asleep = false; kd = 0; }   // the waking press does nothing else
        asleepFlag = asleep;

        int mode;
        if (s.link == LINK_PAIR) mode = M_PAIR;
        else if (s.link == LINK_OFFLINE) mode = M_OFFLINE;
        else if (s.link == LINK_BOOT) mode = M_BOOT;
        else if (recording) mode = M_LISTEN;
        else if (reader) mode = M_REPLY;
        else mode = mode_for(s.state, s.kind);
        // a turn with no answer text yet reads as thinking (or ready, for an old finished turn)
        if (mode == M_REPLY && !reader && !s.a[0]) mode = busy ? M_THINK : M_READY;
        if (mode == M_READY && now - start < HELLO_MS) mode = M_HELLO;
        if (mode == M_READY && asleep) mode = M_ASLEEP;
        if ((mode == M_READY || mode == M_ASLEEP) && battery_low(now)) mode = M_BATTERY;

        // ---- live reply text: new turn or streamed growth ----
        if (!reader && mode == M_REPLY) {
            bool newJob = strcmp(seenJob, s.job) != 0;
            if (newJob || strcmp(text, s.a)) {
                snprintf(text, sizeof text, "%s", s.a);
                textLen = strlen(text);
                nlines = reply_layout(text, lines, MAXLINES);
                npages = nlines ? lines[nlines - 1].page + 1 : 1;
                if (newJob) { page = 0; reveal = 0; }
                if (reveal > textLen) reveal = textLen;
                if (page >= npages) page = npages - 1;
                snprintf(seenJob, sizeof seenJob, "%s", s.job);
            }
        }

        // ---- push to talk (A or the big touch button) ----
        // ---- camera: hold R for the viewfinder, let go to snap. A quick R tap still pages a reply ----
        // The viewfinder stays up once open: frame the shot, then A (or R) snaps, B cancels.
        if (camUp) {
            cam_pump();
            if (kd & (KEY_A | KEY_R)) {
                bool ok = cam_snap();
                cam_stop();
                camUp = false;
                if (kd & KEY_A) aLock = true;              // the snapping press must not start push to talk
                if (ok) cmd = CMD_PHOTO; else say("No picture yet. Give it a second.");
            } else if (kd & KEY_B) {
                cam_stop();
                camUp = false;
                say("Camera closed.");
            }
            kd = 0;                                        // nothing else reacts while the viewfinder is up
        } else if ((kd & KEY_R) && !recording && !sending && s.link == LINK_OK && mode != M_APPROVAL) {
            // one press of R opens the viewfinder; it stays live until A/R snaps or B closes it
            if (busy) say("Still working. Snap when she's done.");
            else if (cam_start()) { camUp = true; audio_stop(); reader = false; }
            else say("Camera didn't start.");
        }
        if (aLock && !(kh & KEY_A)) aLock = false;

        bool canTalk = micOK && s.link == LINK_OK && !asleep && mode != M_APPROVAL && !camUp && !aLock;
        bool touchTalk = touching && hit(&tp, 13, 33, 294, 90);
        bool talk = canTalk && ((kh & KEY_A) || touchTalk);
        if (talk && !prevTalk && !recording && !sending) {
            audio_stop();                                  // she hushes when he talks
            recLen = 0;
            micPos = 0;
            if (R_SUCCEEDED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, micData, true))) {
                recording = true;
                reader = false;
            }
        }
        if (recording) {
            u32 readpos = micPos;
            micPos = micGetLastSampleOffset();
            while (recLen < MAX_REC && readpos != micPos) {
                rec[recLen++] = micbuf[readpos];
                readpos = (readpos + 1) % micData;
            }
            if (!talk || recLen >= MAX_REC) {
                MICU_StopSampling();
                recording = false;
                recLen &= ~1u;
                if (recLen < MIN_REC) say("Too short. Hold A while you talk.");
                else cmd = CMD_VOICE;
            }
        }
        prevTalk = talk;

        // ---- other controls ----
        if (mode == M_APPROVAL) {
            bool tapA = (kd & KEY_TOUCH) && hit(&tp, 13, apprY, 142, 62);
            bool tapB = (kd & KEY_TOUCH) && hit(&tp, 165, apprY, 142, 62);
            if (!sending && ((kd & KEY_A) || tapA)) cmd = CMD_ONCE;
            else if (!sending && ((kd & KEY_B) || tapB)) cmd = CMD_DENY;
        } else if (mode == M_REPLY) {
            if ((kd & KEY_B) && audio_playing()) audio_stop();
            if (kd & KEY_B) {
                int pe = page_end(nlines, page, textLen);
                if (reveal < pe) reveal = pe;                  // skip the type-on for this page
                else if (reader) reader = false;               // close the EARLIER answer
                else if (busy && !sending) cmd = CMD_STOP;
            }
            circlePosition cp;
            hidCircleRead(&cp);
            bool stick = now - padAt > 300;
            bool down = (kd & KEY_DDOWN) || (cp.dy < -60 && stick);
            bool up = (kd & (KEY_DUP | KEY_L)) || (cp.dy > 60 && stick);
            if (down || up) padAt = now;
            if (down && page < npages - 1) {
                page++;
                int ps = page_start(nlines, page);
                if (reveal < ps) reveal = ps;                  // previous page counts as read
            }
            if (up && page > 0) page--;
        } else if (busy && (kd & KEY_B) && !sending) {
            cmd = CMD_STOP;
        }

        // X: finish this conversation. Clears the screen back to the greeting and the next turn starts fresh.
        if ((kd & KEY_X) && photoReady && !busy && !sending && !camUp) {
            cmd = CMD_UNPHOTO;                             // first X drops a pending photo
        } else if ((kd & KEY_X) && s.link == LINK_OK && !sending && !recording && mode != M_APPROVAL && !camUp) {
            if (busy) say("Still working. B stops it first.");
            else {
                audio_stop();
                cmd = CMD_RESET;
                reader = false;
                text[0] = 0; textLen = 0; nlines = 0; page = 0; npages = 1; reveal = 0;
                seenJob[0] = 0;
                start = now;                                   // greet again
            }
        }

        // Y on the offline / pairing screens: change the relay address
        if ((kd & KEY_Y) && (mode == M_OFFLINE || mode == M_PAIR || mode == M_BOOT)) {
            if (ask_server()) say("Relay address saved. Connecting...");
        } else if ((kd & KEY_Y) && s.link == LINK_OK && !sending && mode != M_APPROVAL) {
            SwkbdState kb;
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 500);
            swkbdSetHintText(&kb, busy ? "Follow up for Hermes" : "Message Hermes");
            char buf[512] = "";
            if (swkbdInputText(&kb, buf, sizeof buf) == SWKBD_BUTTON_CONFIRM && buf[0]) {
                snprintf(typed, sizeof typed, "%s", buf);
                cmd = CMD_ASK;
                reader = false;
            }
        }

        // SELECT: voice on/off (not while L/Down are held, so the Rosalina screenshot combo doesn't flip it)
        if ((kd & KEY_SELECT) && !(kh & (KEY_L | KEY_DDOWN))) voice_toggle();

        // ---- talk back + notification LED ----
        audio_pump();
        if (!ledPrimed && s.link == LINK_OK) { snprintf(ackJob, sizeof ackJob, "%s", s.job); ledPrimed = true; }
        bool finished = !strcmp(s.state, "done") || !strcmp(s.state, "error");
        if (kd || touching) { if (finished) snprintf(ackJob, sizeof ackJob, "%s", s.job); }
        if (mode == M_APPROVAL) led_set(HL_ATTN);
        else if (ledPrimed && finished && strcmp(ackJob, s.job)) led_set(HL_DONE);
        else led_set(HL_OFF);

        // tap an EARLIER row to reread that answer on the top screen
        if ((kd & KEY_TOUCH) && mode != M_APPROVAL && !recording && tp.py >= 145 && tp.py < 145 + 21 * 3) {
            int r = (tp.py - 145) / 21;
            if (r < s.nrows && s.rows[r].a[0]) {
                reader = true;
                snprintf(text, sizeof text, "%s", s.rows[r].a);
                textLen = strlen(text);
                nlines = reply_layout(text, lines, MAXLINES);
                npages = nlines ? lines[nlines - 1].page + 1 : 1;
                page = 0;
                reveal = page_end(nlines, 0, textLen);
                seenJob[0] = 0;                                // returning to live re-lays the live text
                mode = M_REPLY;
            }
        }

        // ---- animation: type-on, talk mouth, blinks, idle breathe ----
        bool typingOn = false;
        if (mode == M_REPLY) {
            int pe = page_end(nlines, page, textLen);
            if (reveal < pe) {
                reveal += 3;
                if (reveal > pe) reveal = pe;
                utf8_step(&reveal, textLen);
                typingOn = true;
            }
        }
        bool streaming = (!reader && !strcmp(s.state, "replying")) || audio_playing();   // mouth moves while she speaks
        if ((typingOn || streaming) && now >= talkFlip) {
            talkOpen = !talkOpen;
            talkFlip = now + (talkOpen ? 110 : 90) + (rand() % 60);
        }
        if (!typingOn && !streaming) talkOpen = false;
        bool blink = now >= blinkAt && now < blinkAt + 140;
        if (now >= blinkAt + 140) blinkAt = now + 2500 + (rand() % 3500);
        int bob = 0;
        if (mode == M_READY || mode == M_HELLO || mode == M_ASLEEP) {
            u64 ph = (now / 900) % 4;                          // slow 1 px breathe
            bob = (ph == 1 || ph == 2) ? 1 : 0;
        }

        // ---- build the view ----
        memset(&v, 0, sizeof v);
        v.mode = mode;
        v.talk = talkOpen;
        v.blink = blink;
        v.bob = bob;
        time_t t = time(NULL);
        struct tm* lt = localtime(&t);
        snprintf(v.clock, sizeof v.clock, "%02d:%02d", lt->tm_hour, lt->tm_min);
        v.elapsed = s.elapsed + (busy && s.polledAt ? (int)((now - s.polledAt) / 1000) : 0);
        v.helpers = s.helpers;
        v.tool = s.tool;
        v.code = s.code;
        v.err = s.a;
        v.text = text;
        v.lines = lines;
        v.nlines = nlines;
        v.page = page;
        v.npages = npages;
        v.reveal = reveal;
        {   // ease her aside when the page has ASCII art, back when it doesn't
            static float slide = 0;
            int target = mode == M_REPLY ? art_slide(lines, nlines, page, text) : 0;
            slide += (target - slide) * 0.18f;
            if (target == 0 && slide < 0.5f) slide = 0;
            v.slide = (int)(slide + 0.5f);
        }
        if (mode == M_REPLY) {
            if (npages > 1) snprintf(v.pagelabel, sizeof v.pagelabel, "%s%d / %d", reader ? "EARLIER  " : "", page + 1, npages);
            else if (reader) snprintf(v.pagelabel, sizeof v.pagelabel, "EARLIER");
        }
        v.cam = camUp;
        v.photo = photoReady && !busy;
        v.micOK = micOK;
        v.rec = recording;
        v.recSec = recLen / (RATE * 2.0f);
        v.busy = busy;
        v.reader = reader;
        v.cmd = s.cmd;
        v.desc = s.desc;
        v.rows = s.rows;
        v.nrows = s.nrows;
        v.toast = (s.toast[0] && now - s.toastAt < 2500) ? s.toast : NULL;

        // ---- draw ----
        C2D_TextBufClear(tb);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        float slider = osGet3DSliderState();
        v.eye = -slider;
        C2D_TargetClear(top, PAPER);
        C2D_SceneBegin(top);
        draw_top(&v);
        if (slider > 0.0f) {                               // right eye only when 3D is up
            v.eye = slider;
            C2D_TargetClear(topR, PAPER);
            C2D_SceneBegin(topR);
            draw_top(&v);
        }
        C2D_TargetClear(bot, PAPER);
        C2D_SceneBegin(bot);
        draw_bottom(&v);
        C3D_FrameEnd(0);
    }

    alive = false;
    if (recording) MICU_StopSampling();
    cam_exit();
    if (R_SUCCEEDED(threadJoin(th, 3000000000ULL))) threadFree(th);
    if (micOK) micExit();
    free(micbuf);
    free(rec);
    httpcExit();
    audio_exit();
    sprites_free();
    ui_exit();
    C2D_Fini();
    C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
