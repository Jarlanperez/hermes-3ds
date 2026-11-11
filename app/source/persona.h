// Persona packs: art, names and screen copy loaded from sdmc:/3ds/hermes/personas/<id>/ (tools/build_persona_pack.py).
// Hermes is built in (persona 0); every valid pack on the card is persona 1..n.
#pragma once
#include "app.h"

enum { PO_HELLO, PO_LISTEN, PO_THINK, PO_CODE, PO_REPLY, PO_ASLEEP, PO_ATTENTION, PO_ERROR, PO_BATTERY, PO_COUNT };
// frames: FR_BLINK + pose is that pose's blink; then the special-purpose frames
enum { FR_BLINK = 0, FR_REPLY_TALK = PO_COUNT, FR_REPLY_CLOSED, FR_CODE_TYPE, FR_CODE_WIPE_A, FR_CODE_WIPE_B,
       FR_ASLEEP_SNORE, FR_BATTERY_CHEW, FR_FX0, FR_FX1, FR_FX2, FR_FX3, FR_FX4, FR_FX5, FR_FX6, FR_FX7, FR_COUNT };   // fx: looping layer under the figure
#define M_COUNT   (M_BATTERY + 1)
#define PK_MAX    8
#define PK_SHEETS 6

typedef struct { unsigned char sheet, idx; short x, y, tmax; } PStill;
typedef struct { unsigned char sheet, idx; short x, y; bool ok; } PFrame;
typedef struct {
    char id[24], name[24], label[24], greet[48];
    int  nsheets, label_w;                         // label_w 0 = fit the text
    int  hover;                                    // px the figure floats up and down (0 = still)
    C2D_SpriteSheet sh[PK_SHEETS];                 // loaded only while this persona is active
    PStill st[PO_COUNT];
    PFrame fr[FR_COUNT];
    unsigned short edge[PO_COUNT][240];            // leftmost opaque x per row: text stops beside the figure
    const char* head[M_COUNT][3];                  // NULL = the Hermes default
    const char* sub[M_COUNT];                      // "{t}" = time on this turn
    signed char pose[M_COUNT];
    char arena[3072];
    int  used;
} Pack;

extern Pack* PK;                                   // active pack, NULL while Hermes is up
void personas_free(void);
