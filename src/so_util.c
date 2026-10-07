/*
 * Minimal ARM32 ELF shared-object loader for the PS Vita (needs kubridge).
 * Maps the library at a fixed address, applies REL relocations, and binds
 * undefined symbols against a host-provided import table.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>
#include "so_util.h"
#include "common.h"

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } ElfEhdr;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; } ElfPhdr;
typedef struct { int32_t d_tag; uint32_t d_val; } ElfDyn;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other; uint16_t st_shndx; } ElfSym;
typedef struct { uint32_t r_offset, r_info; } ElfRel;

#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_ARM_EXIDX  0x70000001u
#define PF_X          1
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_PLTRELSZ   2
#define DT_RELSZ      18
#define DT_REL        17
#define DT_JMPREL     23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define R_ARM_NONE      0
#define R_ARM_ABS32     2
#define R_ARM_GLOB_DAT  21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE  23
#define STB_WEAK        2

#define PAGE     0x1000u
#define ALIGN_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))
#define ALIGN_DN(x) ((x) & ~(PAGE - 1))

static struct {
    uintptr_t base;                 /* address of vaddr 0 */
    uintptr_t text_lo, text_hi;     /* absolute, page aligned */
    uintptr_t data_lo, data_hi;
    uintptr_t exidx; size_t exidx_sz;
    const ElfSym *sym; uint32_t nsym;
    const char *str;
    const ElfRel *rel;    size_t nrel;
    const ElfRel *jmprel; size_t njmprel;
    const uint32_t *init_arr; size_t ninit;
    unsigned unresolved;
    int protect_text;               /* text was mapped RW; make it RX before running */
    SoImport *imports; size_t nimports;
} M;

/*
 * Map [t_lo, t_lo+t_len) executable and [d_lo, d_lo+d_len) read/write at fixed
 * addresses. kubridge versions differ, so several strategies are tried and every
 * step is logged:
 *   A) kuKernelMemReserve + kuKernelMemCommit (modern kubridge API)
 *   B) kuKernelAllocMemBlock with a base-address hint; text allocated RW and
 *      switched to RX afterwards with kuKernelMemProtect
 *   C) as B, but asking for an RX block type directly
 */
static int alloc_block(const char *name, uint32_t type, uintptr_t addr, size_t size) {
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.size    = sizeof opt;
    opt.field_4 = 0x1;               /* "has base address" (flag word is field_4 in this header) */
    opt.field_C = (SceUInt32)addr;
    SceUID blk = kuKernelAllocMemBlock(name, (SceKernelMemBlockType)type, size, &opt);
    if (blk < 0) { LOG("  alloc %s type=%08x -> error 0x%08x", name, type, blk); return -1; }
    void *p = NULL;
    int r = sceKernelGetMemBlockBase(blk, &p);
    if (r < 0 || (uintptr_t)p != addr) { LOG("  %s mapped at %p, wanted 0x%08x", name, p, (unsigned)addr); return -1; }
    LOG("  alloc %s ok at %p", name, p);
    return 0;
}

static int map_image(uintptr_t t_lo, size_t t_len, uintptr_t d_lo, size_t d_len) {
    /* A */
    void *addr = (void *)t_lo;
    SceUID rsv = kuKernelMemReserve(&addr, (d_lo + d_len) - t_lo, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    LOG("A: kuKernelMemReserve -> 0x%08x, addr=%p", rsv, addr);
    if (rsv >= 0 && (uintptr_t)addr == t_lo) {
        int rt = kuKernelMemCommit((void *)t_lo, t_len, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC, NULL);
        LOG("A: commit text RWX -> 0x%08x", rt);
        if (rt < 0) {
            rt = kuKernelMemCommit((void *)t_lo, t_len, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC, NULL);
            LOG("A: commit text RX -> 0x%08x", rt);
        }
        int rd = kuKernelMemCommit((void *)d_lo, d_len, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, NULL);
        LOG("A: commit data RW -> 0x%08x", rd);
        return (rt < 0 || rd < 0) ? -1 : 0;
    }
    /* B */
    LOG("B: legacy block, RW text then protect");
    if (!alloc_block("so_text", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, t_lo, t_len)) {
        M.protect_text = 1;
        return alloc_block("so_data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, d_lo, d_len);
    }
    /* C */
    LOG("C: legacy block, RX type");
    if (alloc_block("so_text", 0x1020D005u, t_lo, t_len)) return -1;
    return alloc_block("so_data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, d_lo, d_len);
}

static inline int in_text(uintptr_t a) { return a >= M.text_lo && a < M.text_hi; }
static inline uint32_t rd32(uintptr_t a) { uint32_t v; memcpy(&v, (void *)a, 4); return v; }
static void wr32(uintptr_t a, uint32_t v) {
    if (in_text(a)) kuKernelCpuUnrestrictedMemcpy((void *)a, &v, 4);
    else memcpy((void *)a, &v, 4);
}

int so_load(const char *path, uintptr_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { LOG("read failed"); fclose(f); free(buf); return -1; }
    fclose(f);

    const ElfEhdr *eh = (const ElfEhdr *)buf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) || eh->e_ident[4] != 1 || eh->e_machine != 40) {
        LOG("not an ARM32 ELF"); free(buf); return -1;
    }
    const ElfPhdr *ph = (const ElfPhdr *)(buf + eh->e_phoff);
    uint32_t tlo = ~0u, thi = 0, dlo = ~0u, dhi = 0, dyn_v = 0;
    M.base = base;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uint32_t lo = ph[i].p_vaddr, hi = ph[i].p_vaddr + ph[i].p_memsz;
            if (ph[i].p_flags & PF_X) { if (lo < tlo) tlo = lo; if (hi > thi) thi = hi; }
            else                      { if (lo < dlo) dlo = lo; if (hi > dhi) dhi = hi; }
        } else if (ph[i].p_type == PT_DYNAMIC) dyn_v = ph[i].p_vaddr;
        else if (ph[i].p_type == PT_ARM_EXIDX) { M.exidx = base + ph[i].p_vaddr; M.exidx_sz = ph[i].p_memsz; }
    }
    if (!thi || !dhi || !dyn_v) { LOG("unexpected segment layout"); free(buf); return -1; }

    uint32_t t_off = ALIGN_DN(tlo), t_len = ALIGN_UP(thi) - t_off;
    uint32_t d_off = ALIGN_DN(dlo), d_len = ALIGN_UP(dhi) - d_off;
    if (d_off < t_off + t_len) { LOG("text/data share a page - unsupported"); free(buf); return -1; }
    M.text_lo = base + t_off; M.text_hi = M.text_lo + t_len;
    M.data_lo = base + d_off; M.data_hi = M.data_lo + d_len;
    LOG("text %08x-%08x  data %08x-%08x", (unsigned)M.text_lo, (unsigned)M.text_hi, (unsigned)M.data_lo, (unsigned)M.data_hi);

    if (map_image(M.text_lo, t_len, M.data_lo, d_len)) { LOG("could not map image"); free(buf); return -1; }
    memset((void *)M.data_lo, 0, d_len);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_filesz) continue;
        uintptr_t dst = base + ph[i].p_vaddr;
        if (ph[i].p_flags & PF_X) kuKernelCpuUnrestrictedMemcpy((void *)dst, buf + ph[i].p_offset, ph[i].p_filesz);
        else memcpy((void *)dst, buf + ph[i].p_offset, ph[i].p_filesz);
    }
    free(buf);

    for (const ElfDyn *d = (const ElfDyn *)(base + dyn_v); d->d_tag; d++) {
        uintptr_t v = base + d->d_val;
        switch (d->d_tag) {
        case DT_HASH:        M.nsym = ((const uint32_t *)v)[1]; break;   /* nchain == symbol count */
        case DT_SYMTAB:      M.sym = (const ElfSym *)v; break;
        case DT_STRTAB:      M.str = (const char *)v; break;
        case DT_REL:         M.rel = (const ElfRel *)v; break;
        case DT_RELSZ:       M.nrel = d->d_val / 8; break;
        case DT_JMPREL:      M.jmprel = (const ElfRel *)v; break;
        case DT_PLTRELSZ:    M.njmprel = d->d_val / 8; break;
        case DT_INIT_ARRAY:  M.init_arr = (const uint32_t *)v; break;
        case DT_INIT_ARRAYSZ: M.ninit = d->d_val / 4; break;
        }
    }
    if (!M.sym || !M.str || !M.nsym) { LOG("missing dynamic tables (nsym=%u)", M.nsym); return -1; }
    LOG("loaded: %u dynsyms, %u+%u relocs, %u ctors", M.nsym, (unsigned)M.nrel, (unsigned)M.njmprel, (unsigned)M.ninit);
    return 0;
}

static void unresolved_stub(void) {
    LOG("FATAL: call to unresolved import, caller = lib+0x%x", (unsigned)((uintptr_t)__builtin_return_address(0) - M.base));
    abort();
}

static int imp_cmp(const void *a, const void *b) { return strcmp(((const SoImport *)a)->name, ((const SoImport *)b)->name); }

static uintptr_t resolve(const ElfSym *s) {
    if (s->st_shndx != 0) return M.base + s->st_value;       /* defined in the library itself */
    const char *name = M.str + s->st_name;
    SoImport key = { name, 0 };
    const SoImport *hit = bsearch(&key, M.imports, M.nimports, sizeof(SoImport), imp_cmp);
    if (hit) return hit->addr;
    if ((s->st_info >> 4) == STB_WEAK) return 0;
    LOG("UNRESOLVED import: %s", name);
    M.unresolved++;
    return (uintptr_t)unresolved_stub;
}

static int do_rel(const ElfRel *r, size_t n) {
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t type = r[i].r_info & 0xff, si = r[i].r_info >> 8;
        uintptr_t P = M.base + r[i].r_offset;
        uintptr_t S = si ? resolve(&M.sym[si]) : 0;
        switch (type) {
        case R_ARM_NONE: break;
        case R_ARM_RELATIVE: wr32(P, rd32(P) + (uint32_t)M.base); break;
        case R_ARM_ABS32:    wr32(P, rd32(P) + (uint32_t)S); break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT: wr32(P, (uint32_t)S); break;
        default: LOG("unsupported reloc type %u at +0x%x", type, r[i].r_offset); bad++;
        }
    }
    return bad;
}

int so_link(const SoImport *tbl, size_t count) {
    M.imports = malloc(count * sizeof(SoImport));
    memcpy(M.imports, tbl, count * sizeof(SoImport));
    M.nimports = count;
    qsort(M.imports, count, sizeof(SoImport), imp_cmp);
    int bad = do_rel(M.rel, M.nrel) + do_rel(M.jmprel, M.njmprel);
    LOG("link done: %d bad relocs, %u unresolved imports", bad, M.unresolved);
    return bad;
}

void so_flush(void) {
    if (M.protect_text) {
        int r = kuKernelMemProtect((void *)M.text_lo, M.text_hi - M.text_lo, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
        LOG("kuKernelMemProtect(text, RX) -> 0x%08x", r);
    }
    kuKernelFlushCaches((void *)M.text_lo, M.text_hi - M.text_lo);
}

void so_run_init(void) {
    for (size_t i = 0; i < M.ninit; i++) {
        uint32_t fn = M.init_arr[i];
        if (fn && fn != 0xffffffffu) ((void (*)(void))(uintptr_t)fn)();
    }
}

uintptr_t so_sym(const char *name) {
    for (uint32_t i = 1; i < M.nsym; i++)
        if (M.sym[i].st_shndx && !strcmp(M.str + M.sym[i].st_name, name)) return M.base + M.sym[i].st_value;
    return 0;
}

const uint32_t *so_find_exidx(uintptr_t pc, int *count) {
    if (M.exidx && in_text(pc)) { *count = (int)(M.exidx_sz / 8); return (const uint32_t *)M.exidx; }
    return NULL;
}
