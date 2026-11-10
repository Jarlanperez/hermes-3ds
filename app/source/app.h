// Hermes for Nintendo 3DS: shared types between the UI thread and the network worker.
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRP     "sdmc:/3ds/hermes"
#define RATE     16360
#define MAX_REC  (RATE * 2 * 30)        // 30 s of 16-bit mono
#define MIN_REC  (RATE * 2 / 2)         // 0.5 s
#define A_CAP    6000
#define NROWS    6
#define MAXLINES 400
#define SPEECH_RATE 24000                // relay /app/v2/speech: mono PCM16

// ---------- shared with the worker (guard with lk) ----------
typedef struct { char t[8]; char q[200]; char a[1200]; } Row;
enum { LINK_BOOT, LINK_OK, LINK_OFFLINE, LINK_PAIR };
enum { CMD_NONE, CMD_VOICE, CMD_ASK, CMD_ONCE, CMD_DENY, CMD_STOP, CMD_RESET, CMD_PHOTO, CMD_UNPHOTO };
typedef struct {
    int  link;
    char code[8];
    char state[16], job[24], kind[12], tool[48], q[400], cmd[600], desc[300];
    char a[A_CAP];
    int  elapsed, helpers;
    u64  polledAt;                      // osGetTime() of the last good poll
    Row  rows[NROWS];
    int  nrows;
    char toast[96];
    u64  toastAt;
} Shared;

extern Shared sh;
extern LightLock lk;
extern volatile int  cmd;               // one pending request from the UI thread
extern volatile bool alive, sending, asleepFlag;
extern char server[128], token[64], typed[512];
extern char helloName[16];              // from the relay config: HEY <NAME> (empty = HELLO)
bool ask_server(void);                  // keyboard prompt for the relay address, saved to server.txt
extern volatile int persona;            // who answers: 0 = Hermes (built in), 1.. = personas (persona.c)
void persona_boot(void);                // scan sdmc:/3ds/hermes/personas/ for art packs, read persona.txt
void persona_relay(const char* list);   // worker: the relay's persona list (id \t name \t greet per line)
void persona_sync(void);                // UI thread: merge that list in (once), restore the saved choice
void persona_set(int p);                // wraps; loads that pack's sheets, saves persona.txt
void persona_gc(void);                  // free the previous pack's sheets once the GPU is done with them
int  persona_count(void);
const char* persona_name(void);         // id sent as X-HH-Persona ("hermes", "pirate", ...)
const char* persona_display(void);      // "Pip"
const char* persona_label(void);        // top-left tag, "HERMES" / "PIP"
const char* persona_greet(void);        // toast on switch
extern u8*  rec;
extern u32  recLen;

void net_boot(void);
void worker(void* arg);
void say(const char* msg);             // bottom-screen toast for ~2.5 s

// ---------- talk back + LED (audio.c) ----------
enum { HL_OFF, HL_ATTN, HL_DONE };
extern volatile u8*  speechIn;          // worker -> UI: fetched speech (malloc'd), UI frees
extern volatile u32  speechInLen;
extern volatile bool speechFirst;       // speechIn is part 0 of a new reply
extern volatile u32  speechInGen;       // speechGen when the reply was fetched
extern volatile u32  speechGen;         // bumped by audio_stop()
extern volatile bool voiceOn;
void audio_init(void);
void audio_exit(void);
bool audio_ok(void);
bool audio_playing(void);
void audio_stop(void);
void audio_pump(void);
void voice_toggle(void);
void led_set(int kind);

// ---------- camera (camera.c) ----------
extern u8*  photo;                      // snapped frame, 400x240 RGB565, uploaded by CMD_PHOTO
extern u32  photoLen;
extern volatile bool photoSending;       // snapped, waiting for or in upload
extern volatile bool photoReady;        // the relay holds a photo for his next question
bool cam_start(void);
void cam_pump(void);
void cam_stop(void);
bool cam_snap(void);
bool cam_has_frame(void);
C2D_Image cam_image(void);
void cam_exit(void);
bool busy_state(const char* s);

// ---------- drawing helpers (ui.c) ----------
enum { F_HEAD, F_BIG, F_BTN, F_BODY, F_BODYB, F_SMALL, F_LABEL, F_MONO, F_COUNT };
typedef struct { C2D_Font f; float s, k; int lf; } Font;   // s: draw scale, k: px per charWidth unit
extern Font fonts[F_COUNT];
extern C2D_TextBuf tb;
extern u32 PAPER, INK, DIM, FIELD, PAPERDIM;

typedef struct { int off, len; short y, page; char art; } Line;   // art: \x1e line, mono, unwrapped

bool  ui_init(void);
void  ui_exit(void);
float tw(int f, const char* s, int n, float track);
void  dt(int f, const char* s, int n, float x, float y, u32 col, float track);
void  dtc(int f, const char* s, float cx, float y, u32 col, float track);
void  dtr(int f, const char* s, float rx, float y, u32 col, float track);
void  rect(float x, float y, float w, float h, u32 c);
void  box(float x, float y, float w, float h, float t, u32 c);
void  ellipsize(int f, const char* s, float maxw, char* out, size_t cap);
// Greedy word wrap. edge (per screen row, may be NULL) narrows each line to stop 8 px before her
// silhouette; lines that would pass y1 start a new page at y0. Returns the line count.
int   layout(int f, const char* s, const unsigned short* edge, int x0, int xr, int y0, int y1,
             int lh, int pgap, Line* out, int cap);

// ---------- screens (screens.c) ----------
enum { M_BOOT, M_OFFLINE, M_PAIR, M_HELLO, M_READY, M_ASLEEP, M_LISTEN, M_THINK, M_CODING,
       M_RESEARCH, M_MAKING, M_DELEGATE, M_APPROVAL, M_REPLY, M_DONE, M_ERROR, M_LOOKING, M_BATTERY };
typedef struct {
    int  mode;
    bool talk, blink;                   // mouth open, eyes closed this frame
    int  bob;                           // idle bob in px
    char clock[8];
    int  elapsed, helpers;
    const char* tool;
    const char* code;
    const char* err;
    // reply page
    const char* text;
    const Line* lines;
    int  nlines, page, npages, reveal;
    int  slide;                         // px she steps right so ASCII art clears her
    float eye;                          // 3D: -1 left / +1 right, times the slider (0 = flat)
    bool cam;                           // viewfinder up (hold R)
    bool photo;                         // a photo rides with the next question
    bool photoSending;                  // that photo is still uploading
    char pagelabel[40];
    bool talking;                       // mouth is moving (type-on or speech playing)
    u64  now;                           // osGetTime() for this frame (pack animation clock)
    // bottom
    bool micOK, rec, busy, reader;
    float recSec;
    const char* cmd;
    const char* desc;
    const Row* rows;
    int  nrows;
    const char* toast;
} View;

void sprites_load(void);
void sprites_free(void);
int  reply_layout(const char* text, Line* out, int cap);
int  art_slide(const Line* lines, int nlines, int page, const char* text);
void draw_top(const View* v);
void draw_bottom(const View* v);
extern int apprY;                       // approval buttons' top y, set while drawing
