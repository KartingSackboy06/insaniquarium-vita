/*
 * Insaniquarium (Android port) loader for PS Vita.
 *
 *   ux0:app/INSNQ4R13/libmain.so        <- armeabi-v7a library from the APK, next to eboot.bin
 *   ux0:data/insaniquarium/properties/  <- the game's own data files
 *   ux0:data/insaniquarium/log.txt      <- written on every run
 *
 * Flow: map libmain.so -> relocate against our import table -> run its C++
 * constructors -> hand it a fake JNIEnv for nativeSetWorkingDir -> SDL_main.
 */
#define SDL_MAIN_HANDLED
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <malloc.h>
#include <SDL2/SDL.h>
#include "common.h"
#include <kubridge.h>
#include "so_util.h"
#include "intro_video.h"

extern void cheat_watchdog(void);
extern void loader_quit_hook(void *);
extern void loader_dlg_btn_hook_plain(void *, int, int);
extern void loader_dlg_btn_hook_thunk(void *, int, int);
extern int g_dlg_btn_orig_plain, g_dlg_btn_orig_thunk;
extern const SoImport g_imports[];
extern const size_t   g_imports_count;

/* Heap for the game's malloc/new, and GPU-visible memory is vitaGL's business. */
int _newlib_heap_size_user = 216 * 1024 * 1024;   /* needs ATTRIBUTE2=12 (365 MiB app memory) */

/* ---- Fake JNI: the game only calls GetStringUTFChars/ReleaseStringUTFChars. */
#define JNI_SLOTS 240
static void *jni_vtable[JNI_SLOTS];
static void *jni_env = jni_vtable;          /* JNIEnv* == pointer to the function table */

static int jni_unimplemented(void) { LOG("unimplemented JNI call"); return 0; }
static const char *jni_GetStringUTFChars(void *env, const char *s, unsigned char *copy) {
    (void)env; if (copy) *copy = 0; return s;          /* our "jstring" is just a C string */
}
static void jni_ReleaseStringUTFChars(void *env, const char *s, const char *u) { (void)env; (void)s; (void)u; }

/* Log CPU exceptions with addresses relative to the library, so crashes can be
 * traced back to code (addr2line / disassembly of libmain.so). */
static void crash_handler(KuKernelExceptionContext *c) {
    static const char *names[] = { "DATA ABORT", "PREFETCH ABORT", "UNDEFINED INSTRUCTION" };
    LOG("CRASH: %s  pc=%08x (lib+%x)  lr=%08x (lib+%x)  sp=%08x  fault_addr=%08x  FSR=%08x",
        c->exceptionType < 3 ? names[c->exceptionType] : "?",
        c->pc, c->pc - LOAD_BASE, c->lr, c->lr - LOAD_BASE, c->sp, c->FAR, c->FSR);
    LOG("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x", c->r0, c->r1, c->r2, c->r3, c->r4, c->r5);
    LOG("  r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x", c->r6, c->r7, c->r8, c->r9, c->r10, c->r11, c->r12);
    sceKernelExitProcess(0);
}
static void install_crash_handlers(void) {
    for (int t = 0; t < 3; t++) {
        int r = kuKernelRegisterExceptionHandler(t, crash_handler, NULL, NULL);
        if (r < 0) LOG("could not register exception handler %d: 0x%08x", t, r);
    }
}

#ifdef LOADER_DEBUG
struct free_mem_info { int size, size_user, size_cdram, size_phycont; };
extern int free_mem_query(struct free_mem_info *) __asm__("sceKernelGetFreeMemorySize");

extern volatile unsigned g_frames, g_max_dt_us;
extern volatile int g_gl_ready;
/* vitaGL memory counters (only called once the GL context exists). */
extern size_t vglMemFree(int type) __attribute__((weak));
#endif

/* Background watchdog, wakes every 250 ms. cheat_watchdog() cancels the on-screen-keyboard cheat entry if
 * the game stops drawing while it is open, so the game can never stay stuck behind it.
 * In a LOADER_DEBUG build it also logs memory use and frame rate every 2 s. */
static int watchdog_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
#ifdef LOADER_DEBUG
    unsigned tick = 0;
#endif
    for (;;) {
        for (int i = 0; i < 8; i++) { sceKernelDelayThread(250 * 1000); cheat_watchdog(); }
#ifdef LOADER_DEBUG
        struct mallinfo mi = mallinfo();
        struct free_mem_info fi; memset(&fi, 0, sizeof fi); fi.size = sizeof fi;
        free_mem_query(&fi);
        LOG("hb %u: heap used=%uMB (of %uMB) | free user=%uMB cdram=%uMB phycont=%uMB",
            ++tick, (unsigned)mi.uordblks >> 20, (unsigned)_newlib_heap_size_user >> 20,
            (unsigned)fi.size_user >> 20, (unsigned)fi.size_cdram >> 20, (unsigned)fi.size_phycont >> 20);
        unsigned fr = g_frames, mx = g_max_dt_us; g_frames = 0; g_max_dt_us = 0;
        LOG("   fps=%u  worst frame=%ums", fr / 2, mx / 1000);
        /* vitaGL pools: 0 = VRAM (CDRAM), 1 = RAM. Pool 2 (PHYCONT) is deliberately not queried: asking
         * current vitaGL for it dereferences a NULL mspace and crashes. Phycont is reported above. */
        if (g_gl_ready && vglMemFree) {
            LOG("   vitaGL free: vram=%uMB", (unsigned)(vglMemFree(0) >> 20));
            LOG("   vitaGL free: ram=%uMB",  (unsigned)(vglMemFree(1) >> 20));
        }
#endif
    }
    return 0;
}

extern int g_worker_prio, g_zero_mem, g_key_start, g_key_square, g_cross_rclick, g_triangle_presto, g_circle_close, g_cheat_kb, g_cheat_hold_ms, g_help_lr, g_quit_confirm;
static int g_game_prio = 0xA0;     /* lower number = more important; SDL audio runs around 0x70 */
static int g_cpu_mhz = 444;

/* Optional tuning file: ux0:data/insaniquarium/loader.cfg, lines like "game_priority=160". */
int g_skip_intro = 0;      /* loader.cfg: skip_intro=1 -> do not play the video (for testing) */
int g_intro_bgm = 0;       /* loader.cfg: intro_bgm=1 plays the intro audio on the BGM port (experiment) */
int g_intro_audio_buffer = 117;  /* ms; loader.cfg: intro_audio_buffer=NN (100..700). Smaller = video lags audio less, larger = safer against audio dropouts */
int g_intro_volume = 100;   /* percent, loader.cfg: intro_volume=NN */

static void read_cfg(void) {
    FILE *f = fopen(DATA_DIR "/loader.cfg", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[40]; int v;
        if (sscanf(line, " %39[a-z_] = %d", key, &v) != 2) continue;
        if (!strcmp(key, "game_priority") && v >= 64 && v <= 191) g_game_prio = v;
        else if (!strcmp(key, "worker_priority") && v >= 64 && v <= 191) g_worker_prio = v;
        else if (!strcmp(key, "key_start")  && v >= 0 && v < 128) g_key_start = v;
        else if (!strcmp(key, "key_square") && v >= 0 && v < 128) g_key_square = v;
        else if (!strcmp(key, "cross_right_click")) g_cross_rclick = v != 0;
        else if (!strcmp(key, "circle_close")) g_circle_close = v != 0;
        else if (!strcmp(key, "help_lr")) g_help_lr = v != 0;
        else if (!strcmp(key, "quit_confirm")) g_quit_confirm = v != 0;
        else if (!strcmp(key, "triangle_presto")) g_triangle_presto = v != 0;
        else if (!strcmp(key, "cheat_keyboard")) g_cheat_kb = v != 0;
        else if (!strcmp(key, "cheat_hold_ms") && v >= 0 && v <= 10000) g_cheat_hold_ms = v;
        else if (!strcmp(key, "zero_memory")) g_zero_mem = v != 0;
        else if (!strcmp(key, "skip_intro")) g_skip_intro = v != 0;
        else if (!strcmp(key, "intro_bgm")) g_intro_bgm = v != 0;
        else if (!strcmp(key, "intro_audio_buffer") && v >= 100 && v <= 700) g_intro_audio_buffer = v;
        else if (!strcmp(key, "intro_volume") && v >= 0 && v <= 100) g_intro_volume = v;
        else if (!strcmp(key, "cpu_mhz") && v >= 111 && v <= 444) g_cpu_mhz = v;
        else continue;
        LOG("loader.cfg: %s=%d", key, v);
    }
    fclose(f);
}

static void fatal(const char *msg) {
    LOG("FATAL: %s", msg);
    sceKernelExitProcess(0);
}

static int game_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    install_crash_handlers();

    typedef void (*set_wd_t)(void *env, void *thiz, const char *dir);
    typedef int  (*sdl_main_t)(int, char **);

    set_wd_t set_wd = (set_wd_t)so_sym("Java_io_itch_ksylvestre_insaniquariumportable_InsaniquariumPortableActivity_nativeSetWorkingDir");
    sdl_main_t game_main = (sdl_main_t)so_sym("SDL_main");
    if (!set_wd || !game_main) fatal("entry points not found in libmain.so");

    SDL_SetMainReady();
    LOG("calling nativeSetWorkingDir");
    set_wd(&jni_env, NULL, DATA_DIR "/");
    LOG("calling SDL_main");
    static char *argv[] = { (char *)"insaniquarium", NULL };
    int rc = game_main(1, argv);
    LOG("SDL_main returned %d", rc);
    sceKernelExitProcess(0);
    return 0;
}

/* Tiny code patch: the bytes at (symbol + off) must match `expect`, then they are replaced by `repl`.
 * Anything that does not match is left alone, and every outcome is logged. */
static void patch_code(const char *label, const char *sym, unsigned off,
                       const uint8_t *expect, const uint8_t *repl, size_t n) {
    uintptr_t base = so_sym(sym) & ~(uintptr_t)1;
    if (!base) { LOG("patch %s: symbol not found", label); return; }
    uint8_t *p = (uint8_t *)(base + off);
    if (memcmp(p, expect, n)) { LOG("patch %s: NOT applied (code differs from expected)", label); return; }
    memcpy(p, repl, n);
    if (memcmp(p, repl, n)) LOG("patch %s: write did not stick", label);
    else LOG("patch %s: applied", label);
}

/* Everything that touches libmain.so up to (not including) running its code. Returns 0, or
 * 1 = load failed, 2 = relocation failed. Runs while the intro video plays. */
static int so_load_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    if (so_load(SO_PATH, LOAD_BASE)) return 1;
    if (so_link(g_imports, g_imports_count)) return 2;
    /* Quit button: redirect the (empty) DoQuitDialog virtual slot to our handler, which opens a Yes/No
     * dialog; and redirect the DialogButtonDepress slots so we see that dialog's Yes/No result. */
    {
        uintptr_t q = so_sym("_ZN4Sexy10WinFishApp12DoQuitDialogEv");
        uintptr_t bp = so_sym("_ZN4Sexy11SexyAppBase19DialogButtonDepressEii");
        uintptr_t bt = so_sym("_ZThn4_N4Sexy11SexyAppBase19DialogButtonDepressEii");
        int n = 0, np = 0, nt = 0;
        g_dlg_btn_orig_plain = (int)bp;
        g_dlg_btn_orig_thunk = (int)bt;
        for (uintptr_t a = LOAD_BASE + 0x508930; a < LOAD_BASE + 0x5c5800; a += 4) {
            uintptr_t v = *(uintptr_t *)a;
            if (q && v == q) { *(uintptr_t *)a = (uintptr_t)&loader_quit_hook; n++; }
            else if (bp && v == bp) { *(uintptr_t *)a = (uintptr_t)&loader_dlg_btn_hook_plain; np++; }
            else if (bt && v == bt) { *(uintptr_t *)a = (uintptr_t)&loader_dlg_btn_hook_thunk; nt++; }
        }
        LOG("quit hook: DoQuitDialog=%08x, %d vtable slot(s) patched; dialog result hook: %d + %d slot(s)", (unsigned)q, n, np, nt);
    }
    /* Starving guppies die silently: Fish::Hungry() ends a starved fish with Die(false), and Fish::Die(bool)
     * only plays the death sound (Board::PlayDieSound) when its argument is true (bombs, missiles and the
     * Gekko / Oscar / Ultra starvation paths all pass true). Change that one "movs r1,#0" to "movs r1,#1". */
    {
        static const uint8_t expect[6] = { 0x00, 0x21, 0xd0, 0xf8, 0x64, 0x21 };   /* movs r1,#0 ; ldr.w r2,[r0,#0x164] */
        static const uint8_t repl[6]   = { 0x01, 0x21, 0xd0, 0xf8, 0x64, 0x21 };   /* movs r1,#1 ; ldr.w r2,[r0,#0x164] */
        patch_code("starvation death sound", "_ZN4Sexy4Fish6HungryEv", 0x292, expect, repl, sizeof expect);
    }
    /* Breeder death animation: DeadFish::Draw picks the source row for the three breeder types (10..12) as
     * (type - TYPE_BREEDER) * 240, which skips the +160 row offset of the sprite sheet, so a dead breeder is drawn
     * from the "hungry" row. The WinFish source fixes it with "+ 160" (kyle-sylvestre/WinFish commit 87a6ea1).
     * In the binary the constant is "movw r6,#0xf6a0" (-2400, = -10*240); make it 0xf740 (-2240 = -2400 + 160). */
    {
        static const uint8_t expect[4] = { 0x4f, 0xf2, 0xa0, 0x66 };   /* movw r6,#0xf6a0 */
        static const uint8_t repl[4]   = { 0x4f, 0xf2, 0x40, 0x76 };   /* movw r6,#0xf740 */
        patch_code("breeder death sprite row", "_ZN4Sexy8DeadFish4DrawEPNS_8GraphicsE", 0x23e, expect, repl, sizeof expect);
    }
    so_flush();
    return 0;
}

int main(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    log_open();
    LOG("insaniquarium-vita loader v%s (built %s %s)", LOADER_VERSION, __DATE__, __TIME__);

    read_cfg();
    scePowerSetArmClockFrequency(g_cpu_mhz);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    install_crash_handlers();
    for (int i = 0; i < JNI_SLOTS; i++) jni_vtable[i] = (void *)jni_unimplemented;
    jni_vtable[169] = (void *)jni_GetStringUTFChars;
    jni_vtable[170] = (void *)jni_ReleaseStringUTFChars;

    /* Map, relocate and patch libmain.so on a low-priority thread WHILE the PopCap logo plays.
     * No code from the library runs until the video has finished: only the loading is
     * overlapped. If the thread cannot be created, load after the video instead. */
    SceUID lt = sceKernelCreateThread("so_loader", so_load_thread, 0xB0, 1024 * 1024, 0, 0, NULL);
    if (lt >= 0) sceKernelStartThread(lt, 0, NULL);

    /* A missing or unplayable video is logged and skipped so the game still launches. */
    if (g_skip_intro) LOG("intro video skipped (loader.cfg skip_intro=1)");
    else intro_video_play();

    int load_rc = 0;
    if (lt >= 0) { sceKernelWaitThreadEnd(lt, &load_rc, NULL); sceKernelDeleteThread(lt); }
    else load_rc = so_load_thread(0, NULL);
    if (load_rc == 1) fatal("could not load " SO_PATH " - copy libmain.so next to eboot.bin (ux0:app/INSNQ4R13/libmain.so)");
    if (load_rc == 2) fatal("relocation failed");
    LOG("running constructors");
    so_run_init();
    LOG("constructors done");

    /* The game is C++ and wants a big stack; run it on its own thread. */
    SceUID wd = sceKernelCreateThread("watchdog", watchdog_thread, 0xB0, 64 * 1024, 0, 0, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
    SceUID th = sceKernelCreateThread("game", game_thread, g_game_prio, 4 * 1024 * 1024, 0, 0, NULL);
    if (th < 0) fatal("could not create game thread");
    sceKernelStartThread(th, 0, NULL);
    sceKernelWaitThreadEnd(th, NULL, NULL);
    return 0;
}
