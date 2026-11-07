// Talk back (ndsp playback of the relay's /app/v2/speech PCM), the notification LED, and the
// voice on/off preference. Speech arrives from the worker thread as a malloc'd buffer in speechIn;
// the UI thread hands it to the DSP (needs linear memory) and frees it.
#include "app.h"

volatile u8*  speechIn = NULL;
volatile u32  speechInLen = 0;
volatile bool speechFirst = false;
volatile u32  speechInGen = 0;
volatile bool voiceOn = true;

static bool ndspOK = false, ledOK = false;
// talk back arrives in pieces (first sentence first), queued back to back on channel 0
#define QN 12
static ndspWaveBuf wb[QN];
static s16* pcm[QN];
static int qn = 0;
volatile u32 speechGen = 0;              // bumped on every stop: the worker drops pieces of a hushed reply
static int ledNow = -1;

void audio_init(void) {
    FILE* f = fopen(DIRP "/voice.txt", "r");
    if (f) {
        char t[8] = "";
        if (fgets(t, sizeof t, f) && !strncmp(t, "off", 3)) voiceOn = false;
        fclose(f);
    }
    ndspOK = R_SUCCEEDED(ndspInit());          // needs sdmc:/3ds/dspfirm.cdc (Rosalina DSP dump)
    if (ndspOK) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, SPEECH_RATE);
        ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
        float mix[12] = {0};
        mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(0, mix);
    }
    memset(wb, 0, sizeof wb);
    memset(pcm, 0, sizeof pcm);
    ledOK = R_SUCCEEDED(mcuHwcInit());
    led_set(HL_OFF);
}

bool audio_ok(void) { return ndspOK; }

bool audio_playing(void) {
    if (!ndspOK) return false;
    for (int i = 0; i < qn; i++)
        if (pcm[i] && (wb[i].status == NDSP_WBUF_QUEUED || wb[i].status == NDSP_WBUF_PLAYING)) return true;
    return false;
}

void audio_stop(void) {
    speechGen++;
    if (!ndspOK) return;
    ndspChnWaveBufClear(0);
    svcSleepThread(20 * 1000000LL);             // let the DSP let go of the buffers
    for (int i = 0; i < QN; i++) if (pcm[i]) { linearFree(pcm[i]); pcm[i] = NULL; }
    memset(wb, 0, sizeof wb);
    qn = 0;
}

// first piece of a reply (part 0) starts fresh; later pieces queue behind what is playing
static void audio_add(const u8* data, u32 len, bool first) {
    if (!ndspOK || len < 2) return;
    if (first || qn >= QN) { u32 g = speechGen; audio_stop(); speechGen = g; }
    if (!first && !audio_playing()) {           // previous piece already ended: recycle the slots
        u32 g = speechGen; audio_stop(); speechGen = g;
    }
    s16* p = linearAlloc(len);
    if (!p) return;
    memcpy(p, data, len);
    DSP_FlushDataCache(p, len);
    pcm[qn] = p;
    memset(&wb[qn], 0, sizeof wb[qn]);
    wb[qn].data_vaddr = p;
    wb[qn].nsamples = len / 2;
    ndspChnWaveBufAdd(0, &wb[qn]);
    qn++;
}

// UI thread, once a frame: play whatever the worker fetched
void audio_pump(void) {
    u8* in = NULL;
    u32 n = 0;
    bool first = false, stale = false;
    LightLock_Lock(&lk);
    if (speechIn) {
        in = (u8*)speechIn; n = speechInLen; first = speechFirst; stale = speechInGen != speechGen;
        speechIn = NULL; speechInLen = 0;
    }
    LightLock_Unlock(&lk);
    if (!in) return;
    if (voiceOn && !stale) audio_add(in, n, first);
    free(in);
}

void voice_toggle(void) {
    voiceOn = !voiceOn;
    if (!voiceOn) audio_stop();
    FILE* f = fopen(DIRP "/voice.txt", "w");
    if (f) { fputs(voiceOn ? "on" : "off", f); fclose(f); }
    say(!ndspOK ? "No sound: dspfirm.cdc missing." : voiceOn ? "Voice on." : "Voice off.");
}

// Notification LED: ATTN = quick amber pulse (approval waiting), DONE = slow blue breathe
// (a turn finished; any input clears it).
void led_set(int kind) {
    if (!ledOK || kind == ledNow) return;
    ledNow = kind;
    InfoLedPattern p;
    memset(&p, 0, sizeof p);
    if (kind == HL_ATTN || kind == HL_DONE) {
        p.delay = kind == HL_ATTN ? 0x01 : 0x03;   // 1/16 s per step
        p.smoothing = 0x30;
        p.loopDelay = 0x00;                       // loop until changed
        for (int i = 0; i < 32; i++) {
            int tri = i < 16 ? i * 16 : (31 - i) * 16;   // 0..240..0
            if (kind == HL_ATTN) { p.redPattern[i] = tri; p.greenPattern[i] = tri / 3; }
            else { p.bluePattern[i] = tri; p.greenPattern[i] = tri / 6; }
        }
    }
    MCUHWC_SetInfoLedPattern(&p);
}

void audio_exit(void) {
    audio_stop();
    if (ndspOK) ndspExit();
    if (ledOK) { ledNow = -1; led_set(HL_OFF); mcuHwcExit(); }
}
