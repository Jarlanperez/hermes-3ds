// Camera (v2.3): press R for a live viewfinder from the outer camera on the top screen, A/R snaps, B closes.
// The snapshot (400x240 RGB565, little-endian) is uploaded to the relay, which attaches it to his next
// question. A dedicated thread re-arms CAMU_SetReceiving the instant a frame lands: re-arming from the
// 60 Hz UI loop sometimes missed the next frame start, the camera raised a buffer error, and the feed
// froze for a second or two while it restarted (seen on device every ~4 s). Three buffers rotate so the
// one being filled is never the one being drawn. After a snap the texture keeps the photo for the preview.
#include "app.h"

#define CW 400
#define CH 240
#define CAM_BYTES (CW * CH * 2)

u8*  photo = NULL;                       // the snapped frame, sent by the worker (CMD_PHOTO)
u32  photoLen = 0;

static volatile bool on = false, run = false, fresh = false;
static bool gotFrame = false;
static u8* bufs[3] = {NULL, NULL, NULL};
static volatile int ready = -1;          // last finished buffer
static volatile int drawing = -1;        // buffer the UI is reading
static LightLock ck;
static Thread th = NULL;
static u32 xfer = 0;
static C3D_Tex tex;
static bool texOK = false;
static Tex3DS_SubTexture sub = { CW, CH, 0.0f, 1.0f, CW / 512.0f, 1.0f - CH / 256.0f };
static u8 mort[64];

C2D_Image cam_image(void) { return (C2D_Image){ &tex, &sub }; }
bool cam_has_frame(void) { return texOK && gotFrame; }

static void tex_from(const u16* src) {
    u16* dst = (u16*)tex.data;
    for (int y = 0; y < CH; y++) {
        int ty = y;                      // memory row 0 = subtexture top (v 1.0); 255 - y showed it upside down on device
        u16* row = dst + (ty >> 3) * (512 / 8) * 64;
        int my = (ty & 7) * 8;
        const u16* s = src + y * CW;
        for (int x = 0; x < CW; x++) row[(x >> 3) * 64 + mort[my + (x & 7)]] = s[x];
    }
    C3D_TexFlush(&tex);
}

static int free_buf(int not) {           // call with ck held
    for (int i = 0; i < 3; i++) if (i != ready && i != drawing && i != not) return i;
    return 0;
}

static void cam_thread(void* arg) {
    (void)arg;
    Handle ev[2] = {0, 0};               // [0] buffer error, [1] frame received
    CAMU_GetBufferErrorInterruptEvent(&ev[0], PORT_CAM1);
    LightLock_Lock(&ck);
    int cur = free_buf(-1);
    LightLock_Unlock(&ck);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_SetReceiving(&ev[1], bufs[cur], PORT_CAM1, CAM_BYTES, (s16)xfer);   // armed before capture starts
    CAMU_StartCapture(PORT_CAM1);
    while (run) {
        s32 idx = -1;
        if (R_FAILED(svcWaitSynchronizationN(&idx, ev, 2, false, 100 * 1000000LL)) || idx < 0 || idx > 1) continue;
        if (idx == 1) {                  // frame done: hand it over, re-arm into a free buffer at once
            svcCloseHandle(ev[1]); ev[1] = 0;
            LightLock_Lock(&ck);
            ready = cur;
            fresh = true;
            cur = free_buf(cur);
            LightLock_Unlock(&ck);
            CAMU_SetReceiving(&ev[1], bufs[cur], PORT_CAM1, CAM_BYTES, (s16)xfer);
        } else {                         // buffer error: re-arm and keep going (no stop/clear cycle)
            svcClearEvent(ev[0]);
            if (ev[1]) { svcCloseHandle(ev[1]); ev[1] = 0; }
            CAMU_SetReceiving(&ev[1], bufs[cur], PORT_CAM1, CAM_BYTES, (s16)xfer);
            CAMU_StartCapture(PORT_CAM1);
        }
    }
    CAMU_StopCapture(PORT_CAM1);
    for (int i = 0; i < 2; i++) if (ev[i]) svcCloseHandle(ev[i]);
}

bool cam_start(void) {
    if (on) return true;
    if (!texOK) {
        LightLock_Init(&ck);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            mort[y * 8 + x] = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
        texOK = C3D_TexInit(&tex, 512, 256, GPU_RGB565);
        if (!texOK) return false;
        C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
        memset(tex.data, 0, tex.size);
    }
    for (int i = 0; i < 3; i++) if (!bufs[i]) bufs[i] = malloc(CAM_BYTES);
    if (!bufs[0] || !bufs[1] || !bufs[2] || R_FAILED(camInit())) return false;
    CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    CAMU_SetNoiseFilter(SELECT_OUT1, true);
    CAMU_SetAutoExposure(SELECT_OUT1, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetMaxBytes(&xfer, CW, CH);
    CAMU_SetTransferBytes(PORT_CAM1, xfer, CW, CH);
    CAMU_Activate(SELECT_OUT1);
    ready = -1; drawing = -1; fresh = false; gotFrame = false;
    run = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    th = threadCreate(cam_thread, NULL, 16 * 1024, prio - 2, -2, false);
    if (!th) { run = false; CAMU_Activate(SELECT_NONE); camExit(); return false; }
    on = true;
    return true;
}

// UI thread, once a frame: draw the newest finished frame if there is one
void cam_pump(void) {
    if (!on || !fresh) return;
    LightLock_Lock(&ck);
    drawing = ready;
    fresh = false;
    LightLock_Unlock(&ck);
    if (drawing >= 0) { tex_from((const u16*)bufs[drawing]); gotFrame = true; }
    LightLock_Lock(&ck);
    drawing = -1;
    LightLock_Unlock(&ck);
}

void cam_stop(void) {
    if (!on) return;
    run = false;
    threadJoin(th, 1000000000ULL);
    threadFree(th);
    th = NULL;
    CAMU_Activate(SELECT_NONE);
    camExit();
    on = false;
}

// freeze the newest frame as the photo; the texture keeps it so the top screen can show the preview
bool cam_snap(void) {
    if (!gotFrame || ready < 0) return false;
    if (!photo) photo = malloc(CAM_BYTES);
    if (!photo) return false;
    LightLock_Lock(&ck);
    drawing = ready;
    LightLock_Unlock(&ck);
    memcpy(photo, bufs[drawing], CAM_BYTES);
    LightLock_Lock(&ck);
    drawing = -1;
    LightLock_Unlock(&ck);
    tex_from((const u16*)photo);
    photoLen = CAM_BYTES;
    CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
    return true;
}

void cam_exit(void) {
    cam_stop();
    if (texOK) { C3D_TexDelete(&tex); texOK = false; }
    for (int i = 0; i < 3; i++) { free(bufs[i]); bufs[i] = NULL; }
    free(photo); photo = NULL;
}
