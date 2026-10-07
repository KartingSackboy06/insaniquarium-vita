/*
 * Intro video playback via SceAvPlayer (hardware H.264/AAC decode).
 *
 * Video: SceAvPlayer hands back YUV420 semi-planar frames (Y plane, then
 *        interleaved V/U = NV21). We convert on the CPU to ABGR8888 and show them
 *        with sceDisplaySetFrameBuf, letterboxed on the 960x544 screen. No GXM /
 *        vitaGL is used, so the game's own GL init later is not disturbed.
 * Audio: PCM frames from the player are pushed to a sceAudioOut port from a
 *        helper thread. Pulling the audio is also what keeps the player's clock
 *        (and so the video timing) running.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/display.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include "common.h"
#include "intro_video.h"
#include "yuv_convert.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* If the picture looks sheared, the row pitch guess below is the knob (colour order is
 * VIDEO_SWAP_UV in yuv_convert.h). */
#ifndef VIDEO_STRIDE_ALIGN
#define VIDEO_STRIDE_ALIGN 16      /* Y-plane row pitch = width rounded up to this */
#endif

#define START_TIMEOUT_US  (5u * 1000 * 1000)     /* player never became active */
#define MAX_PLAY_US       (5u * 60 * 1000 * 1000) /* safety net so a hang cannot brick the launch */
#define END_IDLE_US       (1000u * 1000)          /* no video AND no audio for this long near the end = end of stream */
#define STALL_IDLE_US     (4u * 1000 * 1000)      /* ...and this long in the MIDDLE of the video = stall (logged loudly) */
#define END_SLACK_MS      1000u                   /* "near the end" = within this much of the stream duration */
#define JOIN_TIMEOUT_US   (3u * 1000 * 1000)      /* never wait longer than this on teardown */
#define AUDIO_GRAIN       1024
#define BLACK_SCREEN_US   (600u * 1000)           /* black screen shown at launch BEFORE the player is started (0 = off) */

/* ---------------------------------------------------------------- memory */
static void *av_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p; if (align < 16) align = 16;
    return memalign(align, size);
}
static void av_free(void *p, void *mem) { (void)p; free(mem); }

/* Decoder output frames need physically contiguous memory. */
#define MAX_TEX 24
static struct { void *base; SceUID uid; } g_tex[MAX_TEX];

static void *av_alloc_tex(void *p, uint32_t align, uint32_t size) {
    (void)p; (void)align;
    size = ALIGN_UP(size, 1024u * 1024u);
    SceUID uid = sceKernelAllocMemBlock("avp_tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    if (uid < 0) { LOG("intro: texture alloc of %u bytes failed: 0x%08x", size, uid); return NULL; }
    void *base = NULL;
    sceKernelGetMemBlockBase(uid, &base);
    for (int i = 0; i < MAX_TEX; i++)
        if (!g_tex[i].base) { g_tex[i].base = base; g_tex[i].uid = uid; return base; }
    sceKernelFreeMemBlock(uid);
    return NULL;
}
static void av_free_tex(void *p, void *mem) {
    (void)p;
    for (int i = 0; i < MAX_TEX; i++)
        if (g_tex[i].base == mem) { sceKernelFreeMemBlock(g_tex[i].uid); g_tex[i].base = NULL; return; }
}

/* --------------------------------------------------------------- display */
static SceUID    g_fb_uid[2] = { -1, -1 };
static uint32_t *g_fb[2];
static int       g_fb_cur;

static int fb_init(void) {
    const uint32_t one = ALIGN_UP(SCREEN_W * SCREEN_H * 4, 256 * 1024);
    for (int i = 0; i < 2; i++) {
        g_fb_uid[i] = sceKernelAllocMemBlock("intro_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, one, NULL);
        if (g_fb_uid[i] < 0) { LOG("intro: framebuffer alloc failed: 0x%08x", g_fb_uid[i]); intro_video_release(); return -1; }
        void *base = NULL;
        sceKernelGetMemBlockBase(g_fb_uid[i], &base);
        g_fb[i] = base;
        memset(base, 0, one);
    }
    return 0;
}

static void fb_show(int idx) {
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = g_fb[idx];
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void draw_frame(const SceAvPlayerFrameInfo *f) {
    const int w = f->details.video.width, h = f->details.video.height;
    if (w <= 0 || h <= 0 || !f->pData) return;
    const int stride = ALIGN_UP(w, VIDEO_STRIDE_ALIGN);
    const uint8_t *yp = f->pData;
    const uint8_t *cp = yp + (size_t)stride * h;
    uint32_t *dst = g_fb[g_fb_cur ^ 1];             /* draw into the hidden buffer */

#if YUV_HAVE_NEON
    if (w == SCREEN_W && h == SCREEN_H) {           /* the PopCap logo: 960x544, no scaling needed */
        yuv_convert_1to1_neon(yp, cp, stride, w, h, dst, SCREEN_W);
    } else
#endif
    {
        static int xmap[SCREEN_W];
        yuv_convert_scaled(yp, cp, stride, w, h, dst, SCREEN_W, SCREEN_H, xmap);
    }
    g_fb_cur ^= 1;
    fb_show(g_fb_cur);
}

/* ----------------------------------------------------------------- audio */
/*
 * IMPORTANT (this was the bug): the MAIN audio port only accepts 48000 Hz. The intro video's
 * AAC track is 44100 Hz, so opening MAIN at the file's rate fails (0x80260008). The old code
 * then left the audio thread, nobody pulled audio from the player any more, the player's clock
 * stalled, video frames stopped arriving and the "no frames for 1 s" check ended playback early.
 *
 * Now: open MAIN at 48000 Hz and resample in software (linear interpolation). If MAIN cannot be
 * opened, try BGM at the native rate. If no port can be opened the thread still keeps draining
 * the player in real time so video playback is never starved by an audio failure.
 */
static SceAvPlayerHandle g_player;
static volatile int      g_stop;
static volatile uint64_t g_last_frame_us;   /* last time the player delivered any audio/video */

/* ---- diagnostics (all written to log.txt by the main loop once per second) */
static volatile uint32_t g_a_frames, g_a_bytes, g_a_out_calls, g_a_out_errors;
static volatile uint32_t g_a_rate, g_a_ch;
static volatile uint64_t g_a_last_ts;        /* timestamp (ms) of the newest audio frame */
static volatile uint64_t g_a_max_gap_us;     /* longest wait between two audio frames */
static volatile int      g_a_port = -99;     /* sceAudioOut port handle, <0 = none */
static volatile int      g_a_state;          /* 0 none, 1 waiting for 1st frame, 2 playing, 3 drain-only (no port) */
static volatile uint32_t g_a_empty_polls;    /* GetAudioData returned false */

#define AUDIO_MAIN_RATE 48000

/* Output side. The player hands us audio at its own pace (it can be late by tens of ms), but
 * sceAudioOut wants a new grain every 21 ms or it plays a gap. So: the producer thread (below)
 * drops frames into a ring buffer, and a separate output thread starts only once PREBUFFER
 * frames are queued, then feeds the port at a steady rate from rotating buffers. A late frame
 * from the player now eats into the cushion instead of becoming a click. */
#define RING_FRAMES       (1u << 15)                 /* 32768 stereo frames, ~0.68 s */
#define PREBUFFER_FRAMES  (AUDIO_GRAIN * 3)          /* ~64 ms cushion before output starts */
#define NUM_OBUF          3
static int16_t g_ring[RING_FRAMES * 2];
static volatile uint32_t g_ring_wr, g_ring_rd;       /* free-running frame counters (wr: producer, rd: consumer) */
static volatile int      g_prod_done;                /* producer has finished */
static int16_t g_obuf[NUM_OBUF][AUDIO_GRAIN * 2] __attribute__((aligned(64)));   /* grains handed to sceAudioOut */
static volatile uint32_t g_a_underruns;              /* output thread found the ring empty (a gap was played) */
static volatile uint32_t g_a_late;                   /* sceAudioOutOutput returned instantly = output thread ran late */
/* How far ahead of the speaker the producer may run. The player's clock (and so the video) advances as audio is
 * pulled, so every ms queued here is a ms the picture is ahead of the sound. The ring used to fill completely
 * (~680 ms) which put the video ~450 ms ahead; now it is capped (loader.cfg intro_audio_buffer, default 117 ms). */
static uint32_t g_ring_cap = RING_FRAMES;
static uint32_t g_out_rate = AUDIO_MAIN_RATE;
static uint32_t g_rs_step;                   /* input frames per output frame, 16.16 */
static uint32_t g_rs_phase;                  /* 16.16 position between prev and cur */
static int      g_rs_have_prev;
static int16_t  g_rs_prev[2];

static inline void emit_frame(int port, int16_t l, int16_t r) {
    (void)port;
    while (g_ring_wr - g_ring_rd >= g_ring_cap && !g_stop) sceKernelDelayThread(1000);   /* enough queued: wait */
    const uint32_t i = g_ring_wr & (RING_FRAMES - 1);
    g_ring[i * 2]     = l;
    g_ring[i * 2 + 1] = r;
    __sync_synchronize();                      /* data visible before the counter moves */
    g_ring_wr = g_ring_wr + 1;
}

/* Push n interleaved s16 frames (stride channels) through the resampler to the output. */
static void audio_push(int port, const int16_t *src, uint32_t n, int stride) {
    for (uint32_t i = 0; i < n; i++) {
        int16_t cur[2];
        cur[0] = src[i * stride];
        cur[1] = stride > 1 ? src[i * stride + 1] : cur[0];
        if (g_rs_step == 65536u) { emit_frame(port, cur[0], cur[1]); continue; }   /* same rate */
        if (!g_rs_have_prev) { g_rs_prev[0] = cur[0]; g_rs_prev[1] = cur[1]; g_rs_have_prev = 1; }
        while (g_rs_phase < 65536u) {
            int32_t f = (int32_t)g_rs_phase;
            int16_t l = (int16_t)(g_rs_prev[0] + (((int32_t)cur[0] - g_rs_prev[0]) * f >> 16));
            int16_t r = (int16_t)(g_rs_prev[1] + (((int32_t)cur[1] - g_rs_prev[1]) * f >> 16));
            emit_frame(port, l, r);
            g_rs_phase += g_rs_step;
        }
        g_rs_phase -= 65536u;
        g_rs_prev[0] = cur[0]; g_rs_prev[1] = cur[1];
    }
}

static int audio_open_port(uint32_t in_rate) {
    if (g_intro_bgm) {                              /* loader.cfg intro_bgm=1: experiment with the BGM port */
        int bp = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, in_rate, SCE_AUDIO_OUT_MODE_STEREO);
        DLOG("intro: sceAudioOutOpenPort(BGM forced, %d, %u Hz, stereo) = 0x%08x", AUDIO_GRAIN, in_rate, bp);
        if (bp >= 0) { g_out_rate = in_rate; return bp; }
    }
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, AUDIO_MAIN_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    DLOG("intro: sceAudioOutOpenPort(MAIN, %d, %d Hz, stereo) = 0x%08x", AUDIO_GRAIN, AUDIO_MAIN_RATE, port);
    if (port >= 0) { g_out_rate = AUDIO_MAIN_RATE; return port; }

    /* Fallback: BGM accepts the native rate, no resampling needed. */
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, in_rate, SCE_AUDIO_OUT_MODE_STEREO);
    LOG("intro: sceAudioOutOpenPort(BGM, %d, %u Hz, stereo) = 0x%08x", AUDIO_GRAIN, in_rate, port);
    if (port >= 0) { g_out_rate = in_rate; return port; }

    LOG("intro: NO audio port available - video will play silent, audio is still drained");
    g_out_rate = in_rate ? in_rate : 44100;
    return -1;
}

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int port = -1, stride = 2, got_first = 0;
    SceAvPlayerFrameInfo f;
    uint64_t last_us = sceKernelGetProcessTimeWide();
    g_a_state = 1;
    DLOG("intro: audio thread started");

    while (!g_stop) {
        if (!sceAvPlayerGetAudioData(g_player, &f)) { g_a_empty_polls++; sceKernelDelayThread(1000); continue; }

        const uint64_t now = sceKernelGetProcessTimeWide();
        if (got_first && now - last_us > g_a_max_gap_us) g_a_max_gap_us = now - last_us;
        last_us = now;

        if (!got_first) {
            got_first = 1;
            stride = f.details.audio.channelCount ? f.details.audio.channelCount : 1;   /* only the first two are used */
            const uint32_t in_rate = f.details.audio.sampleRate;
            g_a_rate = in_rate; g_a_ch = f.details.audio.channelCount;
            DLOG("intro: first audio frame: %u Hz, %u ch, %u bytes, ts=%u ms, data=%p",
                in_rate, (unsigned)f.details.audio.channelCount, f.details.audio.size,
                (unsigned)f.timeStamp, f.pData);
            port = audio_open_port(in_rate);
            g_a_port = port;
            g_rs_step = (uint32_t)(((uint64_t)in_rate << 16) / g_out_rate);
            if (g_rs_step == 0) g_rs_step = 65536u;
            {   /* queue cap in frames: at least PREBUFFER + 2 grains so the output thread can always start */
                uint32_t cap = (uint32_t)((uint64_t)g_intro_audio_buffer * g_out_rate / 1000u);
                if (cap < PREBUFFER_FRAMES + 2 * AUDIO_GRAIN) cap = PREBUFFER_FRAMES + 2 * AUDIO_GRAIN;
                if (cap > RING_FRAMES) cap = RING_FRAMES;
                g_ring_cap = cap;
                DLOG("intro: audio queue cap %u frames (%u ms)", cap, (unsigned)((uint64_t)cap * 1000u / g_out_rate));
            }
            if (port >= 0) {
                int vv = (int)((int64_t)SCE_AUDIO_OUT_MAX_VOL * g_intro_volume / 100);
                int vol[2] = { vv, vv };
                int vr = sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                DLOG("intro: audio port %d open, out %u Hz, resample step=%u/65536, SetVolume=0x%08x",
                    port, g_out_rate, g_rs_step, vr);
            }
            g_a_state = port >= 0 ? 2 : 3;
        }

        g_a_frames++;
        g_a_bytes += f.details.audio.size;
        g_a_last_ts = f.timeStamp;
        g_last_frame_us = now;
        if (f.pData && f.details.audio.size)
            audio_push(port, (const int16_t *)f.pData, f.details.audio.size / (2u * (uint32_t)stride), stride);
    }
    g_prod_done = 1;
    DLOG("intro: audio producer exiting (%u frames, %u bytes)", g_a_frames, g_a_bytes);
    return 0;
}

/* Output thread: steady feed of sceAudioOut from the ring buffer. */
static int audio_out_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    uint32_t idx = 0;
    int started = 0, final = 0;
    DLOG("intro: audio output thread started");
    for (;;) {
        uint32_t avail = g_ring_wr - g_ring_rd;
        if (!started) {
            if (avail >= PREBUFFER_FRAMES || g_prod_done) started = 1;
            else { sceKernelDelayThread(1000); continue; }
        }
        int16_t *buf = g_obuf[idx];
        idx = (idx + 1) % NUM_OBUF;
        uint32_t n = 0;
        if (avail >= AUDIO_GRAIN) n = AUDIO_GRAIN;
        else if (g_prod_done) { n = avail; final = 1; }        /* tail of the stream */
        else { if (g_a_underruns++ < 5) DLOG("intro: audio underrun (ring has %u frames)", avail); }
        if (n) {
            __sync_synchronize();
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t r = (g_ring_rd + i) & (RING_FRAMES - 1);
                buf[i * 2] = g_ring[r * 2]; buf[i * 2 + 1] = g_ring[r * 2 + 1];
            }
            g_ring_rd = g_ring_rd + n;
        }
        if (n < AUDIO_GRAIN) memset(buf + n * 2, 0, (AUDIO_GRAIN - n) * 2 * sizeof(int16_t));   /* silence */

        const int port = g_a_port;
        if (port >= 0) {
            const uint64_t t = sceKernelGetProcessTimeWide();
            int r = sceAudioOutOutput(port, buf);
            g_a_out_calls++;
            if (sceKernelGetProcessTimeWide() - t < 3000u && g_a_out_calls > 3) g_a_late++;   /* did not have to wait */
            if (r < 0 && g_a_out_errors++ < 5) LOG("intro: sceAudioOutOutput failed: 0x%08x", r);
        } else {
            sceKernelDelayThread((uint32_t)((uint64_t)AUDIO_GRAIN * 1000000u / g_out_rate));   /* no port: keep real-time pace */
        }
        if (final) break;
        if (g_stop && g_prod_done && g_ring_wr == g_ring_rd) break;
    }
    const int port = g_a_port;
    if (port >= 0) {
        sceAudioOutOutput(port, NULL);                         /* wait for the last grain to finish playing */
        DLOG("intro: audio flushed");
        int rr = sceAudioOutReleasePort(port);
        DLOG("intro: audio port released: 0x%08x", rr);
    }
    g_a_port = -99;
    g_a_state = 0;
    DLOG("intro: audio output thread exiting (%u out calls, %u out errors, %u underruns, %u late)",
        g_a_out_calls, g_a_out_errors, g_a_underruns, g_a_late);
    return 0;
}

static int close_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    DLOG("intro: stopping player");
    sceAvPlayerStop(g_player);
    DLOG("intro: closing player");
    sceAvPlayerClose(g_player);
    DLOG("intro: player closed");
    return 0;
}

/* ----------------------------------------------------------- diagnostics */
/* Called by the player from its own threads. Event ids are not documented in the public
 * header, so just record them: a state change / warning / error shows up in the log with the
 * time it happened, which is what is needed to line it up with the status lines. */
static volatile uint64_t g_t0_us;
static void av_event(void *p, int32_t ev, int32_t src, void *data) {
    (void)p;
    DLOG("intro: [+%ums] player event id=%d source=%d data=%p",
        (unsigned)((sceKernelGetProcessTimeWide() - g_t0_us) / 1000), ev, src, data);
}

static uint64_t g_duration_ms;   /* from stream info, 0 if unknown */
static int      g_info_logged;

static void log_stream_info(void) {
    for (uint32_t id = 0; id < 4; id++) {
        SceAvPlayerStreamInfo si;
        memset(&si, 0, sizeof si);
        int r = sceAvPlayerGetStreamInfo(g_player, id, &si);
        if (r < 0) { if (id == 0) return; continue; }       /* not ready yet / no such stream */
        if (si.type == SCE_AVPLAYER_VIDEO) {
            DLOG("intro: stream %u: VIDEO %ux%u aspect=%.3f duration=%u ms start=%u ms", id,
                si.details.video.width, si.details.video.height, (double)si.details.video.aspectRatio,
                (unsigned)si.duration, (unsigned)si.startTime);
            if (si.duration > g_duration_ms) g_duration_ms = si.duration;
        } else if (si.type == SCE_AVPLAYER_AUDIO) {
            DLOG("intro: stream %u: AUDIO %u Hz %u ch duration=%u ms start=%u ms", id,
                si.details.audio.sampleRate, (unsigned)si.details.audio.channelCount,
                (unsigned)si.duration, (unsigned)si.startTime);
            if (si.duration > g_duration_ms) g_duration_ms = si.duration;
        } else {
            DLOG("intro: stream %u: type %u", id, si.type);
        }
    }
    g_info_logged = 1;
}

/* ------------------------------------------------------------------ main */
/* Everything that is global state of one playback, put back to its starting value so a second video
 * starts exactly like the first (audio queue, resampler, filters, counters, diagnostics). */
static void reset_playback_state(void) {
    g_stop = 0; g_prod_done = 0;
    g_ring_wr = 0; g_ring_rd = 0; g_ring_cap = RING_FRAMES;
    g_out_rate = AUDIO_MAIN_RATE; g_rs_step = 0; g_rs_phase = 0; g_rs_have_prev = 0;
    g_rs_prev[0] = g_rs_prev[1] = 0;
    g_a_frames = g_a_bytes = g_a_out_calls = g_a_out_errors = 0;
    g_a_rate = g_a_ch = 0; g_a_last_ts = 0; g_a_max_gap_us = 0;
    g_a_port = -99; g_a_state = 0; g_a_empty_polls = 0; g_a_underruns = g_a_late = 0;
    g_info_logged = 0; g_duration_ms = 0;
}

/* Plays ONE video from start to finish on the current display buffers. Returns 0 if it played, <0 if not.
 * *closed_out = 1 when the player is gone (or never existed). black_until_us (absolute, 0 = none) holds the
 * launch black screen before the player is started. */
static int play_one(const char *path, int index, int *closed_out, uint64_t black_until_us) {
    (void)index;
    *closed_out = 0;
    reset_playback_state();

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof init);
    init.memoryReplacement.allocate          = av_alloc;
    init.memoryReplacement.deallocate        = av_free;
    init.memoryReplacement.allocateTexture   = av_alloc_tex;
    init.memoryReplacement.deallocateTexture = av_free_tex;
    init.eventReplacement.eventCallback      = av_event;
    init.debugLevel = 0;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 4;
    init.autoStart = 1;
    g_player = sceAvPlayerInit(&init);
    if (!g_player) { LOG("intro: sceAvPlayerInit failed"); *closed_out = 1; return -4; }   /* no player exists: nothing to leak */

    if (black_until_us) {   /* first video only: hold the launch black screen until BLACK_SCREEN_US has passed */
        const uint64_t tn = sceKernelGetProcessTimeWide();
        if (tn < black_until_us) sceKernelDelayThread((uint32_t)(black_until_us - tn));
        DLOG("intro: launch black screen held until %u ms after start of intro code", (unsigned)(BLACK_SCREEN_US / 1000));
    }
    g_t0_us = sceKernelGetProcessTimeWide();
    int rc = sceAvPlayerAddSource(g_player, path);
    DLOG("intro: sceAvPlayerAddSource = 0x%08x (player handle %d)", rc, g_player);

    SceUID at = sceKernelCreateThread("intro_audio", audio_thread, 0x78, 64 * 1024, 0, 0, NULL);
    SceUID ot = sceKernelCreateThread("intro_audio_out", audio_out_thread, 0x70, 64 * 1024, 0, 0, NULL);
    g_prod_done = 0; g_ring_wr = g_ring_rd = 0; g_a_underruns = g_a_late = 0;
    if (ot >= 0) sceKernelStartThread(ot, 0, NULL);
    if (at >= 0) sceKernelStartThread(at, 0, NULL);

    /* Play until the player reports it is no longer active, or - because that flag may
     * never drop at end of stream - until it has stopped delivering frames for END_IDLE_US. */
    int started = 0, frames = 0;
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    g_last_frame_us = t0;
    uint64_t last_stat_us = t0, last_vid_us = t0, max_vid_gap_us = 0;
    uint64_t v_last_ts = 0;
    uint32_t v_frames_at_stat = 0, a_frames_at_stat = 0;
    const char *end_reason = "?";
    SceAvPlayerFrameInfo vf, nf;
    for (;;) {
        const uint64_t tnow = sceKernelGetProcessTimeWide();
        const uint64_t now = tnow - t0;
        const int active = sceAvPlayerIsActive(g_player);
        if (active && !started) { started = 1; DLOG("intro: [+%ums] player became active", (unsigned)(now / 1000)); }
        if (started && !g_info_logged) log_stream_info();

        /* One status line per second: this is the line to read when playback misbehaves.
         *   vid/aud  = frames delivered in the last second,   cur = player clock (ms)
         *   vts/ats  = timestamp of the newest video/audio frame (ms)
         *   gaps     = longest wait between video frames / audio frames so far (ms) */
        if (tnow - last_stat_us >= 1000000u) {
            DLOG("intro: [+%5ums] active=%d cur=%ums vid=%u(ts %ums) aud=%u(ts %ums) gaps v=%ums a=%ums "
                "aport=%d astate=%d aout=%u/%uerr aempty=%u urun=%u late=%u ring=%ums",
                (unsigned)(now / 1000), active, (unsigned)sceAvPlayerCurrentTime(g_player),
                (unsigned)(frames - v_frames_at_stat), (unsigned)v_last_ts,
                (unsigned)(g_a_frames - a_frames_at_stat), (unsigned)g_a_last_ts,
                (unsigned)(max_vid_gap_us / 1000), (unsigned)(g_a_max_gap_us / 1000),
                g_a_port, g_a_state, g_a_out_calls, g_a_out_errors, g_a_empty_polls, g_a_underruns, g_a_late,
                (unsigned)((uint64_t)(g_ring_wr - g_ring_rd) * 1000u / g_out_rate));
            last_stat_us = tnow; v_frames_at_stat = frames; a_frames_at_stat = g_a_frames;
        }

        if (started && !active) { end_reason = "player no longer active"; DLOG("intro: player no longer active"); break; }
        if (!started && now > START_TIMEOUT_US) { end_reason = "never started"; LOG("intro: player never started"); break; }
        if (frames > 0) {
            /* Idle = no video AND no audio delivered. Only call it the end of the stream when the
             * clock is actually at the end; stopping in the middle is a stall, which gets a longer
             * grace period and a loud log line instead of silently cutting the video short. */
            /* g_last_frame_us is written by the audio thread, so it can be NEWER than tnow (read at the
             * top of this iteration). Unsigned subtraction then wraps to a huge "idle" time and
             * triggers a false stall/end-of-stream. Re-read both and clamp. */
            const uint64_t lastf = g_last_frame_us;
            const uint64_t tchk = sceKernelGetProcessTimeWide();
            const uint64_t idle = tchk > lastf ? tchk - lastf : 0;
            const uint64_t cur = sceAvPlayerCurrentTime(g_player);
            const uint64_t pos = cur > v_last_ts ? cur : v_last_ts;
            const int near_end = g_duration_ms && pos + END_SLACK_MS >= g_duration_ms;
            if (idle > END_IDLE_US && (near_end || !g_duration_ms) ) {
                end_reason = "idle at end of stream";
                DLOG("intro: no frames for %u ms at %u ms of %u ms, assuming end of stream",
                    (unsigned)(idle / 1000), (unsigned)pos, (unsigned)g_duration_ms);
                break;
            }
            if (idle > STALL_IDLE_US) {
                end_reason = "STALLED mid-video";
                LOG("intro: STALL: no frames for %u ms at %u ms of %u ms (audio state %d, port %d, out errors %u) - giving up",
                    (unsigned)(idle / 1000), (unsigned)pos, (unsigned)g_duration_ms, g_a_state, g_a_port, g_a_out_errors);
                break;
            }
        }
        if (now > MAX_PLAY_US) { end_reason = "timeout"; LOG("intro: playback timed out"); break; }

        if (sceAvPlayerGetVideoData(g_player, &vf)) {
            /* If we fell behind, skip to the newest frame that is already due instead of
             * showing every one late (that is what made playback drift behind the audio). */
            int skipped = 0;
            for (int skip = 0; skip < 4 && sceAvPlayerGetVideoData(g_player, &nf); skip++) { vf = nf; frames++; skipped++; }
            const uint64_t t = sceKernelGetProcessTimeWide();
            if (frames > 0 && t - last_vid_us > max_vid_gap_us) max_vid_gap_us = t - last_vid_us;
            if (t - last_vid_us > 250000u && frames > 0)
                DLOG("intro: [+%ums] video gap of %u ms before ts=%ums", (unsigned)((t - t0) / 1000),
                    (unsigned)((t - last_vid_us) / 1000), (unsigned)vf.timeStamp);
            if (skipped) DLOG("intro: [+%ums] skipped %d late video frame(s), now ts=%ums",
                             (unsigned)((t - t0) / 1000), skipped, (unsigned)vf.timeStamp);
            if (frames == 0)
                DLOG("intro: first video frame: %ux%u ts=%ums data=%p", vf.details.video.width,
                    vf.details.video.height, (unsigned)vf.timeStamp, vf.pData);
            last_vid_us = t;
            v_last_ts = vf.timeStamp;
            g_last_frame_us = t;
            draw_frame(&vf);
            frames++;
        } else {
            sceKernelDelayThread(1000);
        }
    }
    DLOG("intro: end reason: %s; last video ts=%ums, last audio ts=%ums, stream duration=%ums, audio frames=%u",
        end_reason, (unsigned)v_last_ts, (unsigned)g_a_last_ts, (unsigned)g_duration_ms, g_a_frames);
    DLOG("intro: done after %u ms, %d video frames", (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), frames);

    /* Teardown. Every wait has a timeout so a misbehaving player cannot hang the launch. */
    g_stop = 1;
    if (at >= 0) {
        unsigned to = JOIN_TIMEOUT_US;
        int r = sceKernelWaitThreadEnd(at, NULL, &to);
        if (r < 0) LOG("intro: audio thread did not finish (0x%08x), continuing", r);
        else sceKernelDeleteThread(at);
    }
    if (ot >= 0) {                                    /* plays out the last ~64 ms of queued audio, then exits */
        unsigned to = JOIN_TIMEOUT_US;
        int r = sceKernelWaitThreadEnd(ot, NULL, &to);
        if (r < 0) LOG("intro: audio output thread did not finish (0x%08x), continuing", r);
        else sceKernelDeleteThread(ot);
    }
    int closed = 0;
    SceUID tt = sceKernelCreateThread("intro_close", close_thread, 0x78, 64 * 1024, 0, 0, NULL);
    if (tt >= 0) {
        sceKernelStartThread(tt, 0, NULL);
        unsigned to = JOIN_TIMEOUT_US;
        int r = sceKernelWaitThreadEnd(tt, NULL, &to);
        if (r < 0) LOG("intro: player close timed out (0x%08x), leaving it", r);
        else { sceKernelDeleteThread(tt); closed = 1; }
    }
    g_player = 0;
    *closed_out = closed;     /* the caller unloads the AVPLAYER module once EVERY player is gone */
    DLOG("intro: teardown finished");
    return started ? 0 : -5;
}

/* Videos in play order. The second one is optional: if soe_logo.mp4 is not there it is just skipped. */
static const char *const k_video_paths[2][2] = {
    { INTRO_VIDEO_PATH,  INTRO_VIDEO_PATH_ALT  },
    { INTRO_VIDEO2_PATH, INTRO_VIDEO2_PATH_ALT },
};

static void blank_display(void) {
    memset(g_fb[0], 0, SCREEN_W * SCREEN_H * 4);
    memset(g_fb[1], 0, SCREEN_W * SCREEN_H * 4);
    fb_show(g_fb_cur);
}

int intro_video_play(void) {
    const char *found[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2 && !found[i]; k++) {
            SceUID fd = sceIoOpen(k_video_paths[i][k], SCE_O_RDONLY, 0);
            if (fd >= 0) { sceIoClose(fd); found[i] = k_video_paths[i][k]; }
        }
    for (int i = 0; i < 2; i++) if (!found[i]) LOG("intro: video %d (%s) not found, skipping", i + 1, k_video_paths[i][0]);
    if (!found[0] && !found[1]) return -1;

    /* Black screen first: it is up the moment the app starts, so it is obvious the game is launching.
     * The module load and player setup run while it is showing; the first video is not started until
     * BLACK_SCREEN_US after this point. */
    if (fb_init() < 0) return -3;                      /* (fb_init cleans up after itself) */
    fb_show(g_fb_cur);                                  /* buffers are zeroed = black */
    const uint64_t black_until = sceKernelGetProcessTimeWide() + BLACK_SCREEN_US;

    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (rc < 0) { LOG("intro: could not load AVPLAYER module: 0x%08x", rc); intro_video_release(); return -2; }

    int result = -5, all_closed = 1, first = 1;
    for (int i = 0; i < 2; i++) {
        if (!found[i]) continue;
        if (!first) blank_display();                    /* black between the videos instead of a frozen last frame */
        LOG("intro: playing video %d: %s", i + 1, found[i]);
        int closed = 0;
        int r = play_one(found[i], i, &closed, first ? black_until : 0);
        first = 0;
        if (!closed) all_closed = 0;
        if (r == 0 || result != 0) result = r;          /* success of any video counts */
    }
    if (all_closed) sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);   /* only safe once every player is gone */

    /* Black out the display and give ALL of our CDRAM back before returning. vitaGL claims every
     * free byte of CDRAM as one contiguous block when the game creates its window; any block we
     * still hold (or the hole left by freeing only one of two) makes that allocation fail and the
     * game then dies on its first draw. */
    blank_display();
    sceDisplayWaitVblankStart();
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_IMMEDIATE);   /* NULL = display nothing (black) */
    sceDisplayWaitVblankStart();
    intro_video_release();
    return result;
}

void intro_video_release(void) {
    for (int i = 0; i < 2; i++)
        if (g_fb_uid[i] >= 0) { sceKernelFreeMemBlock(g_fb_uid[i]); g_fb_uid[i] = -1; }
}
