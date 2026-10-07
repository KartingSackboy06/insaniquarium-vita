/*
 * Bionic (Android libc) -> Vita newlib compatibility layer.
 *
 * Every function named ov_<symbol> is picked up automatically by
 * tools/gen_import_table.py and is bound to the game library's import of
 * <symbol>. Reasons a function needs a wrapper:
 *   - float/double arguments or results: Android armeabi-v7a uses the
 *     soft-float calling convention, the Vita toolchain uses hard-float.
 *     PCS marks a wrapper as "aapcs" (core-register) so both sides agree.
 *   - different struct layouts / constants (stat, dirent, tm, timespec, O_*)
 *   - different pthread object sizes (bionic mutex/cond are 4 bytes)
 *   - Android-only functions (fortify _chk, __sF, __errno, log, ...)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include "common.h"
#include "so_util.h"
#include "intro_video.h"

#if defined(__arm__)
#define PCS __attribute__((pcs("aapcs")))
#else
#define PCS
#endif

/* ------------------------------------------------------------------ log -- */
FILE *g_log;

void log_open(void) { g_log = fopen(LOG_PATH, "w"); }

void game_log(const char *fmt, ...) {
    if (!g_log) return;
    /* One line at a time, so lines from different threads don't interleave.
     * trylock: a crash handler must never wait on a lock held by the dead thread. */
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int locked = pthread_mutex_trylock(&lk) == 0;
    if (!locked) { sceKernelDelayThread(2000); locked = pthread_mutex_trylock(&lk) == 0; }
    /* Format into one buffer and write it with a single call, so a line can never be split
     * by another thread's output (the previous vfprintf + fputc could interleave). */
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, g_log);
    fflush(g_log);
    /* push to the memory card at most once per second so the tail survives a crash */
    static uint64_t last_sync;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - last_sync > 1000000u) { last_sync = now; fsync(fileno(g_log)); }
    if (locked) pthread_mutex_unlock(&lk);
}

/* Trace file access: failed opens always (so a missing data file shows up in log.txt), successful ones only
 * in LOADER_DEBUG builds. */
static void trace_open(const char *kind, const char *path, int ok) {
#ifndef LOADER_DEBUG
    if (ok) return;                                  /* release builds log only the files that were NOT found */
#endif
    static int n_ok, n_fail;
    static char last[300];
    if (ok ? (n_ok >= 6000) : (n_fail >= 40)) return;
    char line[300]; snprintf(line, sizeof line, "%s(%s) %d", kind, path ? path : "(null)", ok);
    if (!strcmp(line, last)) return;                 /* the engine asks twice per file */
    snprintf(last, sizeof last, "%s", line);
    if (ok) n_ok++; else n_fail++;
    LOG("%s(%s) -> %s", kind, path ? path : "(null)", ok ? "ok" : "FAIL");
}

/* ---------------------------------------------------------------- paths -- */
static char g_cwd[512] = DATA_DIR;
static void dir_dirty(void);
static int known_missing(const char *rp);
static pthread_mutex_t g_dc_lk = PTHREAD_MUTEX_INITIALIZER;

/* Relative paths are resolved against the game's data folder. Paths that
 * already carry a device prefix ("ux0:...") or start with '/' are left alone. */
const char *vpath(const char *p, char *buf, size_t n) {
    if (!p) return p;
    char t[512];
    /* If a device prefix shows up anywhere (e.g. "/ux0:data/x" or
     * "ux0:data/insaniquarium/ux0:data/insaniquarium/x"), keep only the last one. */
    const char *last = NULL, *q = p;
    while ((q = strstr(q, "ux0:")) != NULL) { last = q; q += 4; }
    if (last) p = last;
    const char *c = strchr(p, ':'), *s = strchr(p, '/');
    if (c && (!s || c < s)) {
        snprintf(t, sizeof t, "%s", p);               /* already absolute on a device */
    } else if (p[0] == '/') {
        return p;                                      /* host-absolute (/dev/..., etc.) */
    } else {
        while (p[0] == '.' && p[1] == '/') p += 2;
        snprintf(t, sizeof t, "%s/%s", g_cwd, p);
    }
    /* collapse "//" (keeping the one after nothing special) */
    size_t o = 0;
    for (size_t i = 0; t[i] && o + 1 < n; i++) {
        if (t[i] == '/' && o > 0 && buf[o - 1] == '/') continue;
        buf[o++] = t[i];
    }
    buf[o] = 0;
    return buf;
}

char *ov_getcwd(char *buf, size_t n) {
    if (!buf || strlen(g_cwd) + 1 > n) { errno = ERANGE; return NULL; }
    strcpy(buf, g_cwd);
    return buf;
}
int ov_chdir(const char *p) {
    char b[512]; p = vpath(p, b, sizeof b);
    snprintf(g_cwd, sizeof g_cwd, "%s", p);
    size_t l = strlen(g_cwd);
    while (l > 1 && g_cwd[l - 1] == '/') g_cwd[--l] = 0;
    return 0;
}
char *ov_realpath(const char *p, char *out) {
    char b[512]; p = vpath(p, b, sizeof b);
    if (!out) out = malloc(512);
    snprintf(out, 512, "%s", p);
    return out;
}

/* ------------------------------------------------------------ bionic data -- */
/* sizeof(FILE) is 84 bytes on 32-bit bionic; the game takes &__sF[1] etc. */
char ov___sF[3 * 84] __attribute__((aligned(8)));
uint32_t ov___stack_chk_guard = 0xA5C3E17Du;

static FILE *F(FILE *f) {
    uintptr_t a = (uintptr_t)f, b = (uintptr_t)ov___sF;
    if (a >= b && a < b + sizeof ov___sF)
        return ((a - b) / 84 == 0) ? stdin : (g_log ? g_log : stderr);  /* stdout/stderr -> log.txt */
    return f;
}

void ov___stack_chk_fail(void) { LOG("stack smashing detected"); abort(); }
int *ov___errno(void) { return &errno; }
size_t ov___ctype_get_mb_cur_max(void) { return 1; }
void ov_android_set_abort_message(const char *m) { LOG("abort message: %s", m ? m : "(null)"); }
int ov___android_log_write(int prio, const char *tag, const char *text) {
    LOG("[%d] %s: %s", prio, tag ? tag : "", text ? text : "");
    return 0;
}

/* ---------------------------------------------------------- fortify _chk -- */
void *ov___memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
char *ov___strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
char *ov___strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
char *ov___strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
size_t ov___strlen_chk(const char *s, size_t sl) { (void)sl; return strlen(s); }
char *ov___strchr_chk(const char *s, int c, size_t sl) { (void)sl; return strchr(s, c); }
#define FAKE_RANDOM_FD 0x7E57
static int g_rand_seeded;
static void fill_random(void *b, size_t n) {
    if (!g_rand_seeded) { g_rand_seeded = 1; srand((unsigned)sceKernelGetProcessTimeWide()); }
    uint8_t *p = b;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rand() >> 7);
}
ssize_t ov_read(int fd, void *b, size_t n) {
    if (fd == FAKE_RANDOM_FD) { fill_random(b, n); return (ssize_t)n; }
    return read(fd, b, n);
}
int ov_close(int fd) { return fd == FAKE_RANDOM_FD ? 0 : close(fd); }
ssize_t ov___read_chk(int fd, void *b, size_t n, size_t bl) { (void)bl; return ov_read(fd, b, n); }
int ov___vsnprintf_chk(char *b, size_t m, int fl, size_t sl, const char *f, va_list ap) { (void)fl; (void)sl; return vsnprintf(b, m, f, ap); }
int ov___vsprintf_chk(char *b, int fl, size_t sl, const char *f, va_list ap) { (void)fl; (void)sl; return vsprintf(b, f, ap); }

/* ----------------------------------------------------------------- stdio -- */
FILE *ov_fopen(const char *p, const char *m) {
    char b[512]; const char *rp = vpath(p, b, sizeof b);
    if (m && m[0] == 'r' && known_missing(rp)) { errno = ENOENT; return NULL; }
    if (m && m[0] != 'r') dir_dirty();
    FILE *f = fopen(rp, m);
    trace_open("fopen", rp, f != NULL);
    return f;
}
int ov_fclose(FILE *f) { return fclose(F(f)); }
int ov_feof(FILE *f) { return feof(F(f)); }
int ov_ferror(FILE *f) { return ferror(F(f)); }
int ov_fflush(FILE *f) { return fflush(F(f)); }
int ov_fgetc(FILE *f) { return fgetc(F(f)); }
int ov_getc(FILE *f) { return fgetc(F(f)); }
int ov_ungetc(int c, FILE *f) { return ungetc(c, F(f)); }
int ov_fputc(int c, FILE *f) { return fputc(c, F(f)); }
int ov_fputs(const char *s, FILE *f) { return fputs(s, F(f)); }
char *ov_fgets(char *b, int n, FILE *f) { return fgets(b, n, F(f)); }
size_t ov_fread(void *b, size_t s, size_t n, FILE *f) { return fread(b, s, n, F(f)); }
size_t ov_fwrite(const void *b, size_t s, size_t n, FILE *f) { return fwrite(b, s, n, F(f)); }
int ov_fseek(FILE *f, long o, int w) { return fseek(F(f), o, w); }
long ov_ftell(FILE *f) { return ftell(F(f)); }
int ov_fseeko(FILE *f, int32_t o, int w) { return fseeko(F(f), (off_t)o, w); }
int32_t ov_ftello(FILE *f) { return (int32_t)ftello(F(f)); }
void ov_setbuf(FILE *f, char *b) { setbuf(F(f), b); }
wint_t ov_fputwc(wchar_t c, FILE *f) { return fputwc(c, F(f)); }
wint_t ov_getwc(FILE *f) { return getwc(F(f)); }
wint_t ov_ungetwc(wint_t c, FILE *f) { return ungetwc(c, F(f)); }
int ov_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(F(f), fmt, ap); }
int ov_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(F(f), fmt, ap);
    va_end(ap);
    return r;
}
int ov_fscanf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(F(f), fmt, ap);
    va_end(ap);
    return r;
}
int ov_remove(const char *p) { dir_dirty(); char b[512]; return remove(vpath(p, b, sizeof b)); }
int ov_rename(const char *a, const char *b) { dir_dirty(); char x[512], y[512]; return rename(vpath(a, x, sizeof x), vpath(b, y, sizeof y)); }

/* ------------------------------------------------------- file descriptors -- */
int ov_open(const char *p, int fl, ...) {
    va_list ap; va_start(ap, fl);
    int mode = va_arg(ap, int);
    va_end(ap);
    char b[512];
    int f = 0;
    switch (fl & 3) { case 0: f = O_RDONLY; break; case 1: f = O_WRONLY; break; default: f = O_RDWR; }
    if (fl & 0000100) f |= O_CREAT;      /* bionic ARM flag values */
    if (fl & 0000200) f |= O_EXCL;
    if (fl & 0001000) f |= O_TRUNC;
    if (fl & 0002000) f |= O_APPEND;
    if (p && (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random"))) return FAKE_RANDOM_FD;
    if (f & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) dir_dirty();
    const char *rp = vpath(p, b, sizeof b);
    if (!(f & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) && known_missing(rp)) { errno = ENOENT; return -1; }
    int r = open(rp, f, mode);
    trace_open("open", rp, r >= 0);
    return r;
}
int32_t ov_lseek(int fd, int32_t o, int w) { return (int32_t)lseek(fd, (off_t)o, w); }
int64_t ov_lseek64(int fd, int64_t o, int w) { return (int64_t)lseek(fd, (off_t)o, w); }
int ov_mkdir(const char *p, uint32_t m) { dir_dirty(); char b[512]; return mkdir(vpath(p, b, sizeof b), m); }

/* bionic 32-bit ARM struct stat */
struct bstat {
    uint64_t st_dev; uint8_t pad0[4]; uint32_t st_ino32; uint32_t st_mode; uint32_t st_nlink;
    uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint8_t pad3[4]; int64_t st_size;
    uint32_t st_blksize; uint64_t st_blocks;
    uint32_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint64_t st_ino;
};
static void conv_stat(const struct stat *s, struct bstat *d) {
    memset(d, 0, sizeof *d);
    d->st_dev = s->st_dev; d->st_ino32 = (uint32_t)s->st_ino; d->st_ino = s->st_ino;
    d->st_mode = s->st_mode; d->st_nlink = s->st_nlink ? s->st_nlink : 1;
    d->st_uid = s->st_uid; d->st_gid = s->st_gid; d->st_size = s->st_size;
    d->st_blksize = 4096; d->st_blocks = ((uint64_t)s->st_size + 511) / 512;
    d->atime = (uint32_t)s->st_atime; d->mtime = (uint32_t)s->st_mtime; d->ctime = (uint32_t)s->st_ctime;
}
int ov_stat(const char *p, struct bstat *o) {
    char b[512]; struct stat s;
    const char *rp = vpath(p, b, sizeof b);
    if (known_missing(rp)) { errno = ENOENT; return -1; }
    int r = stat(rp, &s);
    if (r == 0) conv_stat(&s, o); else trace_open("stat", rp, 0);
    return r;
}
int ov_lstat(const char *p, struct bstat *o) { return ov_stat(p, o); }
int ov_fstat(int fd, struct bstat *o) {
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0) conv_stat(&s, o);
    return r;
}

struct bdirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };

/* Directory listings are cached: the engine lists images/ once per image it
 * tries to load (and for every extension variant), which is very slow on the
 * memory card. Any write/rename/remove/mkdir bumps g_dir_gen and invalidates. */
#define DC_MAX 24
struct dsnap { int ref, n; char (*names)[256]; };
struct dcache { char path[256]; unsigned gen; struct dsnap *s; };
static void dsnap_put(struct dsnap *p) { if (p && --p->ref == 0) { free(p->names); free(p); } }
struct dhandle { uint32_t magic; int idx; struct dsnap *s; };
#define DH_MAGIC 0xD1C0FFEEu
static struct dcache g_dc[DC_MAX];
static unsigned g_dir_gen = 1;
static void dir_dirty(void) { g_dir_gen++; }

static struct dcache *dc_get(const char *rp) {
    struct dcache *slot = NULL;
    for (int i = 0; i < DC_MAX; i++) {
        if (g_dc[i].path[0] && !strcmp(g_dc[i].path, rp)) { if (g_dc[i].gen == g_dir_gen) return &g_dc[i]; slot = &g_dc[i]; break; }
    }
    DIR *d = opendir(rp);
    if (!d) return NULL;
    if (!slot) {
        static int rr;
        for (int i = 0; i < DC_MAX; i++) if (!g_dc[i].path[0]) { slot = &g_dc[i]; break; }
        if (!slot) slot = &g_dc[rr++ % DC_MAX];
    }
    dsnap_put(slot->s);
    struct dsnap *sn = calloc(1, sizeof *sn); sn->ref = 1;
    slot->s = sn;
    int cap = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (sn->n == cap) { cap = cap ? cap * 2 : 128; sn->names = realloc(sn->names, (size_t)cap * 256); }
        snprintf(sn->names[sn->n++], 256, "%s", e->d_name);
    }
    closedir(d);
    snprintf(slot->path, sizeof slot->path, "%s", rp);
    slot->gen = g_dir_gen;
    return slot;
}
/* true only if the parent folder is listed and the name is not in it */
static int known_missing(const char *rp) {
    size_t dl = strlen(DATA_DIR);
    if (strncmp(rp, DATA_DIR "/", dl + 1)) return 0;
    const char *slash = strrchr(rp, '/');
    if (!slash || !slash[1]) return 0;
    char dir[256];
    size_t n = (size_t)(slash - rp);
    if (n >= sizeof dir) return 0;
    memcpy(dir, rp, n); dir[n] = 0;
    pthread_mutex_lock(&g_dc_lk);
    struct dcache *c = dc_get(dir);
    int missing = (c != NULL);
    if (c) for (int i = 0; i < c->s->n; i++) if (!strcmp(c->s->names[i], slash + 1)) { missing = 0; break; }
    pthread_mutex_unlock(&g_dc_lk);
    return missing;
}
DIR *ov_opendir(const char *p) {
    char b[512]; const char *rp = vpath(p, b, sizeof b);
    pthread_mutex_lock(&g_dc_lk);
    struct dcache *c = dc_get(rp);
    struct dhandle *h = NULL;
    if (c) {
        h = malloc(sizeof *h);
        h->magic = DH_MAGIC; h->idx = 0; h->s = c->s; h->s->ref++;
    }
    pthread_mutex_unlock(&g_dc_lk);
    if (!h) { trace_open("opendir", rp, 0); errno = ENOENT; return NULL; }
    return (DIR *)h;
}
int ov_closedir(DIR *d) {
    struct dhandle *h = (struct dhandle *)d;
    if (h && h->magic == DH_MAGIC) { h->magic = 0; dsnap_put(h->s); free(h); }
    return 0;
}
struct bdirent *ov_readdir(DIR *d) {
    static __thread struct bdirent bd;
    struct dhandle *h = (struct dhandle *)d;
    if (!h || h->magic != DH_MAGIC || h->idx >= h->s->n) return NULL;
    memset(&bd, 0, sizeof bd);
    bd.d_ino = 1; bd.d_reclen = sizeof bd; bd.d_type = 0; /* DT_UNKNOWN */
    snprintf(bd.d_name, sizeof bd.d_name, "%s", h->s->names[h->idx++]);
    return &bd;
}

/* Rarely/never used by the game: report "not implemented". */
#define STUB_NEG(n) int ov_##n(void) { errno = ENOSYS; return -1; }
#define STUB_NIL(n) void *ov_##n(void) { errno = ENOSYS; return NULL; }
#define STUB_OK(n)  int ov_##n(void) { return 0; }
STUB_NEG(link) STUB_NEG(symlink) STUB_NEG(readlink) STUB_NEG(sendfile) STUB_NEG(utimensat)
STUB_NEG(fchmod) STUB_NEG(fchmodat) STUB_NEG(unlinkat) STUB_NEG(openat) STUB_NEG(statvfs)
STUB_NEG(truncate) STUB_NEG(ftruncate) STUB_NEG(ioctl) STUB_NEG(pathconf)
STUB_NIL(fdopen) STUB_NIL(fdopendir)
STUB_OK(sigaction) STUB_OK(sigemptyset) STUB_OK(syslog) STUB_OK(openlog) STUB_OK(closelog)

int ov_strerror_r(int e, char *b, size_t n) { snprintf(b, n, "error %d", e); return 0; }
long ov_sysconf(int name) {
    switch (name) {
    case 0x27: case 0x28: return 4096;   /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 0x60: case 0x61: return 4;      /* _SC_NPROCESSORS_CONF / ONLN */
    default: return -1;
    }
}

/* ----------------------------------------------------------------- time -- */
struct bts { int32_t sec, nsec; };   /* bionic 32-bit timespec */

int ov_clock_gettime(int clk, struct bts *ts) {
    uint64_t us = sceKernelGetProcessTimeWide();
    if (clk == 0) { ts->sec = (int32_t)time(NULL); ts->nsec = (int32_t)((us % 1000000u) * 1000u); }
    else          { ts->sec = (int32_t)(us / 1000000u); ts->nsec = (int32_t)((us % 1000000u) * 1000u); }
    return 0;
}
int ov_nanosleep(const struct bts *req, struct bts *rem) {
    uint64_t us = (uint64_t)req->sec * 1000000u + (uint64_t)req->nsec / 1000u;
    sceKernelDelayThread((SceUInt32)(us ? us : 1));
    if (rem) rem->sec = rem->nsec = 0;
    return 0;
}

struct btm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; const char *tm_zone; };
static void tm_to_b(const struct tm *s, struct btm *d) {
    memset(d, 0, sizeof *d);
    d->tm_sec = s->tm_sec; d->tm_min = s->tm_min; d->tm_hour = s->tm_hour; d->tm_mday = s->tm_mday;
    d->tm_mon = s->tm_mon; d->tm_year = s->tm_year; d->tm_wday = s->tm_wday; d->tm_yday = s->tm_yday;
    d->tm_isdst = s->tm_isdst; d->tm_zone = "UTC";
}
static void tm_from_b(const struct btm *s, struct tm *d) {
    memset(d, 0, sizeof *d);
    d->tm_sec = s->tm_sec; d->tm_min = s->tm_min; d->tm_hour = s->tm_hour; d->tm_mday = s->tm_mday;
    d->tm_mon = s->tm_mon; d->tm_year = s->tm_year; d->tm_wday = s->tm_wday; d->tm_yday = s->tm_yday;
    d->tm_isdst = s->tm_isdst;
}
struct btm *ov_localtime64_r(const int64_t *t, struct btm *out) {
    time_t tt = (time_t)*t; struct tm r;
    if (!localtime_r(&tt, &r)) return NULL;
    tm_to_b(&r, out);
    return out;
}
struct btm *ov_gmtime(const int32_t *t) {
    static struct btm b; time_t tt = (time_t)*t; struct tm r;
    if (!gmtime_r(&tt, &r)) return NULL;
    tm_to_b(&r, &b);
    return &b;
}
char *ov_asctime(const struct btm *b) { struct tm t; tm_from_b(b, &t); return asctime(&t); }
size_t ov_strftime(char *s, size_t n, const char *f, const struct btm *b) { struct tm t; tm_from_b(b, &t); return strftime(s, n, f, &t); }
size_t ov_strftime_l(char *s, size_t n, const char *f, const struct btm *b, void *l) { (void)l; return ov_strftime(s, n, f, b); }

/* --------------------------------------------------- locale (C locale only) -- */
void *ov_newlocale(int mask, const char *loc, void *base) { (void)mask; (void)loc; (void)base; return (void *)1; }
void *ov_uselocale(void *l) { (void)l; return (void *)1; }
void ov_freelocale(void *l) { (void)l; }
#define ISW(n) int ov_##n##_l(wint_t c, void *l) { (void)l; return n(c); }
ISW(iswalpha) ISW(iswblank) ISW(iswcntrl) ISW(iswdigit) ISW(iswlower)
ISW(iswprint) ISW(iswpunct) ISW(iswspace) ISW(iswupper) ISW(iswxdigit)
wint_t ov_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
wint_t ov_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
int ov_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
size_t ov_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
int ov_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
size_t ov_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
long long ov_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
unsigned long long ov_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
PCS double ov_strtold_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }  /* long double == double on ARM */

/* ------------------------------------------------- float/double (soft ABI) -- */
#define F1(n)  PCS float  ov_##n(float x)  { return n(x); }
#define D1(n)  PCS double ov_##n(double x) { return n(x); }
F1(sinf) F1(cosf) F1(tanf) F1(sinhf) F1(logf) F1(log10f) F1(exp2f) F1(floorf) F1(ceilf) F1(roundf) F1(rintf)
D1(sin) D1(cos) D1(tan) D1(acos) D1(atan) D1(log) D1(log10) D1(log2) D1(exp) D1(exp2) D1(floor) D1(ceil) D1(round) D1(rint)
PCS float  ov_powf(float a, float b)    { return powf(a, b); }
PCS double ov_pow(double a, double b)   { return pow(a, b); }
PCS double ov_hypot(double a, double b) { return hypot(a, b); }
PCS double ov_frexp(double x, int *e)   { return frexp(x, e); }
PCS double ov_ldexp(double x, int e)    { return ldexp(x, e); }
PCS double ov_modf(double x, double *i) { return modf(x, i); }
PCS void   ov_sincos(double x, double *s, double *c)  { *s = sin(x); *c = cos(x); }
PCS void   ov_sincosf(float x, float *s, float *c)    { *s = sinf(x); *c = cosf(x); }
PCS double ov_atof(const char *s)                     { return atof(s); }
PCS double ov_strtod(const char *s, char **e)         { return strtod(s, e); }
PCS float  ov_strtof(const char *s, char **e)         { return strtof(s, e); }
PCS double ov_strtold(const char *s, char **e)        { return strtod(s, e); }
PCS double ov_wcstod(const wchar_t *s, wchar_t **e)   { return wcstod(s, e); }
PCS float  ov_wcstof(const wchar_t *s, wchar_t **e)   { return wcstof(s, e); }
PCS double ov_wcstold(const wchar_t *s, wchar_t **e)  { return wcstod(s, e); }

/* -------------------------------------------------------- SDL overrides -- */
extern void *SDL_RWFromFile(const char *file, const char *mode);
extern double SDL_atof(const char *s);
PCS double ov_SDL_atof(const char *s) { return SDL_atof(s); }
const char *ov_SDL_AndroidGetExternalStoragePath(void) { return DATA_DIR; }
void *ov_SDL_RWFromFile(const char *f, const char *m) {
    char b[512]; const char *rp = vpath(f, b, sizeof b);
    void *r = SDL_RWFromFile(rp, m);
    trace_open("SDL_RWFromFile", rp, r != NULL);
    return r;
}

/* ------------------------------------------------------------- pthreads -- */
/* bionic pthread objects are 4 bytes; we keep a heap-allocated host object
 * and store its pointer in that 4-byte slot. Values < 0x10000 mean "static
 * initializer, not created yet" (0 = normal, 0x4000 = recursive). */
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
#define UNINIT(v) ((v) < 0x10000u)

static pthread_mutex_t *mtx_new(int rec) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    if (rec) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
static pthread_mutex_t *mtx(uint32_t *o) {
    if (UNINIT(*o)) {
        pthread_mutex_lock(&g_lk);
        if (UNINIT(*o)) *o = (uint32_t)(uintptr_t)mtx_new((*o & 0x4000) != 0);
        pthread_mutex_unlock(&g_lk);
    }
    return (pthread_mutex_t *)(uintptr_t)*o;
}
static pthread_cond_t *cnd(uint32_t *o) {
    if (UNINIT(*o)) {
        pthread_mutex_lock(&g_lk);
        if (UNINIT(*o)) { pthread_cond_t *c = malloc(sizeof *c); pthread_cond_init(c, NULL); *o = (uint32_t)(uintptr_t)c; }
        pthread_mutex_unlock(&g_lk);
    }
    return (pthread_cond_t *)(uintptr_t)*o;
}

int ov_pthread_mutexattr_init(uint32_t *a) { *a = 0; return 0; }
int ov_pthread_mutexattr_destroy(uint32_t *a) { (void)a; return 0; }
int ov_pthread_mutexattr_settype(uint32_t *a, int t) { *a = (uint32_t)t; return 0; }
int ov_pthread_mutex_init(uint32_t *o, const uint32_t *attr) {
    *o = (attr && *attr == 1) ? 0x4000u : 0u;     /* bionic RECURSIVE == 1 */
    (void)mtx(o);
    return 0;
}
int ov_pthread_mutex_destroy(uint32_t *o) { if (!UNINIT(*o)) { pthread_mutex_destroy(mtx(o)); free((void *)(uintptr_t)*o); *o = 0; } return 0; }
int ov_pthread_mutex_lock(uint32_t *o) { return pthread_mutex_lock(mtx(o)); }
int ov_pthread_mutex_trylock(uint32_t *o) { return pthread_mutex_trylock(mtx(o)); }
int ov_pthread_mutex_unlock(uint32_t *o) { return pthread_mutex_unlock(mtx(o)); }

int ov_pthread_cond_destroy(uint32_t *o) { if (!UNINIT(*o)) { pthread_cond_destroy(cnd(o)); free((void *)(uintptr_t)*o); *o = 0; } return 0; }
int ov_pthread_cond_signal(uint32_t *c) { return pthread_cond_signal(cnd(c)); }
int ov_pthread_cond_broadcast(uint32_t *c) { return pthread_cond_broadcast(cnd(c)); }
int ov_pthread_cond_wait(uint32_t *c, uint32_t *m) { return pthread_cond_wait(cnd(c), mtx(m)); }
int ov_pthread_cond_timedwait(uint32_t *c, uint32_t *m, const struct bts *abs) {
    struct bts now; ov_clock_gettime(0, &now);                 /* the game's idea of "now" */
    int64_t d = ((int64_t)abs->sec - now.sec) * 1000000000LL + ((int64_t)abs->nsec - now.nsec);
    if (d < 0) d = 0;
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);      /* host clock + the same delta */
    int64_t ns = t.tv_nsec + d % 1000000000LL;
    t.tv_sec += d / 1000000000LL + ns / 1000000000LL; t.tv_nsec = ns % 1000000000LL;
    int r = pthread_cond_timedwait(cnd(c), mtx(m), &t);
    return r == ETIMEDOUT ? 110 : r;                           /* bionic ETIMEDOUT */
}

#define MAX_TH 256
static pthread_t g_th[MAX_TH];
static uint8_t g_th_used[MAX_TH], g_th_done[MAX_TH], g_th_detached[MAX_TH];
static pthread_key_t g_self_key; static pthread_once_t g_self_once = PTHREAD_ONCE_INIT;
static void self_init(void) { pthread_key_create(&g_self_key, NULL); }
struct tramp { void *(*fn)(void *); void *arg; uint32_t id; };
static void th_release(uint32_t id) { g_th_used[id] = g_th_done[id] = g_th_detached[id] = 0; }
int g_worker_prio = 0x80;      /* set from main.c / loader.cfg */
static void *tramp_fn(void *p) {
    struct tramp t = *(struct tramp *)p; free(p);
    /* Game worker threads (music decoding etc.) must outrank the loading thread. */
    sceKernelChangeThreadPriority(0, g_worker_prio);
    pthread_setspecific(g_self_key, (void *)(uintptr_t)t.id);
    void *r = t.fn(t.arg);
    pthread_mutex_lock(&g_lk);
    g_th_done[t.id] = 1;
    if (g_th_detached[t.id]) th_release(t.id);      /* nobody will join: recycle the slot */
    pthread_mutex_unlock(&g_lk);
    return r;
}
int ov_pthread_create(uint32_t *out, const void *attr, void *(*fn)(void *), void *arg) {
    pthread_once(&g_self_once, self_init);
    /* bionic pthread_attr_t: { u32 flags; void *stack_base; size_t stack_size; ... } */
    int detached = attr ? (((const uint32_t *)attr)[0] & 1) : 0;
    size_t ss = attr ? ((const uint32_t *)attr)[2] : 0;
    if (ss < 1024 * 1024) ss = 1024 * 1024;
    if (ss > 4 * 1024 * 1024) ss = 4 * 1024 * 1024;
    pthread_mutex_lock(&g_lk);
    uint32_t id = 0;
    for (uint32_t i = 1; i < MAX_TH; i++) if (!g_th_used[i]) { id = i; g_th_used[i] = 1; g_th_done[i] = 0; g_th_detached[i] = (uint8_t)detached; break; }
    pthread_mutex_unlock(&g_lk);
    if (!id) { LOG("pthread_create: out of thread slots"); return EAGAIN; }
    struct tramp *t = malloc(sizeof *t); t->fn = fn; t->arg = arg; t->id = id;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, ss);
    int r = pthread_create(&g_th[id], &a, tramp_fn, t);
    pthread_attr_destroy(&a);
    if (r) { free(t); pthread_mutex_lock(&g_lk); th_release(id); pthread_mutex_unlock(&g_lk); LOG("pthread_create failed: %d (stack %u)", r, (unsigned)ss); return r; }
    *out = id;
    return 0;
}
int ov_pthread_join(uint32_t id, void **ret) {
    if (id >= MAX_TH || !g_th_used[id]) return ESRCH;
    int r = pthread_join(g_th[id], ret);
    pthread_mutex_lock(&g_lk); th_release(id); pthread_mutex_unlock(&g_lk);
    return r;
}
int ov_pthread_detach(uint32_t id) {
    if (id >= MAX_TH || !g_th_used[id]) return ESRCH;
    int r = pthread_detach(g_th[id]);
    pthread_mutex_lock(&g_lk);
    if (g_th_done[id]) th_release(id); else g_th_detached[id] = 1;
    pthread_mutex_unlock(&g_lk);
    return r;
}
uint32_t ov_pthread_self(void) {
    pthread_once(&g_self_once, self_init);
    void *v = pthread_getspecific(g_self_key);
    if (!v) {
        static uint32_t other = 1000;
        pthread_mutex_lock(&g_lk); v = (void *)(uintptr_t)other++; pthread_mutex_unlock(&g_lk);
        pthread_setspecific(g_self_key, v);
    }
    return (uint32_t)(uintptr_t)v;
}

static pthread_key_t g_keys[128]; static int g_nkeys;
int ov_pthread_key_create(uint32_t *k, void (*dtor)(void *)) {
    pthread_mutex_lock(&g_lk);
    int i = g_nkeys < 128 ? g_nkeys++ : -1;
    pthread_mutex_unlock(&g_lk);
    if (i < 0) return EAGAIN;
    int r = pthread_key_create(&g_keys[i], dtor);
    *k = (uint32_t)i;
    return r;
}
void *ov_pthread_getspecific(uint32_t k) { return k < 128 ? pthread_getspecific(g_keys[k]) : NULL; }
int ov_pthread_setspecific(uint32_t k, const void *v) { return k < 128 ? pthread_setspecific(g_keys[k], v) : EINVAL; }

/* Emulated TLS (the game library uses thread_local; libgcc's emutls ABI). */
struct emu_obj { uint32_t size, align, index; void *templ; };
static pthread_key_t g_emu_key; static pthread_once_t g_emu_once = PTHREAD_ONCE_INIT;
static void emu_init(void) { pthread_key_create(&g_emu_key, NULL); }
void *ov___emutls_get_address(struct emu_obj *o) {
    static uint32_t next = 1;
    pthread_once(&g_emu_once, emu_init);
    void **tab = pthread_getspecific(g_emu_key);
    if (!tab) { tab = calloc(256, sizeof(void *)); pthread_setspecific(g_emu_key, tab); }
    if (!o->index) {
        pthread_mutex_lock(&g_lk);
        if (!o->index) o->index = next++;
        pthread_mutex_unlock(&g_lk);
    }
    if (o->index >= 256) { LOG("emutls: too many objects"); abort(); }
    if (!tab[o->index]) {
        uint32_t al = o->align ? o->align : 4;
        uint8_t *raw = calloc(1, o->size + al + sizeof(void *));
        uint8_t *p = (uint8_t *)(((uintptr_t)raw + al - 1) & ~(uintptr_t)(al - 1));
        if (o->templ) memcpy(p, o->templ, o->size);
        tab[o->index] = p;
    }
    return tab[o->index];
}

/* ----------------------------------- C++ runtime pieces the Vita SDK lacks -- */
/* These constructors are inline-only in libstdc++, so there is nothing to link
 * against: just install the vtable pointer (Itanium ABI: vtable + 8 bytes). */
extern char _ZTVSt9bad_alloc[], _ZTVSt8bad_cast[], _ZTVSt20bad_array_new_length[];
void ov__ZNSt9bad_allocC1Ev(void **self)            { *self = _ZTVSt9bad_alloc + 8; }
void ov__ZNSt8bad_castC1Ev(void **self)             { *self = _ZTVSt8bad_cast + 8; }
void ov__ZNSt20bad_array_new_lengthC1Ev(void **self) { *self = _ZTVSt20bad_array_new_length + 8; }

/* std::exception_ptr support is not in this libstdc++. Only needed if the game
 * stores/rethrows exceptions across threads (std::current_exception etc). */
void *ov___cxa_current_primary_exception(void) { LOG("std::current_exception() called - unsupported"); return NULL; }
void ov___cxa_increment_exception_refcount(void *p) { (void)p; }
void ov___cxa_decrement_exception_refcount(void *p) { (void)p; }
void ov___cxa_rethrow_primary_exception(void *p) { (void)p; LOG("std::rethrow_exception() called - unsupported"); abort(); }
int  ov___cxa_uncaught_exceptions(void) { return 0; }

/* ------------------------------------------------ diagnostics for the log -- */
extern int SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *msg, void *win);
extern int SDL_GL_SetAttribute(int attr, int value);
extern void *SDL_CreateWindow(const char *t, int x, int y, int w, int h, uint32_t flags);
extern void *SDL_GL_CreateContext(void *window);
extern const char *SDL_GetError(void);
extern const unsigned char *glGetString(unsigned name);

/* Missing game files: show the system's own message dialog with an OK button, then close the game.
 * Called on the game thread while vitaGL's context is current; vglSwapBuffers(GL_TRUE) is what lets vitaGL draw
 * the system dialog on top of the (cleared) frame. */
extern void vglSwapBuffers(unsigned char has_commondialog);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned mask);
static void show_missing_assets_dialog(void) {
    static const char text[] =
        "Copy your Insaniquarium! Deluxe assets into the \"ux0:data/insaniquarium/\" folder to continue.";
    SceMsgDialogParam param;
    SceMsgDialogUserMessageParam um;
    sceMsgDialogParamInit(&param);
    memset(&um, 0, sizeof um);
    um.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    um.msg = (const SceChar8 *)text;
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &um;
    int r = sceMsgDialogInit(&param);
    if (r < 0) { LOG("assets dialog: sceMsgDialogInit failed 0x%08x", (unsigned)r); return; }
    LOG("assets dialog: shown, waiting for OK");
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(0x4000);                          /* GL_COLOR_BUFFER_BIT */
        vglSwapBuffers(1);
    }
    sceMsgDialogTerm();
    LOG("assets dialog: closed");
}

int ov_SDL_ShowSimpleMessageBox(uint32_t f, const char *t, const char *m, void *w) {
    LOG("MESSAGE BOX [%s]: %s", t ? t : "", m ? m : "");
    if (t && strstr(t, "FATAL")) {
        LOG("fatal error reported by the game; exiting cleanly");
        if (m && strstr(m, "not found")) show_missing_assets_dialog();   /* "Resource file not found: ..." */
        if (g_log) { fflush(g_log); fsync(fileno(g_log)); }
        sceKernelDelayThread(300 * 1000);
        sceKernelExitProcess(0);
    }
    return SDL_ShowSimpleMessageBox(f, t, m, w);
}
int ov_SDL_GL_SetAttribute(int a, int v) {
    int r = SDL_GL_SetAttribute(a, v);
    LOG("SDL_GL_SetAttribute(%d, %d) -> %d", a, v, r);
    return r;
}
static void *g_window;
void *ov_SDL_CreateWindow(const char *t, int x, int y, int w, int h, uint32_t fl) {
    void *r = SDL_CreateWindow(t, x, y, w, h, fl);
    g_window = r;
    LOG("SDL_CreateWindow(\"%s\", %d,%d, %dx%d, flags=0x%x) -> %p %s", t ? t : "", x, y, w, h, fl, r, r ? "" : SDL_GetError());
    return r;
}
/* Frame statistics for the heartbeat line. */
extern void SDL_GL_SwapWindow(void *window);
volatile unsigned g_frames, g_max_dt_us;
volatile int g_gl_ready;
volatile uint64_t g_swap_stamp, g_poll_stamp;
void ov_SDL_GL_SwapWindow(void *w) {
    static uint64_t last;
    uint64_t now = sceKernelGetProcessTimeWide();
    g_swap_stamp = now;
    if (last) { unsigned dt = (unsigned)(now - last); if (dt > g_max_dt_us) g_max_dt_us = dt; }
    last = now;
    g_frames++;
    SDL_GL_SwapWindow(w);
    /* vitaGL owns the screen from here: drop the black frame the intro video left up. */
    static int swaps;
    if (swaps < 2 && ++swaps == 2) intro_video_release();
}
void *ov_SDL_GL_CreateContext(void *win) {
    void *c = SDL_GL_CreateContext(win);
    LOG("SDL_GL_CreateContext -> %p %s", c, c ? "" : SDL_GetError());
    if (c) {
        const unsigned char *v = glGetString(0x1F02), *r = glGetString(0x1F01);
        LOG("GL_VERSION=%s  GL_RENDERER=%s", v ? (const char *)v : "(null)", r ? (const char *)r : "(null)");
        g_gl_ready = 1;
    }
    return c;
}
void ov_SDL_Log(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("SDL_Log: %s", b);
}
void ov_exit(int code) { LOG("game called exit(%d)", code); exit(code); }
void ov_abort(void) { LOG("game called abort()"); abort(); }


/* ------------------------------------------------------------ text input -- */
/* The Vita keyboard dialog hands SDL the whole typed string in ONE text event.
 * The game was written for Android, where each event carries one character, and
 * only reads the first character of each event. Split long events up. */
extern int SDL_PollEvent(void *event);
static char     g_txt_rest[64];
static uint8_t  g_txt_hdr[12];
static size_t u8len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }
static void put_text_event(uint8_t *e, const char *s, size_t n) {
    memset(e, 0, 56);                         /* sizeof(SDL_Event) */
    memcpy(e, g_txt_hdr, sizeof g_txt_hdr);   /* type, timestamp, windowID */
    memcpy(e + 12, s, n);
}
/* ---- Vita buttons -> PC keyboard keys / mouse buttons ---------------------
 * The game was built for phones and a PC keyboard + mouse. Start acts as Space
 * (pause), Square as B (bubbles) and Triangle as the RIGHT mouse button (an SDL
 * mouse-button event at the last pointer position, since the PC code reads
 * right-clicks from mouse events, not keys). We watch the pad ourselves and
 * hand the game ordinary SDL events, in the same order a real keyboard produces them:
 * key-down, then (only if a text box is active) a text event, then key-up.
 * With no text box open the game turns a key-down into the typed character
 * itself, so nothing is delivered twice.
 *
 * EXPERIMENTAL: D-pad Up opens the Vita on-screen keyboard; whatever is typed
 * is then replayed to the game one key at a time (for cheat codes).
 *
 * loader.cfg: key_start=32  key_square=98  (ASCII codes; 0 disables)
 *             cross_right_click=1            (0 disables Cross = right-click)
 *             triangle_presto=1              (0 disables Triangle = Presto menu)
 *             circle_close=1                 (0 disables Circle = close Presto menu / options / unpause)
 *             help_lr=1                      (0 disables L / R = previous / next on the Help and Story screens and tab on the Hall of Fame)
 *             quit_confirm=1                 (0 = the Quit button exits without the confirmation dialog)
 *             cheat_keyboard=1               (0 disables the D-pad Up keyboard)
 *             cheat_hold_ms=1500             (how long to hold Up) */
int g_key_start = 32, g_key_square = 98, g_cross_rclick = 1, g_triangle_presto = 1, g_circle_close = 1, g_cheat_kb = 1, g_cheat_hold_ms = 1500;
int g_help_lr = 1;        /* loader.cfg: help_lr=0 disables L/R = previous/next on the Help, Story and Hall of Fame screens */
int g_quit_confirm = 1;   /* loader.cfg: quit_confirm=0 makes the Quit button exit without asking */

extern int  SDL_IsTextInputActive(void);
extern void SDL_StartTextInput(void);
extern void SDL_StopTextInput(void);
extern int  SDL_IsScreenKeyboardShown(void *window);

static int ascii_to_scancode(int c) {
    if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 4 + (c - 'A');
    if (c >= '1' && c <= '9') return 30 + (c - '1');
    if (c == '0') return 39;
    if (c == ' ') return 44;
    return 0;
}
static void put_key_event(uint8_t *e, int down, int ascii) {
    int sc = ascii_to_scancode(ascii), mod = 0;
    if (ascii >= 'A' && ascii <= 'Z') { ascii += 32; mod = 0x0001; }   /* KMOD_LSHIFT */
    memset(e, 0, 56);
    *(uint32_t *)(e + 0)  = down ? 0x300u : 0x301u;   /* SDL_KEYDOWN / SDL_KEYUP */
    *(uint32_t *)(e + 8)  = 1;                         /* windowID */
    e[12] = down ? 1 : 0;                              /* state */
    *(int32_t *)(e + 16) = sc;                         /* keysym.scancode */
    *(int32_t *)(e + 20) = ascii;                      /* keysym.sym (== ASCII for these keys) */
    *(uint16_t *)(e + 24) = (uint16_t)mod;             /* keysym.mod */
}

/* Last pointer position the game has seen (window coordinates), updated from the real mouse/touch events
 * that pass through ov_SDL_PollEvent. A Triangle right-click is delivered at this position. */
#define SDL_MOUSEMOTION_T      0x400u
#define SDL_MOUSEBUTTONDOWN_T  0x401u
#define SDL_MOUSEBUTTONUP_T    0x402u
#define SDL_BUTTON_RIGHT_N     3
static int32_t  g_mouse_x = 320, g_mouse_y = 240;     /* start at the middle of the 640x480 window */
static uint32_t g_mouse_win = 1;
static void track_mouse(const uint8_t *e) {
    uint32_t t = *(const uint32_t *)e;
    if (t != SDL_MOUSEMOTION_T && t != SDL_MOUSEBUTTONDOWN_T && t != SDL_MOUSEBUTTONUP_T) return;
    g_mouse_win = *(const uint32_t *)(e + 8);
    g_mouse_x = *(const int32_t *)(e + 20);           /* SDL_MouseMotionEvent / SDL_MouseButtonEvent: x, y */
    g_mouse_y = *(const int32_t *)(e + 24);
}
static void put_mouse_button_event(uint8_t *e, int down, int button) {
    memset(e, 0, 56);
    *(uint32_t *)(e + 0)  = down ? SDL_MOUSEBUTTONDOWN_T : SDL_MOUSEBUTTONUP_T;
    *(uint32_t *)(e + 4)  = (uint32_t)(sceKernelGetProcessTimeWide() / 1000u);   /* timestamp (ms) */
    *(uint32_t *)(e + 8)  = g_mouse_win;               /* windowID */
    *(uint32_t *)(e + 12) = 0;                          /* which: a real mouse (not SDL_TOUCH_MOUSEID) */
    e[16] = (uint8_t)button;                            /* button */
    e[17] = down ? 1 : 0;                               /* state */
    e[18] = 1;                                          /* clicks */
    *(int32_t *)(e + 20) = g_mouse_x;                   /* x */
    *(int32_t *)(e + 24) = g_mouse_y;                   /* y */
}

/* Queue of pending key presses: kind 0 = key-up, 1 = key-down. */
#define KQ 512
static struct { uint8_t down; uint8_t ascii; } g_kq[KQ];
static int g_kq_head, g_kq_n;
static int g_followup_text;                  /* char to send as a text event next, or 0 */
#define QUIT_DLG_ID 0x5a                     /* id of our "Quit game?" confirmation dialog (unused by the game) */
#define KQ_HELP_PREV 4                       /* "ascii" 4 / 5: Help screen previous / next page */
#define KQ_HELP_NEXT 5
#define KQ_CIRCLE 3                          /* "ascii" 3: close Presto menu / options dialog, unpause */
#define KQ_PRESTO 2                          /* "ascii" 2 in the queue means: run the Presto right-click action */
#define KQ_RCLICK 1                          /* not a key: "ascii" 1 in the queue means right mouse button */
static void kq_push(int down, int ascii) {
    if (g_kq_n >= KQ) return;
    int i = (g_kq_head + g_kq_n++) % KQ;
    g_kq[i].down = (uint8_t)down; g_kq[i].ascii = (uint8_t)ascii;
}

/* Vita on-screen keyboard session for the cheat feature. */
volatile int    g_cheat_wait;
static int      g_cheat_was_active, g_cheat_saw_shown;
static uint64_t g_cheat_t0, g_cheat_closed_at;
static void cheat_finish(const char *text) {
    g_cheat_wait = 0;
    if (!g_cheat_was_active) SDL_StopTextInput();
    if (!text || !*text) { LOG("cheat keyboard: cancelled"); return; }
    LOG("cheat keyboard: sending \"%s\"", text);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 0x20 || *p > 0x7e) continue;          /* plain ASCII only */
        kq_push(1, *p); kq_push(0, *p);
    }
}

static void scan_pad(void) {
    static unsigned prev;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) return;
    unsigned now = pad.buttons, ch = now ^ prev;
    prev = now;
    /* Keep the buttons to ourselves while any keyboard dialog is open. */
    if (g_cheat_wait || (g_window && SDL_IsScreenKeyboardShown(g_window))) return;
    { static int nl; if (nl < 60 && (ch & (SCE_CTRL_START | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_UP))) {
        nl++; DLOG("pad: %s%s%s%s%s%s %s", (ch & SCE_CTRL_START) ? "Start " : "", (ch & SCE_CTRL_SQUARE) ? "Square " : "",
            (ch & SCE_CTRL_TRIANGLE) ? "Triangle " : "", (ch & SCE_CTRL_CROSS) ? "Cross " : "", (ch & SCE_CTRL_CIRCLE) ? "Circle " : "", (ch & SCE_CTRL_UP) ? "Up " : "", (now & ch) ? "down" : "up"); } }
    if (g_key_start && (ch & SCE_CTRL_START))   kq_push((now & SCE_CTRL_START) != 0,   g_key_start);
    if (g_key_square && (ch & SCE_CTRL_SQUARE)) kq_push((now & SCE_CTRL_SQUARE) != 0, g_key_square);
    if (g_cross_rclick && (ch & SCE_CTRL_CROSS)) kq_push((now & SCE_CTRL_CROSS) != 0, KQ_RCLICK);           /* Cross = right-click */
    if (g_circle_close && (ch & SCE_CTRL_CIRCLE) && (now & SCE_CTRL_CIRCLE)) kq_push(1, KQ_CIRCLE);        /* Circle = back out */
    if (g_help_lr && (ch & SCE_CTRL_LTRIGGER) && (now & SCE_CTRL_LTRIGGER)) kq_push(1, KQ_HELP_PREV);       /* L = Help: previous page */
    if (g_help_lr && (ch & SCE_CTRL_RTRIGGER) && (now & SCE_CTRL_RTRIGGER)) kq_push(1, KQ_HELP_NEXT);       /* R = Help: next page */
    if (g_triangle_presto && (ch & SCE_CTRL_TRIANGLE) && (now & SCE_CTRL_TRIANGLE)) kq_push(1, KQ_PRESTO);  /* Triangle = Presto menu */
    /* Up must be HELD (cheat_hold_ms, default 1.5 s) so a stray tap does nothing. */
    static uint64_t up_since;
    if (!g_cheat_kb || !(now & SCE_CTRL_UP)) { up_since = 0; return; }
    uint64_t t = sceKernelGetProcessTimeWide();
    if (ch & SCE_CTRL_UP) up_since = t;
    if (up_since && t - up_since >= (uint64_t)g_cheat_hold_ms * 1000u) {
        up_since = 0;
        int active = SDL_IsTextInputActive();
        g_cheat_was_active = 0;
        if (active) {                                   /* SDL thinks text input is already on: restart it so the dialog appears */
            LOG("cheat keyboard: text input was already active, restarting it");
            SDL_StopTextInput();
        }
        g_cheat_wait = 1; g_cheat_saw_shown = 0; g_cheat_closed_at = 0;
        g_cheat_t0 = t;
        SDL_StartTextInput();
        LOG("cheat keyboard: opened");
    }
}

/* Heartbeat thread calls this: if the keyboard is open but the game has stopped
 * drawing/polling, cancel it so the game cannot stay stuck, and say why in the log. */
void cheat_watchdog(void) {
    if (!g_cheat_wait) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - g_swap_stamp > 4u * 1000000u) {
        LOG("cheat keyboard: STALLED (last frame %ums ago, last event poll %ums ago) - cancelling",
            (unsigned)((now - g_swap_stamp) / 1000), (unsigned)((now - g_poll_stamp) / 1000));
        g_cheat_wait = 0;
        if (!g_cheat_was_active) SDL_StopTextInput();
    }
}

/* Called every poll while a cheat keyboard session is open. */
static void cheat_tick(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    int shown = g_window ? SDL_IsScreenKeyboardShown(g_window) : 0;
    if (shown) { g_cheat_saw_shown = 1; g_cheat_closed_at = 0; }
    else if (g_cheat_saw_shown && !g_cheat_closed_at) g_cheat_closed_at = now;
    if (g_cheat_closed_at && now - g_cheat_closed_at > 300000u) cheat_finish(NULL);   /* closed without text */
    else if (now - g_cheat_t0 > 120u * 1000000u) cheat_finish(NULL);                  /* safety timeout */
}

/* Triangle: do what a right-click on Presto does. The game's own handler (FishTypePet::HandleMouseDown /
 * OtherTypePet::MouseDown with button -1) is:  if (pet->PrestoRightClicked(cooldown)) app->OpenPrestoDialog(pet);
 * PrestoRightClicked shows the "cool down" message itself and returns nonzero when the menu may open.
 * Object layout (libmain.so): app = *gSexyApp, board = *(app+0x604), pet->app = *(pet+0x8c),
 * fish-type pets live in *(board+0xb8) (cooldown at pet+0x220), other pets in *(board+0xb4) (cooldown at pet+0x19c).
 * Runs on the game thread (called from the event poll). Every pointer is checked; no Presto = nothing happens. */
static void presto_right_click(void) {
    typedef void *(*get_pet_t)(void *board);
    typedef int   (*rclick_t)(void *pet, int cooldown);
    typedef void  (*open_t)(void *app, void *pet);
    static get_pet_t get_pet; static rclick_t rclick; static open_t open_dlg; static void **p_app; static int resolved;
    if (!resolved) {
        resolved = 1;
        get_pet  = (get_pet_t)so_sym("_ZN4Sexy5Board12GetPrestoPetEv");
        rclick   = (rclick_t)so_sym("_ZN4Sexy10GameObject18PrestoRightClickedEi");
        open_dlg = (open_t)so_sym("_ZN4Sexy10WinFishApp16OpenPrestoDialogEPNS_10GameObjectE");
        p_app    = (void **)so_sym("_ZN4Sexy8gSexyAppE");
        LOG("presto: GetPrestoPet=%p PrestoRightClicked=%p OpenPrestoDialog=%p gSexyApp=%p", (void *)get_pet, (void *)rclick, (void *)open_dlg, (void *)p_app);
    }
    if (!get_pet || !rclick || !open_dlg || !p_app) return;
    uint8_t *app = (uint8_t *)*p_app;
    if (!app) { DLOG("presto: no app"); return; }
    uint8_t *board = *(uint8_t **)(app + 0x604);
    if (!board) { DLOG("presto: no board (not in a tank)"); return; }
    /* No "dialog already open" check: app+0x720 is left set when the menu is cancelled, and
     * OpenPrestoDialog closes any older Presto dialog itself before opening a new one. */
    uint8_t *pet = (uint8_t *)get_pet(board);
    if (!pet) { DLOG("presto: no Presto in this tank"); return; }
    if (*(uint8_t **)(pet + 0x8c) != app) { LOG("presto: unexpected pet->app %p != %p, ignoring", *(void **)(pet + 0x8c), (void *)app); return; }
    /* Which list is the pet in? (decides where its cooldown lives) */
    int is_fish = 0;
    uint32_t **vec = *(uint32_t ***)(board + 0xb8);
    if (vec) for (uint32_t *it = vec[0]; it && it < (uint32_t *)vec[1]; it++) if ((uint8_t *)*it == pet) { is_fish = 1; break; }
    int cooldown = *(int *)(pet + (is_fish ? 0x220 : 0x19c));
    int ok = rclick(pet, cooldown);
    LOG("presto: pet=%p type=%s cooldown=%d -> %s", (void *)pet, is_fish ? "fish" : "other", cooldown, ok ? "opening menu" : "cooldown message");
    if (ok) open_dlg(app, pet);
}

/* Circle: back out of the top-most dialog the way tapping its Cancel/No button would.
 * Layout (libmain.so, SexyAppBase): dialogs are in a std::map<int,Dialog*> at app+0x2c8 (count at +0x2cc) and in
 * an add-order list whose last node is *(app+0x2d0) (node: prev, next, Dialog*). The top dialog is the last one added.
 *  - Options (id 1): KillDialog(1).            - Presto menu (id 0x1f): KillDialog(0x1f) and clear app+0x720.
 *  - Continue-level dialog: its CANCEL button (ContinueDialog::ButtonDepress(2) - any id other than 0/1 just closes it).
 *  - Anything else (confirmations such as "return to main menu?"): DialogButtonDepress(id, 1001) = the Cancel/No button.
 * With no dialog open, the top-most of the Help / level (Tank) / pet (Pets) / story (Story) / store (Store) /
 * hall of fame (HighScore) screens gets its "menu" / "back" button pressed (Help id 1, Tank id 0x63, Pets id 0x64,
 * Story id 0, Store id 0x63 = back to the virtual tank, HighScore id 4), found by walking the widget
 * manager's widget list (app+0x2c0, nodes {prev,next,Widget*}, last = top-most) and stopping at the game board.
 * Only when NO dialog is left, no screen was handled and the board is still paused is the game unpaused, by sending the Space key the same
 * way Start does (the game then removes its own "Game Paused" overlay). Game thread only. */
static int dialog_id_of(uint8_t *app, void *dlg) {
    uint8_t *stack[64]; int sp = 0; uint8_t *n = *(uint8_t **)(app + 0x2c8);
    while ((n || sp) && sp < 63) {
        while (n && sp < 63) { stack[sp++] = n; n = *(uint8_t **)n; }
        if (!sp) break;
        n = stack[--sp];
        if (*(void **)(n + 0x14) == dlg) return *(int *)(n + 0x10);
        n = *(uint8_t **)(n + 4);
    }
    return -1;
}
/* Is widget w in the widget manager's list? */
static int widget_listed(uint8_t *app, uint8_t *w) {
    uint8_t *wm = *(uint8_t **)(app + 0x2c0);
    if (!wm || !w) return 0;
    uint8_t *sentinel = wm + 4, *node = *(uint8_t **)sentinel;
    for (int guard = 0; node && node != sentinel && guard < 256; guard++, node = *(uint8_t **)node)
        if (*(uint8_t **)(node + 8) == w) return 1;
    return 0;
}
/* In a level the Help screen is opened from the Options dialog (id 1), which stays open behind it. */
static int help_over_options(uint8_t *app) {
    uint8_t *help = *(uint8_t **)(app + 0x6c0);
    if (!help || *(int *)(app + 0x2cc) != 1 || !widget_listed(app, help)) return 0;
    uint8_t *last = *(uint8_t **)(app + 0x2d0);
    void *dlg = last ? *(void **)(last + 8) : NULL;
    return dlg && dialog_id_of(app, dlg) == 1;
}
static void circle_back(void) {
    typedef void (*pause_t)(void *board, int on);
    typedef void (*dbd_t)(void *app, int dlg_id, int button_id);
    typedef void (*btn_t)(void *self, int id);
    static pause_t pause_game; static dbd_t dlg_button; static void **p_app; static uint8_t *cont_vt; static int resolved;
    static btn_t cont_btn, help_btn, tank_btn, pets_btn, story_btn, store_btn, hs_btn, setup_btn, fish_btn;
    if (!resolved) {
        resolved = 1;
        pause_game = (pause_t)so_sym("_ZN4Sexy5Board9PauseGameEb");
        dlg_button = (dbd_t)so_sym("_ZN4Sexy11SexyAppBase19DialogButtonDepressEii");
        p_app      = (void **)so_sym("_ZN4Sexy8gSexyAppE");
        cont_vt    = (uint8_t *)so_sym("_ZTVN4Sexy14ContinueDialogE");
        cont_btn   = (btn_t)so_sym("_ZN4Sexy14ContinueDialog13ButtonDepressEi");
        help_btn   = (btn_t)so_sym("_ZN4Sexy10HelpScreen13ButtonDepressEi");
        tank_btn   = (btn_t)so_sym("_ZN4Sexy10TankScreen13ButtonDepressEi");
        pets_btn   = (btn_t)so_sym("_ZN4Sexy10PetsScreen13ButtonDepressEi");
        story_btn  = (btn_t)so_sym("_ZN4Sexy11StoryScreen13ButtonDepressEi");
        store_btn  = (btn_t)so_sym("_ZN4Sexy11StoreScreen13ButtonDepressEi");
        hs_btn     = (btn_t)so_sym("_ZN4Sexy15HighScoreScreen13ButtonDepressEi");
        setup_btn  = (btn_t)so_sym("_ZN4Sexy14SimSetupScreen13ButtonDepressEi");
        fish_btn   = (btn_t)so_sym("_ZN4Sexy13SimFishScreen13ButtonDepressEi");
        LOG("circle: PauseGame=%p DialogButtonDepress=%p ContinueDialog vtable=%p gSexyApp=%p",
            (void *)pause_game, (void *)dlg_button, (void *)cont_vt, (void *)p_app);
    }
    if (!p_app || !*p_app) return;
    uint8_t *app = (uint8_t *)*p_app;
    uint8_t *board = *(uint8_t **)(app + 0x604);
    void (**vt)(void *, int) = *(void (***)(void *, int))app;
    int count = *(int *)(app + 0x2cc), id = -1;
    const char *act = "no dialog";
    int skip_unpause = 0, screen_done = 0;
    int help_over = help_over_options(app);      /* Help screen showing on top of the Options dialog (in a level) */
    if (count > 0 && !help_over) {
        uint8_t *last = *(uint8_t **)(app + 0x2d0);
        void *dlg = last ? *(void **)(last + 8) : NULL;
        if (dlg) {
            id = dialog_id_of(app, dlg);
            if (id == 1)         { vt[0x130 / 4](app, 1); act = "options closed"; }
            else if (id == 0x1f) { vt[0x130 / 4](app, 0x1f); *(void **)(app + 0x720) = NULL; act = "presto menu closed"; }
            else if (id == QUIT_DLG_ID) { vt[0x130 / 4](app, id); act = "quit dialog: cancelled"; }
            else if (cont_vt && *(uint8_t **)dlg == cont_vt + 8) {
                if (cont_btn) { cont_btn(dlg, 2); act = "continue dialog: cancel"; } else act = "continue dialog (no handler)";
                skip_unpause = 1;
            }
            else if (id >= 0 && dlg_button) { dlg_button(app, id, 1001); act = "cancel pressed"; }
            else act = "unknown dialog";
        }
    }
    int left = *(int *)(app + 0x2cc);
    if (left == 0 || help_over) {                  /* no dialog (or only Options behind Help): Help / level / pet / ... screens */
        uint8_t *wm = *(uint8_t **)(app + 0x2c0);
        uint8_t *help = *(uint8_t **)(app + 0x6c0), *tank = *(uint8_t **)(app + 0x6b8), *pets = *(uint8_t **)(app + 0x698);
        uint8_t *story = *(uint8_t **)(app + 0x6ac), *store = *(uint8_t **)(app + 0x69c), *hs = *(uint8_t **)(app + 0x6bc);
        /* Virtual tank "Tank Setup" (app+0x6b0) and "Fish Setup" (app+0x6b4): their Done buttons (ids 9 / 100) apply
         * the settings and return to the virtual tank. */
        uint8_t *setup = *(uint8_t **)(app + 0x6b0), *fishs = *(uint8_t **)(app + 0x6b4);
        if (wm) {
            uint8_t *sentinel = wm + 4, *node = *(uint8_t **)sentinel;      /* last = top-most */
            for (int guard = 0; node && node != sentinel && guard < 256; guard++, node = *(uint8_t **)node) {
                uint8_t *w = *(uint8_t **)(node + 8);
                if (!w) continue;
                if (w == board) break;                                      /* game board on top: no screen */
                if (help && w == help && help_btn) { help_btn(w, 1);    act = "help screen: menu button"; screen_done = 1; break; }
                if (tank && w == tank && tank_btn) { tank_btn(w, 0x63); act = "level screen: menu button"; screen_done = 1; break; }
                if (pets && w == pets && pets_btn) {
                    /* Virtual tank (app+0x6e8 == 5, the game's own test for it, with its board running): the pet
                     * screen's "done" button (id 99) applies the choice and returns to the tank. Otherwise: menu. */
                    int vtank = board && *(int *)(app + 0x6e8) == 5;
                    pets_btn(w, vtank ? 99 : 0x64);
                    act = vtank ? "pet screen: back to virtual tank" : "pet screen: menu button";
                    screen_done = 1; break;
                }
                if (story && w == story && story_btn) { story_btn(w, 0);  act = "story screen: menu button"; screen_done = 1; break; }
                if (store && w == store && store_btn) { store_btn(w, 0x63); act = "store screen: back button"; screen_done = 1; break; }
                if (hs && w == hs && hs_btn)          { hs_btn(w, 4);     act = "hall of fame: menu button"; screen_done = 1; break; }
                if (setup && w == setup && setup_btn) { setup_btn(w, 9);  act = "tank setup: done (back to virtual tank)"; screen_done = 1; break; }
                if (fishs && w == fishs && fish_btn)  { fish_btn(w, 100); act = "fish setup: done (back to virtual tank)"; screen_done = 1; break; }
            }
        }
    }
    int paused = board ? (*(volatile uint8_t *)(board + 0x98) != 0) : 0;
    const char *un = "";
    if (board && paused && left == 0 && !skip_unpause && !screen_done) {
        if (g_key_start) { kq_push(1, g_key_start); kq_push(0, g_key_start); un = ", unpause via Space"; }
        else if (pause_game) { pause_game(board, 0); un = ", unpause via PauseGame"; }
    }
    if (help_over && screen_done) un = ", options dialog kept, game stays paused";
    LOG("circle: dialogs %d->%d top id=%d: %s; board=%s paused=%d%s", count, left, id, act, board ? "yes" : "no", paused, un);
}

/* L / R on the Help screen and on the pet/alien Story screen: press the screen's Previous / Next button, which is
 * what tapping them does. HelpScreen::ButtonDepress: Next = id 2, Previous = id 3 (page field wraps 1..8).
 * StoryScreen::ButtonDepress: Previous = id 1, Next = id 2 (wraps over the 33 stories, skipping locked ones).
 * HighScoreScreen (hall of fame): ButtonDepress(0..3) selects a tab, so L / R pick the neighbouring tab (wrapping).
 * Only while that screen is the top-most one and no dialog is open (in a level the Options dialog stays open behind
 * the Help screen, which is fine). Game thread only. */
static void help_page(int next) {
    typedef void (*btn_t)(void *self, int id);
    static btn_t help_btn, story_btn, hs_btn; static void **p_app; static int resolved;
    if (!resolved) {
        resolved = 1;
        help_btn  = (btn_t)so_sym("_ZN4Sexy10HelpScreen13ButtonDepressEi");
        story_btn = (btn_t)so_sym("_ZN4Sexy11StoryScreen13ButtonDepressEi");
        hs_btn    = (btn_t)so_sym("_ZN4Sexy15HighScoreScreen13ButtonDepressEi");
        p_app     = (void **)so_sym("_ZN4Sexy8gSexyAppE");
        LOG("page L/R: Help=%p Story=%p HallOfFame=%p gSexyApp=%p", (void *)help_btn, (void *)story_btn, (void *)hs_btn, (void *)p_app);
    }
    if (!p_app || !*p_app) return;
    uint8_t *app = (uint8_t *)*p_app;
    if (*(int *)(app + 0x2cc) > 0 && !help_over_options(app)) return;   /* a dialog is open (Options behind Help is fine) */
    uint8_t *wm = *(uint8_t **)(app + 0x2c0), *board = *(uint8_t **)(app + 0x604);
    uint8_t *help = *(uint8_t **)(app + 0x6c0), *story = *(uint8_t **)(app + 0x6ac);
    uint8_t *hs = *(uint8_t **)(app + 0x6bc);
    uint8_t *other[4] = { board, *(uint8_t **)(app + 0x6b8) /* tank */, *(uint8_t **)(app + 0x698) /* pets */,
                          *(uint8_t **)(app + 0x69c) /* store */ };
    if (!wm) return;
    uint8_t *sentinel = wm + 4, *node = *(uint8_t **)sentinel;      /* last = top-most */
    for (int guard = 0; node && node != sentinel && guard < 256; guard++, node = *(uint8_t **)node) {
        uint8_t *w = *(uint8_t **)(node + 8);
        if (!w) continue;
        for (int k = 0; k < 4; k++) if (other[k] && w == other[k]) return;   /* some other screen is on top */
        if (help && w == help && help_btn) {
            help_btn(w, next ? 2 : 3);
            LOG("help: %s page", next ? "next" : "previous");
            return;
        }
        if (hs && w == hs && hs_btn) {
            /* Hall of Fame tabs, left to right: 0 Adventure, 1 Time Trial, 2 Challenge, 3 Personal. The current
             * tab is the int at +0xac; HighScoreScreen::ButtonDepress(0..3) selects a tab. L / R wrap around. */
            int cur = *(int *)(w + 0xac);
            if (cur < 0 || cur > 3) cur = 0;
            int nt = next ? (cur + 1) % 4 : (cur + 3) % 4;
            hs_btn(w, nt);
            LOG("hall of fame: tab %d -> %d", cur, nt);
            return;
        }
        if (story && w == story && story_btn) {
            story_btn(w, next ? 2 : 1);
            LOG("story: %s", next ? "next" : "previous");
            return;
        }
    }
}

int ov_SDL_PollEvent(void *ev) {
    uint8_t *e = ev;
    g_poll_stamp = sceKernelGetProcessTimeWide();
    if (g_txt_rest[0]) {
        size_t n = u8len((unsigned char)g_txt_rest[0]), len = strlen(g_txt_rest);
        if (n > len) n = len;
        char one[8]; memcpy(one, g_txt_rest, n); one[n] = 0;
        memmove(g_txt_rest, g_txt_rest + n, len - n + 1);
        put_text_event(e, one, n);
        return 1;
    }
    if (g_followup_text) {                       /* text event that goes with the key-down just sent */
        char one[2] = { (char)g_followup_text, 0 };
        g_followup_text = 0;
        memset(g_txt_hdr, 0, sizeof g_txt_hdr); *(uint32_t *)g_txt_hdr = 0x303u; g_txt_hdr[8] = 1;
        put_text_event(e, one, 1);
        return 1;
    }
    if (!g_kq_n && !g_cheat_wait) scan_pad();
    if (g_kq_n) {
        int down = g_kq[g_kq_head].down, ascii = g_kq[g_kq_head].ascii;
        g_kq_head = (g_kq_head + 1) % KQ; g_kq_n--;
        if (ascii == KQ_CIRCLE) { circle_back(); return ov_SDL_PollEvent(ev); }
        if (ascii == KQ_PRESTO) { presto_right_click(); return ov_SDL_PollEvent(ev); }
        if (ascii == KQ_HELP_PREV || ascii == KQ_HELP_NEXT) { help_page(ascii == KQ_HELP_NEXT); return ov_SDL_PollEvent(ev); }
        if (ascii == KQ_RCLICK) { put_mouse_button_event(e, down, SDL_BUTTON_RIGHT_N); return 1; }
        put_key_event(e, down, ascii);
        if (down && SDL_IsTextInputActive() && ascii >= 0x20) g_followup_text = ascii;
        return 1;
    }
    for (;;) {
        int r = SDL_PollEvent(ev);
        if (r) track_mouse(e);
        if (r && g_cheat_wait && *(uint32_t *)e == 0x303 /* SDL_TEXTINPUT */) {
            char text[64]; snprintf(text, sizeof text, "%s", (const char *)e + 12);
            cheat_finish(text);                  /* swallow it: the game never sees the raw text */
            continue;
        }
        if (g_cheat_wait) cheat_tick();
        if (r && *(uint32_t *)e == 0x303 /* SDL_TEXTINPUT */) {
            const char *t = (const char *)e + 12;
            size_t len = strlen(t), n = u8len((unsigned char)t[0]);
            if (len > n && len < sizeof g_txt_rest) {
                memcpy(g_txt_hdr, e, sizeof g_txt_hdr);
                snprintf(g_txt_rest, sizeof g_txt_rest, "%s", t + n);
                char one[8]; memcpy(one, t, n); one[n] = 0;
                put_text_event(e, one, n);
                { static int n2; if (n2++ < 3) LOG("text input split into single characters"); }
            }
        }
        return r;
    }
}

/* ------------------------------------------- optional zero-filled memory -- */
/* Android hands out zeroed pages far more often than this heap does. A latent
 * "uninitialised variable" bug in the game can therefore look random here.
 * loader.cfg: zero_memory=1 turns this on as an experiment. */
int g_zero_mem = 1;   /* on by default: fixes invisible fish; zero_memory=0 in loader.cfg turns it off */
void *ov_malloc(size_t n) {
    void *p = malloc(n);
    if (p && g_zero_mem) memset(p, 0, n);
    return p;
}
static void *new_impl(size_t n, int may_throw) {
    void *p = malloc(n ? n : 1);
    if (!p) { LOG("out of memory: operator new(%u)", (unsigned)n); if (may_throw) abort(); return NULL; }
    if (g_zero_mem) memset(p, 0, n);
    return p;
}
void *ov__Znwj(size_t n) { return new_impl(n, 1); }
void *ov__Znaj(size_t n) { return new_impl(n, 1); }
void *ov__ZnwjRKSt9nothrow_t(size_t n, const void *x) { (void)x; return new_impl(n, 0); }
void *ov__ZnajRKSt9nothrow_t(size_t n, const void *x) { (void)x; return new_impl(n, 0); }

/* ------------------------------------------------------------ Quit button -- */
/* The Android build leaves WinFishApp::DoQuitDialog() empty, so the main-menu
 * Quit button does nothing. main.c points the game's virtual-call slot for it
 * at loader_quit_hook(), which opens the game's own Yes/No dialog ("Quit" / "Stop the insanity?", buttons QUIT / CANCEL,
 * id QUIT_DLG_ID, same DoDialog call the game uses for "leave game?").
 * The dialog's buttons report through DialogButtonDepress(dialog id, button id)
 * (Yes = 1000, No = 1001); main.c also redirects that virtual slot (both the plain
 * entry and the this-adjusting thunk) to loader_dlg_btn_hook_*(). For our dialog
 * Yes quits and No closes it; every other dialog is passed straight to the game.
 * Quitting: ask SDL to quit (the game shuts down normally), and force the exit to
 * the LiveArea if it has not finished after 3 s. */
extern int SDL_PushEvent(void *ev);
int g_dlg_btn_orig_plain, g_dlg_btn_orig_thunk;      /* set by main.c: original DialogButtonDepress entries */
static int quit_watch(SceSize a, void *b) {
    (void)a; (void)b;
    sceKernelDelayThread(3 * 1000 * 1000);
    LOG("quit: forcing exit");
    sceKernelExitProcess(0);
    return 0;
}
static void do_quit(void) {
    static int once;
    if (once++) return;
    uint8_t ev[56]; memset(ev, 0, sizeof ev);
    *(uint32_t *)ev = 0x100u;                    /* SDL_QUIT */
    SDL_PushEvent(ev);
    SceUID t = sceKernelCreateThread("quit", quit_watch, 0x60, 16 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

typedef void *(*dodlg_t)(void *app, int id, int modal, const void *hdr, const void *lines, const void *foot, int buttons);
typedef void  (*sinit_t)(void *s, const char *p, unsigned n);
typedef void  (*sdtor_t)(void *s);
typedef void *(*getdlg_t)(void *app, int id);
typedef void *(*sassign_t)(void *s, const char *p, unsigned n);

void loader_quit_hook(void *app) {
    static dodlg_t do_dialog; static sinit_t str_init; static sdtor_t str_dtor; static getdlg_t get_dialog; static sassign_t str_assign; static int resolved;
    if (!resolved) {
        resolved = 1;
        do_dialog  = (dodlg_t)so_sym("_ZN4Sexy10WinFishApp8DoDialogEibRKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_S9_i");
        str_init   = (sinit_t)so_sym("_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcj");
        str_dtor   = (sdtor_t)so_sym("_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
        get_dialog = (getdlg_t)so_sym("_ZN4Sexy11SexyAppBase9GetDialogEi");
        str_assign = (sassign_t)so_sym("_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcj");
        LOG("quit dialog: DoDialog=%p string init=%p dtor=%p GetDialog=%p assign=%p", (void *)do_dialog, (void *)str_init, (void *)str_dtor, (void *)get_dialog, (void *)str_assign);
    }
    LOG("Quit button pressed");
    if (!g_quit_confirm || !app || !do_dialog || !str_init || !str_dtor) {
        if (g_quit_confirm) LOG("quit dialog: functions missing, quitting without confirmation");
        do_quit();
        return;
    }
    if (get_dialog && get_dialog(app, QUIT_DLG_ID)) return;           /* already showing */
    /* std::string objects are built by the library's own code, so the layout does not matter here. */
    uint32_t hdr[3] = {0, 0, 0}, lines[3] = {0, 0, 0}, foot[3] = {0, 0, 0};
    static const char H[] = "Quit", L[] = "Stop the insanity?";
    str_init(hdr, H, sizeof H - 1);
    str_init(lines, L, sizeof L - 1);
    str_init(foot, "", 0);
    do_dialog(app, QUIT_DLG_ID, 1, hdr, lines, foot, 1 /* Dialog::BUTTONS_YES_NO */);
    str_dtor(hdr); str_dtor(lines); str_dtor(foot);
    /* Button captions: Dialog+0x98 = Yes button, +0x9c = No button, Dialog+0xc8 = button mode; the caption is the
     * std::string at button+0x90. Replace "YES"/"NO" with "Quit"/"CANCEL". */
    uint8_t *dlg = get_dialog ? (uint8_t *)get_dialog(app, QUIT_DLG_ID) : NULL;
    if (dlg && str_assign && *(int *)(dlg + 0xc8) == 1) {
        uint8_t *yes = *(uint8_t **)(dlg + 0x98), *no = *(uint8_t **)(dlg + 0x9c);
        if (yes) str_assign(yes + 0x90, "Quit", 4);
        if (no)  str_assign(no + 0x90, "CANCEL", 6);
        LOG("quit dialog: opened (buttons relabelled)");
    } else LOG("quit dialog: opened (button labels left as is: dlg=%p)", (void *)dlg);
}

/* Dialog button result. Called by the dialog through the DialogListener part of the app (this = app+4 for the
 * thunk entry, app for the plain one - neither is used here). */
static void dlg_btn_common(void *self, int id, int btn, int thunk) {
    typedef void (*orig_t)(void *, int, int);
    orig_t orig = (orig_t)(uintptr_t)(thunk ? g_dlg_btn_orig_thunk : g_dlg_btn_orig_plain);
    if (id == QUIT_DLG_ID) {
        if (btn == 1000) {                                   /* Dialog::ID_YES */
            LOG("quit dialog: Yes");
            do_quit();
            return;
        }
        LOG("quit dialog: No");
        if (orig) orig(self, id, btn);
        static getdlg_t get_dialog; static void **p_app; static int resolved;
        if (!resolved) {
            resolved = 1;
            get_dialog = (getdlg_t)so_sym("_ZN4Sexy11SexyAppBase9GetDialogEi");
            p_app      = (void **)so_sym("_ZN4Sexy8gSexyAppE");
        }
        if (get_dialog && p_app && *p_app && get_dialog(*p_app, id)) {   /* the game left it open: close it ourselves */
            void *app = *p_app;
            void (**vt)(void *, int) = *(void (***)(void *, int))app;
            vt[0x130 / 4](app, id);                          /* KillDialog(int) */
        }
        return;
    }
    if (orig) orig(self, id, btn);
}
void loader_dlg_btn_hook_plain(void *self, int id, int btn) { dlg_btn_common(self, id, btn, 0); }
void loader_dlg_btn_hook_thunk(void *self, int id, int btn) { dlg_btn_common(self, id, btn, 1); }
