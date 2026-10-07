#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* Everything lives in one folder on the Vita memory card. */
#define DATA_DIR   "ux0:data/insaniquarium"
/* libmain.so lives in the app folder, right next to eboot.bin (ux0:app/INSNQ4R13/libmain.so). There is
 * deliberately no fallback location, so "it is not next to eboot.bin" is the only thing to check. */
#define SO_PATH    "app0:libmain.so"
#define LOG_PATH   DATA_DIR "/log.txt"

/* Fixed virtual address the game library is mapped at (page aligned). */
#define LOAD_BASE  0x98000000u

void log_open(void);
void game_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) game_log(__VA_ARGS__)

/* Version shown on the first line of log.txt. CMakeLists.txt passes the real one (same number as the VPK). */
#ifndef LOADER_VERSION
#define LOADER_VERSION "dev"
#endif

/* Verbose diagnostics (per-second playback status, memory/fps heartbeat, file-open trace, pad events).
 * Compiled out unless the loader is built with LOADER_DEBUG=ON (see build.sh / CMakeLists.txt). */
#ifdef LOADER_DEBUG
#define DLOG(...) game_log(__VA_ARGS__)
#else
#define DLOG(...) do { if (0) game_log(__VA_ARGS__); } while (0)   /* compiled out, but still type-checked */
#endif

/* Resolve a (possibly relative) game path against DATA_DIR. */
const char *vpath(const char *p, char *buf, size_t n);
