#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *name;
    uintptr_t   addr;
} SoImport;

/* Map an ARM32 ELF shared object at `base` (text RX + data RW, contiguous). */
int  so_load(const char *path, uintptr_t base);
/* Apply all relocations, binding undefined symbols from `tbl`. */
int  so_link(const SoImport *tbl, size_t count);
/* Flush instruction caches after loading/relocating. */
void so_flush(void);
/* Run DT_INIT_ARRAY constructors. */
void so_run_init(void);
/* Address of a symbol the library defines (0 if absent). */
uintptr_t so_sym(const char *name);
/* ARM exception-table lookup for the C++ unwinder (NULL if pc is not ours). */
const uint32_t *so_find_exidx(uintptr_t pc, int *count);
