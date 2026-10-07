/*
 * C++ exceptions: the game library carries its own .ARM.exidx table. The host
 * unwinder asks __gnu_Unwind_Find_exidx() for the table covering a PC; we
 * intercept it (linker flag --wrap) and answer for the library's address range.
 */
#include <stdint.h>
#include "so_util.h"
#include "common.h"

extern const uint32_t *__real___gnu_Unwind_Find_exidx(uintptr_t pc, int *n) __attribute__((weak));
extern const uint32_t __exidx_start[] __attribute__((weak));
extern const uint32_t __exidx_end[]   __attribute__((weak));

const uint32_t *__wrap___gnu_Unwind_Find_exidx(uintptr_t pc, int *n) {
    const uint32_t *t = so_find_exidx(pc, n);
    { static int cnt; if (cnt++ < 20) LOG("unwind: find_exidx(pc=0x%08x) -> %s", (unsigned)pc, t ? "game table" : "host"); }
    if (t) return t;
    if (__real___gnu_Unwind_Find_exidx) return __real___gnu_Unwind_Find_exidx(pc, n);
    if (__exidx_start && __exidx_end) { *n = (int)((__exidx_end - __exidx_start) / 2); return __exidx_start; }
    *n = 0;
    return 0;
}
