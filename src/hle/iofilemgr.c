/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * When the host also names the disc image (psp_io_set_disc_image), `disc0:`
 * and `umd0:` are served from the ISO itself instead: every file then has its
 * real start sector, which games read back through sceIoGetstat or an ioctl
 * and use to open raw sector ranges as `disc0:/sce_lbn0x<lbn>_size0x<bytes>`.
 * An extracted tree cannot answer that, because a directory has no sectors.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#ifndef _WIN32
#  define _FILE_OFFSET_BITS 64        /* fseeko/ftello past 2 GB */
#  define _POSIX_C_SOURCE 200809L
#endif

#include "psprecomp/hle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <io.h>
#  define io_seek _fseeki64
#  define io_tell _ftelli64
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#else
#  include <dirent.h>
#  define io_seek fseeko
#  define io_tell ftello
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

#define ERR_NOENT   0x80010002
#define ERR_INVAL   0x80010016
#define ERR_MFILE   0x80010018
#define ERR_BADF    0x80020323

#define SECTOR 2048u

/* A handle is either a host file, or a window [base, base+size) of the disc
 * image with its own position. */
typedef struct {
    FILE *f;
    int used;
    int on_disc;
    uint64_t base, size, pos;
    uint32_t lbn;
} io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static char    g_iso[1024];
static uint64_t g_bytes_read;

static int trace_io(void) { return getenv("PSPRECOMP_TRACE_IO") != NULL; }

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        memset(&g_file[i], 0, sizeof g_file[i]);
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; g_iso[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* ---- the disc image (ISO 9660) ------------------------------------------- */

typedef struct {
    uint32_t lbn, size;
    int dir;
    uint8_t date[7];      /* years since 1900, month, day, h, m, s, tz */
} iso_entry;

static iso_entry g_iso_root;

static int iso_read(FILE *f, uint32_t lbn, uint8_t *buf) {
    if (io_seek(f, (int64_t)lbn * SECTOR, SEEK_SET) != 0) return -1;
    return fread(buf, 1, SECTOR, f) == SECTOR ? 0 : -1;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void iso_record(const uint8_t *r, iso_entry *e) {
    e->lbn  = le32(r + 2);
    e->size = le32(r + 10);
    e->dir  = (r[25] & 0x02) != 0;
    memcpy(e->date, r + 18, 7);
}

int psp_io_set_disc_image(const char *path) {
    g_iso[0] = '\0';
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    uint8_t pvd[SECTOR];
    int ok = f && iso_read(f, 16, pvd) == 0 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5);
    if (ok) iso_record(pvd + 156, &g_iso_root);
    if (f) fclose(f);
    if (!ok) {
        fprintf(stderr, "io: \"%s\" is not an ISO 9660 image; disc0: stays on the extracted tree\n", path);
        return -1;
    }
    snprintf(g_iso, sizeof g_iso, "%s", path);
    fprintf(stderr, "io: disc0: is served from %s (%u-byte root directory at sector %u)\n",
            path, g_iso_root.size, g_iso_root.lbn);
    return 0;
}

/* Compare one path component against a directory record's name, which is
 * upper case and may end in ";1" (and "." for a name with no extension). */
static int iso_name_eq(const uint8_t *name, int len, const char *comp, size_t clen) {
    const uint8_t *semi = memchr(name, ';', (size_t)len);
    if (semi) len = (int)(semi - name);
    if (len > 0 && name[len - 1] == '.') len--;
    if ((size_t)len != clen) return 0;
    for (int i = 0; i < len; i++)
        if (toupper(name[i]) != toupper((unsigned char)comp[i])) return 0;
    return 1;
}

/* Find a path (no device, no leading slash) on the image. */
static int iso_find(FILE *f, const char *path, iso_entry *out) {
    iso_entry cur = g_iso_root;
    uint8_t buf[SECTOR];
    while (*path == '/' || *path == '\\') path++;
    while (*path) {
        size_t clen = strcspn(path, "/\\");
        if (!cur.dir) return -1;
        int found = 0;
        for (uint32_t off = 0; off < cur.size && !found; off += SECTOR) {
            if (iso_read(f, cur.lbn + off / SECTOR, buf) != 0) return -1;
            for (uint32_t p = 0; p + 33 < SECTOR && buf[p]; p += buf[p]) {
                const uint8_t *r = buf + p;
                int nlen = r[32];
                if (nlen == 1 && r[33] <= 1) continue;   /* "." and ".." */
                if (iso_name_eq(r + 33, nlen, path, clen)) {
                    iso_record(r, &cur);
                    found = 1;
                    break;
                }
            }
        }
        if (!found) return -1;
        path += clen;
        while (*path == '/' || *path == '\\') path++;
    }
    *out = cur;
    return 0;
}

/* "sce_lbn" "0x"? HEX "_size" "0x"? HEX -- a raw sector range. */
static int parse_lbn(const char *p, uint32_t *lbn, uint32_t *size) {
    if (strncmp(p, "sce_lbn", 7)) return 0;
    const char *s = strstr(p, "_size");
    if (!s) return 0;
    *lbn  = (uint32_t)strtoul(p + 7, NULL, 16);
    *size = (uint32_t)strtoul(s + 5, NULL, 16);
    return 1;
}

/* Split off the device. Returns the part after "dev:" (leading slashes
 * dropped) and whether that device is the disc. */
static const char *split_device(const char *guest, int *is_disc) {
    *is_disc = !strncmp(guest, "disc0:", 6) || !strncmp(guest, "umd0:", 5) ||
               !strncmp(guest, "umd1:", 5);
    const char *p = strchr(guest, ':');
    p = p ? p + 1 : guest;
    while (*p == '/' || *p == '\\') p++;
    return p;
}

/* ---- paths on the host ---------------------------------------------------- */

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }

    while (*p == '/' || *p == '\\') p++;
    snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
}

static int new_handle(void) {
    for (int i = 0; i < MAX_FILES; i++)
        if (!g_file[i].used) return i;
    return -1;
}

/* Open a window of the disc image. */
static uint32_t open_disc(const char *guest, uint32_t lbn, uint64_t size) {
    int i = new_handle();
    if (i < 0) return ERR_MFILE;
    FILE *f = fopen(g_iso, "rb");
    if (!f) return ERR_NOENT;
    io_file *h = &g_file[i];
    memset(h, 0, sizeof *h);
    h->f = f;
    h->used = 1;
    h->on_disc = 1;
    h->lbn = lbn;
    h->base = (uint64_t)lbn * SECTOR;
    h->size = size;
    if (trace_io())
        fprintf(stderr, "io: open \"%s\" -> image sector 0x%X, %llu bytes: fd %d\n",
                guest, lbn, (unsigned long long)size, i + 3);
    return (uint32_t)(i + 3);
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    int is_disc;
    const char *rest = split_device(guest, &is_disc);
    uint32_t lbn, size;
    if (is_disc && parse_lbn(rest, &lbn, &size)) {
        if (!g_iso[0]) {
            fprintf(stderr, "io: open \"%s\" needs the disc image (no ISO was given): not found\n", guest);
            psp_ret(ERR_NOENT);
            return;
        }
        psp_ret(open_disc(guest, lbn, size));
        return;
    }
    if (is_disc && g_iso[0]) {
        iso_entry e;
        FILE *f = fopen(g_iso, "rb");
        int found = f && iso_find(f, rest, &e) == 0 && !e.dir;
        if (f) fclose(f);
        if (!found) {
            if (trace_io()) fprintf(stderr, "io: open \"%s\" -> image: not found\n", guest);
            psp_ret(ERR_NOENT);
            return;
        }
        psp_ret(open_disc(guest, e.lbn, e.size));
        return;
    }

    map_path(guest, host, sizeof host);

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    /* PSPRECOMP_TRACE_IO=1 names every open and whether it worked. A game
     * that cannot find its assets usually does not crash; it waits. */
    if (trace_io())
        fprintf(stderr, "io: open \"%s\" -> \"%s\" flags 0x%X: %s\n",
                guest, host, flags, f ? "ok" : "not found");
    if (!f) {
        /* A failed open is normal (a game probing for a save file) and is not
         * worth a warning, but the mapped path is worth knowing when a game
         * cannot find assets it expects. */
        psp_ret(ERR_NOENT);
        return;
    }

    int i = new_handle();
    if (i < 0) {
        fclose(f);
        psp_ret(ERR_MFILE);
        return;
    }
    memset(&g_file[i], 0, sizeof g_file[i]);
    g_file[i].f = f;
    g_file[i].used = 1;
    psp_ret((uint32_t)(i + 3));       /* 0-2 are reserved for the std streams */
}

static io_file *fd_get(uint32_t fd) {
    int32_t i = (int32_t)fd - 3;
    if (i < 0 || i >= MAX_FILES || !g_file[i].used) return NULL;
    return &g_file[i];
}

static io_file *fd_arg(void) { return fd_get(psp_arg(0)); }

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(ERR_BADF); return; }
    fclose(h->f);
    memset(h, 0, sizeof *h);
    psp_ret(0);
}

/* Read `size` bytes at the handle's position into guest memory. */
static uint32_t do_read(io_file *h, uint32_t dst, uint32_t size) {
    if (!size) return 0;
    if (h->on_disc) {
        uint64_t left = h->pos < h->size ? h->size - h->pos : 0;
        if (size > left) size = (uint32_t)left;
        if (!size) return 0;
        if (io_seek(h->f, (int64_t)(h->base + h->pos), SEEK_SET) != 0) return 0;
    }

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) return SCE_KERNEL_ERROR_NO_MEMORY;

    size_t got = fread(tmp, 1, size, h->f);
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* Fall back to byte-at-a-time so a partially mapped destination still
         * gets what fits, and the bad-access counter records the rest. */
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    }
    free(tmp);
    h->pos += got;
    g_bytes_read += got;
    return (uint32_t)got;
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(ERR_BADF); return; }
    psp_ret(do_read(h, psp_arg(1), psp_arg(2)));
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(ERR_BADF); return; }
    if (h->on_disc) { psp_ret(0x8001000D); return; }   /* EACCES: the disc is read-only */

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* Move a handle; returns the new position, or -1. */
static int64_t do_seek(io_file *h, int64_t off, uint32_t whence) {
    if (h->on_disc) {
        int64_t from = whence == 1 ? (int64_t)h->pos : whence == 2 ? (int64_t)h->size : 0;
        int64_t to = from + off;
        if (to < 0) return -1;
        h->pos = (uint64_t)to;
        return to;
    }
    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (io_seek(h->f, off, w) != 0) return -1;
    return io_tell(h->f);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    if (!h) { psp_ret(ERR_BADF); return; }
    int64_t pos = do_seek(h, (int64_t)off, psp_arg(4));
    psp_cpu.r[PSP_REG_V0] = (uint32_t)pos;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)((uint64_t)pos >> 32);
}

/* (fd, int offset, int whence) -- the 32-bit form, all in registers. */
static void hle_Lseek32(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(ERR_BADF); return; }
    psp_ret((uint32_t)do_seek(h, (int32_t)psp_arg(1), psp_arg(2)));
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : ERR_NOENT);
}

/* ---- stat ------------------------------------------------------------------
 *
 * SceIoStat: mode, attr, u64 size, then creation, access and modification
 * times (ScePspDateTime, 16 bytes each), then six private words. On a UMD the
 * first private word is the start sector. A real PSP writes only up to the
 * times on other devices, so the private words are left alone there. */

#define FIO_S_IFDIR  0x1000
#define FIO_S_IFREG  0x2000
#define FIO_SO_IFDIR 0x0010
#define FIO_SO_IFREG 0x0020

static void put_time(uint32_t at, int year, int mon, int day, int h, int m, int s) {
    psp_write16(at + 0, (uint16_t)year);
    psp_write16(at + 2, (uint16_t)mon);
    psp_write16(at + 4, (uint16_t)day);
    psp_write16(at + 6, (uint16_t)h);
    psp_write16(at + 8, (uint16_t)m);
    psp_write16(at + 10, (uint16_t)s);
    psp_write32(at + 12, 0);
}

static void put_stat(uint32_t st, int dir, uint32_t perm, uint64_t size,
                     const struct tm *t) {
    psp_write32(st + 0, (dir ? FIO_S_IFDIR : FIO_S_IFREG) | perm);
    psp_write32(st + 4, dir ? FIO_SO_IFDIR : FIO_SO_IFREG);
    psp_write32(st + 8, (uint32_t)size);
    psp_write32(st + 12, (uint32_t)(size >> 32));
    for (int k = 0; k < 3; k++)
        put_time(st + 16 + 16 * k, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
}

/* (const char *path, SceIoStat *stat) */
static void hle_Getstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t st = psp_arg(1);

    int is_disc;
    const char *rest = split_device(guest, &is_disc);
    if (!*rest && strchr(guest, ':')) { psp_ret(ERR_INVAL); return; }   /* a volume root */

    if (is_disc && g_iso[0]) {
        iso_entry e;
        FILE *f = fopen(g_iso, "rb");
        int found = f && iso_find(f, rest, &e) == 0;
        if (f) fclose(f);
        if (trace_io())
            fprintf(stderr, "io: getstat \"%s\" -> %s sector 0x%X size %u\n", guest,
                    found ? "image" : "not found", found ? e.lbn : 0, found ? e.size : 0);
        if (!found) { psp_ret(ERR_NOENT); return; }
        struct tm t = {0};
        t.tm_year = e.date[0]; t.tm_mon = e.date[1] - 1; t.tm_mday = e.date[2];
        t.tm_hour = e.date[3]; t.tm_min = e.date[4]; t.tm_sec = e.date[5];
        put_stat(st, e.dir, 0555, e.size, &t);
        psp_write32(st + 64, e.lbn);
        psp_ret(0);
        return;
    }

    map_path(guest, host, sizeof host);
    struct stat hs;
    int ok = stat(host, &hs) == 0;
    if (trace_io())
        fprintf(stderr, "io: getstat \"%s\" -> \"%s\": %s\n", guest, host, ok ? "ok" : "not found");
    if (!ok) { psp_ret(ERR_NOENT); return; }
    time_t mt = hs.st_mtime;
    struct tm *t = localtime(&mt);
    struct tm zero = {0};
    put_stat(st, S_ISDIR(hs.st_mode), 0777, (uint64_t)hs.st_size, t ? t : &zero);
    if (is_disc) psp_write32(st + 64, 0);   /* an extracted tree has no sectors */
    psp_ret(0);
}

/* ---- ioctl / devctl --------------------------------------------------------- */

/* (fd, cmd, indata, inlen, outdata, outlen) */
static void hle_Ioctl(void) {
    io_file *h = fd_arg();
    uint32_t cmd = psp_arg(1), in = psp_arg(2), inlen = psp_arg(3);
    uint32_t out = psp_arg(4), outlen = psp_arg(5);
    if (!h) { psp_ret(ERR_BADF); return; }

    switch (cmd) {
    case 0x01010005:              /* seek: { s64 offset; u32 unk; u32 whence } */
        if (!in || inlen < 4) { psp_ret(ERR_INVAL); return; }
        {
            int64_t off = (int64_t)((uint64_t)psp_read32(in) | ((uint64_t)psp_read32(in + 4) << 32));
            int64_t pos = do_seek(h, off, psp_read32(in + 12));
            psp_ret(pos < 0 ? 0x80010005 : 0);
        }
        return;
    case 0x01020006:              /* start sector of a disc file */
        if (!out || outlen < 4) { psp_ret(ERR_INVAL); return; }
        psp_write32(out, h->lbn);
        psp_ret(0);
        return;
    case 0x01020007:              /* size of a disc file, u64 */
        if (!out || outlen < 8) { psp_ret(ERR_INVAL); return; }
        {
            uint64_t size = h->size;
            if (!h->on_disc) {
                int64_t cur = io_tell(h->f);
                io_seek(h->f, 0, SEEK_END);
                size = (uint64_t)io_tell(h->f);
                io_seek(h->f, cur, SEEK_SET);
            }
            psp_write32(out, (uint32_t)size);
            psp_write32(out + 4, (uint32_t)(size >> 32));
        }
        psp_ret(0);
        return;
    case 0x01030008:              /* read: in = { u32 size } */
        if (!in || inlen < 4) { psp_ret(ERR_INVAL); return; }
        {
            uint32_t size = psp_read32(in);
            if (size > outlen) { psp_ret(ERR_INVAL); return; }
            psp_ret(do_read(h, out, size));
        }
        return;
    case 0x01F010DB:              /* UMD read-ahead hint */
        psp_ret(0);
        return;
    }
    fprintf(stderr, "io: ioctl fd %u cmd 0x%08X (in 0x%08X/%u, out 0x%08X/%u) not implemented\n",
            psp_arg(0), cmd, in, inlen, out, outlen);
    psp_ret(0);
}

#define MS_FREE_BYTES (1024ull * 1024 * 1024)

/* (const char *dev, cmd, indata, inlen, outdata, outlen) */
static void hle_Devctl(void) {
    char dev[64];
    psp_str(psp_arg(0), dev, sizeof dev);
    uint32_t cmd = psp_arg(1), in = psp_arg(2), inlen = psp_arg(3);
    uint32_t out = psp_arg(4), outlen = psp_arg(5);

    switch (cmd) {
    case 0x01F20001:              /* UMD: disc type, second word */
        if (out && outlen >= 8) { psp_write32(out, 0); psp_write32(out + 4, 0x10); }
        psp_ret(0);
        return;
    case 0x01F20002:              /* UMD: current sector */
        if (out && outlen >= 4) psp_write32(out, 0x10);
        psp_ret(0);
        return;
    case 0x02025801:              /* memory stick: driver state, 4 = inserted */
        if (out && outlen >= 4) psp_write32(out, 4);
        psp_ret(0);
        return;
    case 0x02025806:              /* memory stick: present */
        if (out && outlen >= 4) psp_write32(out, 1);
        psp_ret(0);
        return;
    case 0x02015804:              /* memory stick: register the insert/eject callback */
    case 0x02415821:
        /* in = { callback uid }. Registering reports the current state at
         * once, as the firmware does (and PPSSPP): 1, inserted. Games wait on
         * that notification before touching savedata -- MHFU sits on
         * "Checking for Memory Stick Duo" without it. */
        if (in && inlen >= 4) {
            uint32_t cb = psp_read32(in);
            int ok = psp_callback_notify(cb, 1) == 0;
            fprintf(stderr, "io: devctl \"%s\" 0x%08X: memory stick callback 0x%X %s\n",
                    dev, cmd, cb, ok ? "notified: inserted" : "is not a callback");
        }
        psp_ret(0);
        return;
    case 0x02015805:              /* ...and unregister it */
    case 0x02415822:
        psp_ret(0);
        return;
    case 0x02425818:              /* free space; in = { DeviceSize *ptr } */
        if (!in || inlen < 4) { psp_ret(ERR_INVAL); return; }
        {
            uint32_t p = psp_read32(in);
            uint32_t sector = 0x200, per = 64;
            uint32_t clusters = (uint32_t)(MS_FREE_BYTES / (sector * per));
            psp_write32(p + 0, clusters);    /* maxClusters */
            psp_write32(p + 4, clusters);    /* freeClusters */
            psp_write32(p + 8, clusters);    /* maxSectors */
            psp_write32(p + 12, sector);
            psp_write32(p + 16, per);
        }
        psp_ret(0);
        return;
    case 0x02425823:              /* write protected? */
    case 0x02425824:
        if (out && outlen >= 4) psp_write32(out, 0);
        psp_ret(0);
        return;
    }
    fprintf(stderr, "io: devctl \"%s\" cmd 0x%08X (in 0x%08X/%u, out 0x%08X/%u) not implemented\n",
            dev, cmd, in, inlen, out, outlen);
    psp_ret(0);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(ERR_NOENT); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(ERR_NOENT); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(ERR_MFILE);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(ERR_BADF); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: a 52-byte SceIoStat, then char d_name[256]. */
    for (int i = 0; i < 52; i++) psp_write8(dirent + (uint32_t)i, 0);
    uint32_t at = dirent + 52;
    for (uint32_t i = 0; i < 255 && name[i]; i++) psp_write8(at + i, (uint8_t)name[i]);
    psp_write8(at + (uint32_t)strlen(name), 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(ERR_BADF); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

void psp_io_register(void) {
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",    hle_Open);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",   hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",    hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",   hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",   hle_Lseek);
    psp_hle_register(0x68963324, "IoFileMgrForUser", "sceIoLseek32", hle_Lseek32);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename",  hle_Rename);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat", hle_Getstat);
    psp_hle_register(0x63632449, "IoFileMgrForUser", "sceIoIoctl",   hle_Ioctl);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl",  hle_Devctl);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",   hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",   hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose",  hle_Dclose);
}
