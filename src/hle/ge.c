/* psprecomp — sceGe_user.
 *
 * The GE is the PSP's GPU. It is not driven by function calls: user code builds
 * a **display list** — an array of 32-bit words, each an 8-bit command and 24
 * bits of argument — and hands the GE a pointer plus a *stall address*. The GE
 * consumes commands up to the stall, and the CPU moves the stall forward as it
 * writes more. That producer/consumer arrangement is the whole API.
 *
 * So `sceGu*` (the list-building library) is ordinary user code and gets
 * recompiled like anything else. Only list *execution* is emulated, and that is
 * this file.
 *
 * ## What this does and does not do
 *
 * It walks the list, follows control flow (JUMP/CALL/RET/END/FINISH), tracks
 * the state commands that matter, decodes and transforms vertices, and hands
 * assembled primitives plus a psp_draw_state to the render backend, which
 * rasterizes them (src/render.c). See the rasterizer section below for what
 * is and is not decoded.
 *
 * psp_ge_dump_stats() reports what the game asked for, including every
 * command not decoded, by opcode, with the last argument it carried. During
 * bring-up that census answers "what is the game drawing, and with what state"
 * before any pixel is trusted.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Display-list opcodes. Only the ones the walk needs to be correct about are
 * named; everything else is counted rather than guessed at, because a
 * misidentified state command silently changes rendering. */
#define GE_NOP          0x00
#define GE_VADDR        0x01
#define GE_IADDR        0x02
#define GE_PRIM         0x04
#define GE_BEZIER       0x05
#define GE_SPLINE       0x06
#define GE_JUMP         0x08
#define GE_BJUMP        0x09
#define GE_CALL         0x0A
#define GE_RET          0x0B
#define GE_END          0x0C
#define GE_SIGNAL       0x0E
#define GE_FINISH       0x0F
#define GE_BASE         0x10
#define GE_VTYPE        0x12
#define GE_OFFSET_ADDR  0x13
#define GE_ORIGIN_ADDR  0x14
#define GE_TME          0x1E
#define GE_ABE          0x21
#define GE_ATE          0x22
#define GE_WMS          0x3A
#define GE_WORLD        0x3B
#define GE_VMS          0x3C
#define GE_VIEW         0x3D
#define GE_PMS          0x3E
#define GE_PROJ         0x3F
#define GE_XSCALE       0x42
#define GE_YSCALE       0x43
#define GE_XPOS         0x45
#define GE_YPOS         0x46
#define GE_USCALE       0x48
#define GE_VSCALE       0x49
#define GE_UOFFSET      0x4A
#define GE_VOFFSET      0x4B
#define GE_OFFSETX      0x4C
#define GE_OFFSETY      0x4D
#define GE_SHADE        0x50
#define GE_AMC          0x55
#define GE_AMA          0x58
#define GE_FBP          0x9C
#define GE_FBW          0x9D
#define GE_TBP0         0xA0
#define GE_TBW0         0xA8
#define GE_CBP          0xB0
#define GE_CBPH         0xB1
#define GE_TSIZE0       0xB8
#define GE_TMODE        0xC2
#define GE_TPSM         0xC3
#define GE_CLOAD        0xC4
#define GE_CMODE        0xC5
#define GE_TWRAP        0xC7
#define GE_TFUNC        0xC9
#define GE_TEC          0xCA
#define GE_PSM          0xD2
#define GE_CLEAR        0xD3
#define GE_SCISSOR1     0xD4
#define GE_SCISSOR2     0xD5
#define GE_ATST         0xDB
#define GE_ALPHA        0xDF
#define GE_SFIX         0xE0
#define GE_DFIX         0xE1

#define MAX_QUEUES 8
#define GE_STACK   8

/* Primitive types, from the PRIM argument's type field. */
static const char *const PRIM_NAME[8] = {
    "points", "lines", "line-strip", "triangles",
    "triangle-strip", "triangle-fan", "sprites", "?"
};

typedef struct {
    uint32_t id;
    uint32_t list;      /* current read pointer */
    uint32_t stall;     /* stop before this address; 0 means "no stall" */
    uint32_t base;      /* GE_BASE: high bits for addresses */
    uint32_t origin;
    int      used;
    int      done;
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];
static uint32_t g_next_id;

static uint64_t g_unsupported;     /* vertices in a format we do not read */

/* Draw state handed to the backend with every primitive, and the raw words it
 * is decoded from where one value spans two commands. */
static psp_draw_state g_ds;
static uint32_t g_tbp, g_tbw, g_cbp, g_psm;
static uint8_t  g_clut[1024];      /* the GE's CLUT cache, filled by CLOAD */
/* A vertex with no colour takes the material colour: AMC's RGB, AMA's alpha. */
static uint32_t g_material = 0xFFFFFFFFu;

/* The transform pipeline. Matrices are uploaded one float per command, column
 * by column: world and view are 4x3 (the last column is the translation),
 * projection is 4x4. The viewport maps clip space into the GE's 4096-wide
 * drawing space, and the offset moves the screen's top-left within it. */
static struct {
    float world[12], view[12], proj[16];
    int   wi, vi, pi;
    float xscale, yscale, xpos, ypos;
    float offx, offy;                   /* pixels */
    float uscale, vscale, uoff, voff;
    uint64_t behind;                    /* primitives dropped with w <= 0 */
} g_xf;

/* GE floats are the top 24 bits of an IEEE single. */
static float f24(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

static void xf_reset(void) {
    memset(&g_xf, 0, sizeof g_xf);
    g_xf.world[0] = g_xf.world[4] = g_xf.world[8] = 1;
    g_xf.view[0] = g_xf.view[4] = g_xf.view[8] = 1;
    g_xf.proj[0] = g_xf.proj[5] = g_xf.proj[10] = g_xf.proj[15] = 1;
    g_xf.uscale = g_xf.vscale = 1;
}

/* Model space to screen pixels. Returns 0 for a point behind the eye.
 * ponytail: no near-plane clipping, so a primitive with any vertex behind the
 * eye is dropped whole; add clipping when 3D scenes lose triangles. */
static int transform(const float p[3], float *sx, float *sy) {
    const float *W = g_xf.world, *V = g_xf.view, *P = g_xf.proj;
    float w[3], v[3], c[4];
    for (int r = 0; r < 3; r++)
        w[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r];
    for (int r = 0; r < 3; r++)
        v[r] = V[r] * w[0] + V[3 + r] * w[1] + V[6 + r] * w[2] + V[9 + r];
    for (int r = 0; r < 4; r++)
        c[r] = P[r] * v[0] + P[4 + r] * v[1] + P[8 + r] * v[2] + P[12 + r];
    if (c[3] <= 0) return 0;
    *sx = c[0] / c[3] * g_xf.xscale + g_xf.xpos - g_xf.offx;
    *sy = c[1] / c[3] * g_xf.yscale + g_xf.ypos - g_xf.offy;
    return 1;
}

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr;
    uint64_t commands;
    uint64_t prims[8];
    uint64_t vertices;
    uint64_t unknown;
    uint64_t lists;
    uint64_t finishes;
    uint64_t op_count[256];     /* undecoded commands, by opcode */
    uint32_t op_last[256];      /* and the last argument each one carried */
} g_ge;

void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    memset(&g_ge, 0, sizeof g_ge);
    memset(&g_ds, 0, sizeof g_ds);
    memset(g_clut, 0, sizeof g_clut);
    g_tbp = g_tbw = g_cbp = 0;
    g_psm = 3;
    g_material = 0xFFFFFFFFu;
    g_ds.clut = g_clut;
    g_ds.clut_mask = 0xFF;
    g_ds.atest_func = 1;
    g_ds.atest_mask = 0xFF;
    g_ds.tex_w = g_ds.tex_h = 1;
    g_ds.tex_alpha = 1;
    g_ds.sc_x1 = 479;
    g_ds.sc_y1 = 271;
    xf_reset();
    psp_render_reset_pixels();
    g_unsupported = 0;
    g_next_id = 0x00080000u;
}

void psp_ge_init(void) { psp_ge_reset(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    fprintf(out, "    framebuffer 0x%08X stride %u, vertex type 0x%06X\n",
            g_ge.fbp, g_ge.fbw, g_ge.vtype);
    fprintf(out, "    vertices submitted: %llu\n", (unsigned long long)g_ge.vertices);
    for (int i = 0; i < 8; i++)
        if (g_ge.prims[i])
            fprintf(out, "    %-15s %llu\n", PRIM_NAME[i], (unsigned long long)g_ge.prims[i]);
    if (g_ge.unknown) {
        fprintf(out, "    %llu commands not individually decoded:\n",
                (unsigned long long)g_ge.unknown);
        for (int i = 0; i < 256; i++)
            if (g_ge.op_count[i])
                fprintf(out, "      0x%02X x%-8llu last arg 0x%06X\n", i,
                        (unsigned long long)g_ge.op_count[i], g_ge.op_last[i]);
    }
    fprintf(out, "    pixels written: %llu\n", (unsigned long long)psp_render_pixels());
    if (g_xf.behind)
        fprintf(out, "    %llu primitives dropped with a vertex behind the eye (no clipping)\n",
                (unsigned long long)g_xf.behind);
    if (psp_render_unsupported_texels())
        fprintf(out, "    %llu texels in an undecoded format (DXT), drawn magenta\n",
                (unsigned long long)psp_render_unsupported_texels());
    if (g_unsupported)
        fprintf(out, "    %llu vertices in an unsupported format (skinned, or no position)\n",
                (unsigned long long)g_unsupported);
}

uint64_t psp_ge_command_count(void) { return g_ge.commands; }
uint64_t psp_ge_vertex_count(void)  { return g_ge.vertices; }

/* ---- rasterizer ----------------------------------------------------------
 *
 * Enough of the GE to put pixels in the framebuffer. Deliberately narrow:
 *
 *  - "through" mode (VTYPE bit 23), where vertex coordinates are already in
 *    screen space and texture coordinates are texels, and transformed mode,
 *    where positions go through world, view and projection and the viewport,
 *    and texture coordinates are normalised and scaled by UV scale/offset.
 *    Lighting, skinning, morphing and the texture matrix are not done.
 *  - Position as 16-bit or float; colour as 8888, the 16-bit formats, or
 *    none; texture coordinates as 16-bit or float texels.
 *  - Textures, CLUTs, the texture function, alpha test, blending, clear mode
 *    and scissor are decoded into psp_draw_state for the backend. No depth.
 *
 * The point is to close the loop from display list to visible pixels so the
 * rest can be measured against something. Everything omitted is omitted
 * loudly: unsupported vertex formats are counted, not guessed at.
 */

/* VTYPE field extraction. */
#define VT_TEX(v)     ((v) & 3)
#define VT_COLOR(v)   (((v) >> 2) & 7)
#define VT_NORMAL(v)  (((v) >> 5) & 3)
#define VT_POS(v)     (((v) >> 7) & 3)
#define VT_WEIGHT(v)  (((v) >> 9) & 3)
#define VT_INDEX(v)   (((v) >> 11) & 3)
#define VT_THROUGH(v) (((v) >> 23) & 1)


uint64_t psp_ge_pixels(void) { return psp_render_pixels(); }

/* Size of one vertex in bytes, and the offsets within it. Components appear in
 * a fixed order (weights, texture, colour, normal, position) and each is
 * aligned to its own size, which is what makes the stride awkward enough to be
 * worth computing rather than assuming. */
static int vertex_layout(uint32_t vtype, int *tex_off, int *col_off, int *pos_off) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };

    int off = 0, align = 1;
    int t = tex_sz[VT_TEX(vtype)] * 2;
    int c = col_sz[VT_COLOR(vtype)];
    int n = norm_sz[VT_NORMAL(vtype)] * 3;
    int p = pos_sz[VT_POS(vtype)] * 3;

    if (VT_WEIGHT(vtype)) return 0;          /* skinning: not handled */

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); *tex_off = off; off += t; if (ts > align) align = ts; }
    else *tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); *col_off = off; off += c; if (cs > align) align = cs; }
    else *col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); off += n; if (ns > align) align = ns; }
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); *pos_off = off; off += p;
    if (ps > align) align = ps;

    return (off + align - 1) & ~(align - 1);  /* stride */
}

static int read_vertex(uint32_t addr, uint32_t vtype, int tex_off, int col_off,
                       int pos_off, psp_vertex *out, float pos[3]) {
    /* Through mode reads 8/16-bit values as integers; transformed mode reads
     * them as fixed point, signed for positions. */
    const int through = (int)VT_THROUGH(vtype);
    out->rgba = g_material;
    if (col_off >= 0) {
        if (VT_COLOR(vtype) == 7)
            out->rgba = psp_read32(addr + (uint32_t)col_off);
        else    /* 4, 5, 6: 565, 5551, 4444 -- the texture formats, numbered from 4 */
            out->rgba = psp_color_expand(psp_read16(addr + (uint32_t)col_off),
                                         (int)VT_COLOR(vtype) - 4);
    }

    out->u = out->v = 0;
    if (tex_off >= 0) {
        uint32_t t = addr + (uint32_t)tex_off;
        switch (VT_TEX(vtype)) {
        case 1: out->u = psp_read8(t);  out->v = psp_read8(t + 1);
                if (!through) { out->u /= 128.0f; out->v /= 128.0f; }
                break;
        case 2: out->u = psp_read16(t); out->v = psp_read16(t + 2);
                if (!through) { out->u /= 32768.0f; out->v /= 32768.0f; }
                break;
        default: out->u = psp_read_f32(t); out->v = psp_read_f32(t + 4); break;
        }
        if (!through) {     /* normalised, scaled, then in texels */
            out->u = (out->u * g_xf.uscale + g_xf.uoff) * (float)g_ds.tex_w;
            out->v = (out->v * g_xf.vscale + g_xf.voff) * (float)g_ds.tex_h;
        }
    }

    const uint32_t a = addr + (uint32_t)pos_off;
    switch (VT_POS(vtype)) {
    case 1:   /* 8-bit */
        for (int i = 0; i < 3; i++) pos[i] = (float)(int8_t)psp_read8(a + (uint32_t)i);
        if (!through) for (int i = 0; i < 3; i++) pos[i] /= 128.0f;
        break;
    case 2:   /* 16-bit */
        for (int i = 0; i < 3; i++) pos[i] = (float)(int16_t)psp_read16(a + (uint32_t)i * 2);
        if (!through) for (int i = 0; i < 3; i++) pos[i] /= 32768.0f;
        break;
    case 3:   /* float */
        for (int i = 0; i < 3; i++) pos[i] = psp_read_f32(a + (uint32_t)i * 4);
        break;
    default:
        return 0;
    }
    if (through) {
        out->x = (int)pos[0];
        out->y = (int)pos[1];
        return 1;
    }
    float sx, sy;
    if (!transform(pos, &sx, &sy)) return -1;
    out->x = (int)floorf(sx);
    out->y = (int)floorf(sy);
    return 1;
}


static void draw_prim(uint32_t type, uint32_t count) {
    if (!g_ge.vaddr) { g_unsupported += count; return; }

    int tex_off = -1, col_off = -1, pos_off = 0;
    int stride = vertex_layout(g_ge.vtype, &tex_off, &col_off, &pos_off);
    if (!stride) { g_unsupported += count; return; }

    /* Decode the whole batch, then hand it to the backend in one call.
     *
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. Wrong once,
     * centrally, is at least diagnosable. */
    enum { BATCH = 256 };
    psp_vertex v[BATCH];
    const psp_render_backend *be = psp_render_current();

    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > BATCH) n = BATCH;

        /* Strips are order-dependent, so a batch boundary must overlap by two
         * vertices or the triangle spanning it is lost. */
        uint32_t decoded = 0;
        int behind = 0;
        for (; decoded < n; decoded++) {
            float pos[3];
            int ok = read_vertex(g_ge.vaddr + (done + decoded) * (uint32_t)stride,
                                 g_ge.vtype, tex_off, col_off, pos_off, &v[decoded], pos);
            if (!ok) break;
            if (ok < 0) behind = 1;
        }
        if (!decoded) break;
        if (behind) { g_xf.behind++; done += decoded; continue; }

        be->draw((int)type, v, (int)decoded, &g_ds);

        if (type == 4 && decoded == BATCH && done + decoded < count)
            done += decoded - 2;     /* strip overlap */
        else
            done += decoded;
    }
}

/* The framebuffer is always in eDRAM. Games send FBP relative to it, so a high
 * byte of 0 is the norm, and an absolute 0x04xxxxxx folds to the same place.
 * Taken literally, 0x88000 is main RAM: for a vaddr-0 module, its own code. */
static uint32_t fb_vram(void) { return PSP_VRAM_BASE | (g_ge.fbp & (PSP_VRAM_SIZE - 1)); }

/* Walk a list until END/FINISH, the stall address, or a step budget.
 *
 * The budget is not paranoia: a list whose JUMP forms a cycle is a normal
 * intermediate state while the CPU is still writing, and without a bound a
 * malformed or partially-written list hangs the host with no diagnostic. */
static void run_list(ge_queue *q) {
    uint32_t stack[GE_STACK];
    int sp = 0;
    uint64_t budget = 1u << 22;

    g_ge.lists++;

    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */

        uint32_t word = psp_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        q->list += 4;
        g_ge.commands++;

        switch (cmd) {
        case GE_NOP:
            break;

        case GE_PRIM: {
            uint32_t type  = (arg >> 16) & 7;
            uint32_t count = arg & 0xFFFF;
            g_ge.prims[type]++;
            g_ge.vertices += count;
            draw_prim(type, count);
            break;
        }
        case GE_BEZIER:
        case GE_SPLINE:
            /* Patches expand to triangles on hardware; counted as their own
             * thing rather than folded into the triangle count. */
            g_ge.prims[3]++;
            break;

        case GE_JUMP:
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_CALL:
            if (sp < GE_STACK) stack[sp++] = q->list;
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_RET:
            if (sp > 0) q->list = stack[--sp];
            break;
        case GE_BJUMP:
            /* Conditional on the bounding-box test, which needs geometry we do
             * not process. Not taking it means we walk the enclosed commands
             * rather than skipping them -- the conservative direction, since
             * skipping would under-report what the game drew. */
            break;

        case GE_END:
        case GE_FINISH:
            if (cmd == GE_FINISH) g_ge.finishes++;
            q->done = 1;
            return;

        case GE_SIGNAL:
            /* Raises a callback on hardware. Callbacks are not delivered yet
             * (no scheduler), so this is recorded and ignored. */
            break;

        case GE_BASE:        q->base = (arg & 0xFF0000) << 8; break;
        case GE_ORIGIN_ADDR: q->origin = q->list - 4; break;
        case GE_OFFSET_ADDR: q->base = arg << 8; break;

        case GE_VTYPE: g_ge.vtype = arg; break;
        case GE_FBP:
            g_ge.fbp = (g_ge.fbp & 0xFF000000u) | arg;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;
        case GE_FBW:
            g_ge.fbw = arg & 0xFFFF;
            g_ge.fbp = (g_ge.fbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case GE_PSM:
            g_psm = arg & 3;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case GE_WMS:   g_xf.wi = (int)(arg & 0xF); break;
        case GE_WORLD: if (g_xf.wi < 12) g_xf.world[g_xf.wi++] = f24(arg); break;
        case GE_VMS:   g_xf.vi = (int)(arg & 0xF); break;
        case GE_VIEW:  if (g_xf.vi < 12) g_xf.view[g_xf.vi++] = f24(arg); break;
        case GE_PMS:   g_xf.pi = (int)(arg & 0xF); break;
        case GE_PROJ:  if (g_xf.pi < 16) g_xf.proj[g_xf.pi++] = f24(arg); break;
        case GE_XSCALE:  g_xf.xscale = f24(arg); break;
        case GE_YSCALE:  g_xf.yscale = f24(arg); break;
        case GE_XPOS:    g_xf.xpos = f24(arg); break;
        case GE_YPOS:    g_xf.ypos = f24(arg); break;
        case GE_USCALE:  g_xf.uscale = f24(arg); break;
        case GE_VSCALE:  g_xf.vscale = f24(arg); break;
        case GE_UOFFSET: g_xf.uoff = f24(arg); break;
        case GE_VOFFSET: g_xf.voff = f24(arg); break;
        case GE_OFFSETX: g_xf.offx = (float)(arg & 0xFFFF) / 16.0f; break;   /* 12.4 */
        case GE_OFFSETY: g_xf.offy = (float)(arg & 0xFFFF) / 16.0f; break;

        case GE_TME:   g_ds.tex_enable = arg & 1; break;
        case GE_ABE:   g_ds.blend = arg & 1; break;
        case GE_ATE:   g_ds.atest = arg & 1; break;
        case GE_SHADE: g_ds.gouraud = arg & 1; break;
        case GE_AMC:   g_material = (g_material & 0xFF000000u) | arg; break;
        case GE_AMA:   g_material = (g_material & 0x00FFFFFFu) | ((arg & 0xFF) << 24); break;
        case GE_CLEAR:
            g_ds.clear = arg & 1;
            g_ds.clear_color = (arg >> 8) & 1;
            g_ds.clear_alpha = (arg >> 9) & 1;
            break;

        /* TBP0 carries the low 24 bits and TBW0 bits 24-31 in its top byte. */
        case GE_TBP0:
            g_tbp = arg;
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TBW0:
            g_tbw = arg;
            g_ds.tex_bw = (int)(arg & 0xFFFF);
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TSIZE0:
            g_ds.tex_w = 1 << (arg & 0xF);
            g_ds.tex_h = 1 << ((arg >> 8) & 0xF);
            break;
        case GE_TMODE: g_ds.tex_swizzle = arg & 1; break;
        case GE_TPSM:  g_ds.tex_fmt = (int)(arg & 0xF); break;
        case GE_TWRAP:
            g_ds.tex_clamp_u = arg & 1;
            g_ds.tex_clamp_v = (arg >> 8) & 1;
            break;
        case GE_TFUNC:
            g_ds.tex_func = (int)(arg & 7);
            g_ds.tex_alpha = (arg >> 8) & 1;
            g_ds.tex_double = (arg >> 16) & 1;
            break;
        case GE_TEC:   g_ds.tex_env = arg; break;

        /* The CLUT is copied into the GE when CLOAD runs, so a game may
         * rewrite the memory afterwards. CBPH carries bits 24-27. */
        case GE_CBP:   g_cbp = (g_cbp & 0xFF000000u) | arg; break;
        case GE_CBPH:  g_cbp = (g_cbp & 0x00FFFFFFu) | ((arg & 0x0F0000u) << 8); break;
        case GE_CLOAD: {
            uint32_t n = (arg & 0x3F) * 32u;
            if (n > sizeof g_clut) n = sizeof g_clut;
            for (uint32_t i = 0; i < n; i++) g_clut[i] = psp_read8(g_cbp + i);
            break;
        }
        case GE_CMODE:
            g_ds.clut_fmt = (int)(arg & 3);
            g_ds.clut_shift = (int)((arg >> 2) & 0x1F);
            g_ds.clut_mask = (int)((arg >> 8) & 0xFF);
            g_ds.clut_start = (int)((arg >> 16) & 0x1F);
            break;

        case GE_SCISSOR1:
            g_ds.sc_x0 = (int)(arg & 0x3FF);
            g_ds.sc_y0 = (int)((arg >> 10) & 0x3FF);
            break;
        case GE_SCISSOR2:
            g_ds.sc_x1 = (int)(arg & 0x3FF);
            g_ds.sc_y1 = (int)((arg >> 10) & 0x3FF);
            break;

        case GE_ATST:
            g_ds.atest_func = (int)(arg & 7);
            g_ds.atest_ref = (int)((arg >> 8) & 0xFF);
            g_ds.atest_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_ALPHA:
            g_ds.blend_src = (int)(arg & 0xF);
            g_ds.blend_dst = (int)((arg >> 4) & 0xF);
            g_ds.blend_op = (int)((arg >> 8) & 0xF);
            break;
        case GE_SFIX:  g_ds.blend_sfix = arg; break;
        case GE_DFIX:  g_ds.blend_dfix = arg; break;

        case GE_VADDR: g_ge.vaddr = (q->base | (arg & 0xFFFFFF)); break;
        case GE_IADDR: break;

        default:
            /* A real state command we do not decode individually. Counted --
             * and, for the first few, named. "240 commands not individually
             * decoded" hides whether the game is configuring a draw or just
             * poking state, which is the difference between a rendering bug
             * and a game that has not asked to render yet. */
            g_ge.unknown++;
            g_ge.op_count[cmd]++;
            g_ge.op_last[cmd] = arg;
            break;
        }
    }
}

/* ---- the calls ----------------------------------------------------------- */

static ge_queue *find_queue(uint32_t id) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].id == id) return &g_queue[i];
    return NULL;
}

static void enqueue(int head) {
    /* (list, stall, cbid, arg) */
    ge_queue *q = NULL;
    for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }
    if (!q) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(q, 0, sizeof *q);
    q->id    = g_next_id++;
    q->list  = psp_arg(0) & ~3u;
    q->stall = psp_arg(1) & ~3u;
    q->used  = 1;
    (void)head;

    /* Hardware runs the list asynchronously. We run it here and finish before
     * returning, which is indistinguishable from the game's point of view
     * because every way it can observe progress -- ListSync, DrawSync -- then
     * reports completion. */
    run_list(q);
    psp_ret(q->id);
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    ge_queue *q = find_queue(psp_arg(0));
    if (!q) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    q->stall = psp_arg(1) & ~3u;
    if (!q->done) run_list(q);          /* the new stall released more commands */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Lists are complete by the time they are enqueued, so every sync succeeds
 * immediately. */
static void hle_ListSync(void) { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_DrawSync(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_Break(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Continue(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_SetCallback(void)   { psp_ret(0); }
static void hle_UnsetCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* eDRAM is the GPU-visible VRAM window: 2 MB at 0x04000000. */
static void hle_EdramGetAddr(void) { psp_ret(PSP_VRAM_BASE); }
static void hle_EdramGetSize(void) { psp_ret(PSP_VRAM_SIZE); }

void psp_ge_register(void) {
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueueHead);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_ListSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_DrawSync);
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_Break);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",            hle_Continue);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr",        hle_EdramGetAddr);
    psp_hle_register(0x1F6752AD, "sceGe_user", "sceGeEdramGetSize",        hle_EdramGetSize);
}
