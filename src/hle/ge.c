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
#include "psprecomp/dispatch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#define GE_ZTE          0x23
#define GE_BOFS         0x2A
#define GE_BONE         0x2B
#define GE_MW0          0x2C
#define GE_ZSCALE       0x44
#define GE_ZPOS         0x47
#define GE_ZBP          0x9E
#define GE_ZBW          0x9F
#define GE_ZTST         0xDE
#define GE_ZMSK         0xE7
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
/* Lighting, texture generation and fog (PPSSPP's ge_constants.h names). */
#define GE_LTE          0x17
#define GE_LTE0         0x18
#define GE_FGE          0x1F
#define GE_TMS          0x40
#define GE_TMATRIX      0x41
#define GE_RNORM        0x51
#define GE_CMAT         0x53
#define GE_EMC          0x54
#define GE_DMC          0x56
#define GE_SMC          0x57
#define GE_SPOW         0x5B
#define GE_ALC          0x5C
#define GE_ALA          0x5D
#define GE_LT0          0x5F
#define GE_LXP0         0x63
#define GE_LXD0         0x6F
#define GE_LCA0         0x7B
#define GE_SPOTEXP0     0x87
#define GE_SPOTCUT0     0x8B
#define GE_LAC0         0x8F
#define GE_TMAP         0xC0
#define GE_TSHADE       0xC1
#define GE_FFAR         0xCD
#define GE_FDIST        0xCE
#define GE_FCOL         0xCF
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
    uint32_t offset;    /* OFFSET_ADDR / ORIGIN_ADDR: added to every address */
    int      used;
    int      done;
    int32_t  cbid;      /* sceGeSetCallback id from EnQueue, -1 for none */
    /* CALL stack: return address, offset and base per level. It belongs to
     * the list, not to one run of it: a list that stalls inside a called
     * sub-list resumes there later, and its RET must still find the way back.
     * Kept local, a resumed list ignored that RET and ran on into whatever
     * followed (MHFU's village: model data in VRAM, executed as commands). */
    uint32_t stack[GE_STACK][3];
    int      sp;
    uint32_t prev;      /* the previous command word: END reads it (SIGNAL) */
    uint32_t start;     /* where EnQueue pointed it, for diagnostics */
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];

/* Block transfer (TRXKICK): a rectangle of pixels copied from one buffer to
 * another, the GE's own DMA. Registers as PPSSPP's GPUState.h. MHFU uploads
 * model data into VRAM with it -- display lists included, which its lists
 * then CALL; without it those were empty, and the GE ran a megabyte of zeros
 * into whatever followed. */
#define GE_TRXSBP   0xB2
#define GE_TRXSBW   0xB3
#define GE_TRXDBP   0xB4
#define GE_TRXDBW   0xB5
#define GE_TRXKICK  0xEA
#define GE_TRXSPOS  0xEB
#define GE_TRXDPOS  0xEC
#define GE_TRXSIZE  0xEE
static struct { uint32_t sbp, sbw, dbp, dbw, spos, dpos, size; } g_trx;
static uint64_t g_trx_count, g_trx_bytes;

static void trx_kick(uint32_t arg) {
    const uint32_t src = (g_trx.sbp & 0xFFFFF0u) | ((g_trx.sbw & 0xFF0000u) << 8);
    const uint32_t dst = (g_trx.dbp & 0xFFFFF0u) | ((g_trx.dbw & 0xFF0000u) << 8);
    uint32_t sstride = g_trx.sbw & 0x7F8u, dstride = g_trx.dbw & 0x7F8u;
    if (sstride > 0x400) sstride = 0;
    if (dstride > 0x400) dstride = 0;
    const uint32_t sx = g_trx.spos & 0x3FF, sy = (g_trx.spos >> 10) & 0x3FF;
    const uint32_t dx = g_trx.dpos & 0x3FF, dy = (g_trx.dpos >> 10) & 0x3FF;
    const uint32_t w = (g_trx.size & 0x3FF) + 1, h = ((g_trx.size >> 10) & 0x3FF) + 1;
    const uint32_t bpp = (arg & 1) ? 4 : 2;
    g_trx_count++;
    if (getenv("PSPRECOMP_TRACE_TRX"))
        fprintf(stderr, "ge: block transfer 0x%08X (%u,%u) stride %u -> 0x%08X (%u,%u) stride %u, %ux%u x%u bytes\n",
                src, sx, sy, sstride, dst, dx, dy, dstride, w, h, bpp);
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t from = src + ((sy + y) * sstride + sx) * bpp;
        const uint32_t to   = dst + ((dy + y) * dstride + dx) * bpp;
        const void *s = psp_mem_ptr(from, w * bpp);
        void *d = psp_mem_ptr(to, w * bpp);
        if (!s || !d) break;
        memmove(d, s, w * bpp);
        g_trx_bytes += w * bpp;
    }
}

/* The last control transfers of the lists, printed when one jumps into
 * nothing: how it got there is the question, not where it ended. */
static struct { uint32_t from, to; char kind; } g_flow[16];
static int g_nflow;
static FILE *g_dump;
static int dumping(void);
static void flow(uint32_t from, uint32_t to, char kind) {
    if (g_dump && dumping()) fprintf(g_dump, "  flow %c 0x%08X -> 0x%08X\n", kind, from, to);
    g_flow[g_nflow % 16].from = from;
    g_flow[g_nflow % 16].to = to;
    g_flow[g_nflow % 16].kind = kind;
    g_nflow++;
}
static uint32_t g_base;     /* BASE is GE state, not per list */
static uint32_t g_next_id;

/* ---- callbacks ---------------------------------------------------------------
 *
 * sceGeSetCallback registers a signal and a finish handler; a list enqueued
 * with that callback id raises the finish handler when it reaches FINISH, and
 * the signal handler at a SIGNAL whose behaviour asks for one. Games wait on
 * these: MHFU clears a byte, enqueues its frame list, and spins on the byte
 * until its finish handler sets it -- a loop with no call in it, so no wait
 * point would ever deliver anything.
 *
 * Lists run to completion inside EnQueue, so the interrupt is raised there.
 * It is delivered as soon as the raising thread has interrupts enabled: at
 * once, or when it leaves its CpuSuspendIntr bracket, or at the next wait. */

#define MAX_GE_CB      16
#define MAX_GE_PENDING 32

typedef struct { int used; uint32_t signal_func, signal_arg, finish_func, finish_arg; } ge_cb;
typedef struct { uint32_t func, a0, a1, a2; } ge_intr;

static ge_cb    g_cb[MAX_GE_CB];
static ge_intr  g_pending[MAX_GE_PENDING];
static int      g_pending_n;
static int      g_ge_delivering;
static uint64_t g_ge_callbacks_run;

static void raise_intr(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!func) return;
    if (g_pending_n == MAX_GE_PENDING) {
        fprintf(stderr, "psprecomp: GE callback queue full, dropping 0x%08X\n", func);
        return;
    }
    g_pending[g_pending_n++] = (ge_intr){ func, a0, a1, a2 };
}

void psp_ge_deliver_pending(void) {
    if (g_ge_delivering || !g_pending_n || psp_threadman_intr_disabled()) return;
    g_ge_delivering = 1;
    while (g_pending_n) {
        ge_intr in = g_pending[0];
        memmove(g_pending, g_pending + 1, (size_t)--g_pending_n * sizeof g_pending[0]);
        if (!psp_lookup(in.func)) {
            fprintf(stderr, "psprecomp: GE callback 0x%08X is not a recompiled function\n", in.func);
            continue;
        }
        /* handler(id, arg, list pc), with the interrupted registers kept, as
         * intrman.c does for sub-interrupts. */
        psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = in.a0;
        psp_cpu.r[PSP_REG_A1] = in.a1;
        psp_cpu.r[PSP_REG_A2] = in.a2;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_call_from_host(in.func);
        psp_cpu = saved;
        g_ge_callbacks_run++;
    }
    g_ge_delivering = 0;
}

uint64_t psp_ge_callbacks_run(void) { return g_ge_callbacks_run; }

static const ge_cb *queue_cb(const ge_queue *q) {
    return (q->cbid >= 0 && q->cbid < MAX_GE_CB && g_cb[q->cbid].used) ? &g_cb[q->cbid] : NULL;
}

static uint64_t g_unsupported;     /* vertices in a format we do not read */

/* Draw state handed to the backend with every primitive, and the raw words it
 * is decoded from where one value spans two commands. */
static psp_draw_state g_ds;
static uint32_t g_tbp, g_tbw, g_cbp, g_psm;
static uint8_t  g_clut[1024];      /* the GE's CLUT cache, filled by CLOAD */
/* A vertex with no colour takes the material colour: AMC's RGB, AMA's alpha. */
static uint32_t g_material = 0xFFFFFFFFu;

/* The transform pipeline. Matrices are uploaded one float per command, column
 * by column: world, view and the texture-generation matrix are 4x3 (the last
 * column is the translation), projection is 4x4. The viewport maps clip space
 * into the GE's 4096-wide drawing space, and the offset moves the screen's
 * top-left within it. */
static struct {
    float world[12], view[12], proj[16], tgen[12];
    int   wi, vi, pi, ti;
    float bone[8 * 12];                 /* BONE, eight 4x3 matrices */
    int   bi;
    float morph[8];                     /* MW0-7 */
    float xscale, yscale, xpos, ypos, zscale, zpos;
    float offx, offy;                   /* pixels */
    float uscale, vscale, uoff, voff;
    int   uv_mode, uv_proj, ls0, ls1;   /* TMAP, TSHADE */
    float fog1, fog2;                   /* FOG1, FOG2 */
    uint64_t behind;                    /* triangles wholly behind the near plane */
    uint64_t clipped;                   /* triangles cut by it */
} g_xf;

/* Vertex lighting state (LTE and friends). Colours are as the commands carry
 * them, 0xBBGGRR; positions, directions and factors are GE floats. */
static struct {
    int      enable, on[4], type[4], comp[4];
    float    pos[4][3], dir[4][3], att[4][3], spot_exp[4], spot_cut[4];
    uint32_t lac[4], ldc[4], lsc[4];
    uint32_t emissive, diffuse, specular, ambient;   /* ambient: ALC | ALA << 24 */
    float    spec_exp;
    int      mat_update, reverse_normal;
} g_lt;

/* GE floats are the top 24 bits of an IEEE single. */
static float f24(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

/* The exponent of the lighting pow (specular, spot): the GE keeps only the top
 * 4 mantissa bits and saturates at 496 (PPSSPP's PSPLightExponent). */
static float light_exponent(float e) {
    if (isnan(e)) return signbit(e) ? 0.0f : 496.0f;
    if (e >= 512.0f) return 496.0f;
    union { uint32_t u; float f; } c;
    c.f = e;
    c.u &= 0xFFF80000u;
    return c.f;
}

static void xf_reset(void) {
    memset(&g_xf, 0, sizeof g_xf);
    g_xf.world[0] = g_xf.world[4] = g_xf.world[8] = 1;
    g_xf.view[0] = g_xf.view[4] = g_xf.view[8] = 1;
    g_xf.tgen[0] = g_xf.tgen[4] = g_xf.tgen[8] = 1;
    g_xf.proj[0] = g_xf.proj[5] = g_xf.proj[10] = g_xf.proj[15] = 1;
    g_xf.uscale = g_xf.vscale = 1;
    g_xf.morph[0] = 1;
    for (int b = 0; b < 8; b++)
        g_xf.bone[b * 12 + 0] = g_xf.bone[b * 12 + 4] = g_xf.bone[b * 12 + 8] = 1;
    memset(&g_lt, 0, sizeof g_lt);
}

/* p times a 4x3 matrix: a point with w = 1, or a direction with w = 0. */
static void mul43(const float *M, const float p[3], float w, float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = M[r] * p[0] + M[3 + r] * p[1] + M[6 + r] * p[2] + M[9 + r] * w;
}

/* World space to clip space; `view` gets the view-space position (for fog). */
static void world_to_clip(const float w[3], float view[3], float c[4]) {
    const float *P = g_xf.proj;
    mul43(g_xf.view, w, 1.0f, view);
    for (int r = 0; r < 4; r++)
        c[r] = P[r] * view[0] + P[4 + r] * view[1] + P[8 + r] * view[2] + P[12 + r];
}

/* Clip space to screen pixels, depth and 1/w. Only for points in front of the
 * near plane: clipping happens before this. */
static void clip_to_screen(const float c[4], psp_vertex *out) {
    const float iw = 1.0f / c[3];
    float sx = c[0] * iw * g_xf.xscale + g_xf.xpos - g_xf.offx;
    float sy = c[1] * iw * g_xf.yscale + g_xf.ypos - g_xf.offy;
    /* Keep wildly off-screen vertices inside int range; the rasterizer clips
     * to the screen anyway. */
    if (sx < -100000.0f) sx = -100000.0f; else if (sx > 100000.0f) sx = 100000.0f;
    if (sy < -100000.0f) sy = -100000.0f; else if (sy > 100000.0f) sy = 100000.0f;
    out->x = (int)floorf(sx);
    out->y = (int)floorf(sy);
    out->z = c[2] * iw * g_xf.zscale + g_xf.zpos;
    out->w = iw;
}

static void normalize3(float v[3], float fallback_z) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    else { v[0] = v[1] = 0.0f; v[2] = fallback_z; }
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float light_pow(float x, float e) { return e <= 0.0f ? 1.0f : powf(x > 0.0f ? x : 0.0f, e); }

/* One channel of a 0xBBGGRR / 0xAABBGGRR colour, 0-1. */
static float ch(uint32_t c, int i) { return (float)((c >> (i * 8)) & 0xFF) / 255.0f; }

/* The direction from a vertex to light l, normalised, and the light's
 * attenuation and spot factor. */
static float light_vector(int l, const float wpos[3], float L[3]) {
    if (g_lt.type[l] == 0) {                        /* directional */
        L[0] = g_lt.pos[l][0]; L[1] = g_lt.pos[l][1]; L[2] = g_lt.pos[l][2];
        normalize3(L, 0.0f);
        return 1.0f;
    }
    for (int i = 0; i < 3; i++) L[i] = g_lt.pos[l][i] - wpos[i];
    const float d2 = dot3(L, L), d = sqrtf(d2);
    if (d > 0.0f) { L[0] /= d; L[1] /= d; L[2] /= d; }
    else { L[0] = L[1] = 0.0f; L[2] = 1.0f; }
    const float den = g_lt.att[l][0] + g_lt.att[l][1] * d + g_lt.att[l][2] * d2;
    float scale = den > 0.0f ? 1.0f / den : 0.0f;
    if (!(scale > 0.0f)) scale = 0.0f; else if (scale > 1.0f) scale = 1.0f;
    if (g_lt.type[l] >= 2) {                        /* spot */
        float D[3] = { g_lt.dir[l][0], g_lt.dir[l][1], g_lt.dir[l][2] };
        normalize3(D, 0.0f);
        const float angle = dot3(D, L);
        scale = angle >= g_lt.spot_cut[l] ? scale * light_pow(angle, g_lt.spot_exp[l]) : 0.0f;
    }
    return scale;
}

/* The viewer sits at infinity along view-space +z; in world space that is the
 * view matrix's third row. */
static void view_dir(float out[3]) {
    out[0] = g_xf.view[2]; out[1] = g_xf.view[5]; out[2] = g_xf.view[8];
    normalize3(out, 1.0f);
}

/* Vertex lighting, the model PPSSPP's vertex shaders use: emissive, plus the
 * ambient light times the material ambient, plus per light the ambient,
 * diffuse (N.L) and specular (Blinn half vector) terms scaled by attenuation
 * and spot. The material colours come from the vertex where MATERIALUPDATE
 * says so. Specular is added straight into the colour; the GE can instead keep
 * it as a secondary colour added after texturing (LIGHTMODE), which this does
 * not model. */
static uint32_t light_vertex(const float wpos[3], const float wn[3], uint32_t vcol, int has_color) {
    const int upd = has_color ? g_lt.mat_update : 0;
    const uint32_t amb_c = (upd & 1) ? vcol : g_material;
    const uint32_t dif_c = (upd & 2) ? vcol : g_lt.diffuse;
    const uint32_t spc_c = (upd & 4) ? vcol : g_lt.specular;
    float s0[4], s1[3] = { 0, 0, 0 };
    for (int i = 0; i < 4; i++)
        s0[i] = ch(g_lt.ambient, i) * ch(amb_c, i) + (i < 3 ? ch(g_lt.emissive, i) : 0.0f);
    float V[3];
    view_dir(V);
    for (int l = 0; l < 4; l++) {
        if (!g_lt.on[l]) continue;
        float L[3];
        const float scale = light_vector(l, wpos, L);
        float ldot = dot3(L, wn);
        if (g_lt.comp[l] == 2) ldot = light_pow(ldot, g_lt.spec_exp);
        const float diffuse = ldot > 0.0f ? ldot : 0.0f;
        if (g_lt.comp[l] == 1 && ldot >= 0.0f) {
            float H[3] = { L[0] + V[0], L[1] + V[1], L[2] + V[2] };
            normalize3(H, 1.0f);
            const float sp = light_pow(dot3(H, wn), g_lt.spec_exp);
            if (sp > 0.0f)
                for (int i = 0; i < 3; i++) s1[i] += ch(g_lt.lsc[l], i) * ch(spc_c, i) * sp * scale;
        }
        for (int i = 0; i < 3; i++)
            s0[i] += (ch(g_lt.lac[l], i) * ch(amb_c, i) + ch(g_lt.ldc[l], i) * ch(dif_c, i) * diffuse) * scale;
    }
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        float x = s0[i] + (i < 3 ? s1[i] : 0.0f);
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        out |= (uint32_t)(x * 255.0f + 0.5f) << (i * 8);
    }
    return out;
}

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr, iaddr;
    uint32_t zbp, zbw;
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
    g_base = 0;
    memset(g_cb, 0, sizeof g_cb);
    g_pending_n = 0;
    g_ge_delivering = 0;
    g_ge_callbacks_run = 0;
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
    g_ds.zwrite = 1;
    xf_reset();
    psp_render_reset_pixels();
    g_unsupported = 0;
    g_next_id = 0x00080000u;
}

static void ge_watch_init(void);
void psp_ge_init(void) { psp_ge_reset(); ge_watch_init(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    fprintf(out, "    block transfers %llu, %llu bytes\n",
            (unsigned long long)g_trx_count, (unsigned long long)g_trx_bytes);
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
    uint64_t zt, zf, zc;
    psp_render_depth_stats(&zt, &zf, &zc);
    fprintf(out, "    depth: ZBP 0x%06X ZBW 0x%06X, test %s func %d, %llu tested, %llu rejected, %llu cleared\n",
            g_ge.zbp, g_ge.zbw, g_ds.ztest ? "on" : "off", g_ds.ztest_func,
            (unsigned long long)zt, (unsigned long long)zf, (unsigned long long)zc);
    if (g_xf.behind || g_xf.clipped)
        fprintf(out, "    near plane: %llu triangles clipped, %llu wholly behind it (dropped)\n",
                (unsigned long long)g_xf.clipped, (unsigned long long)g_xf.behind);
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
 *    Skinning (BONE), morphing (MW), index buffers and depth are done;
 *    lighting and the texture matrix are not.
 *  - Position as 16-bit or float; colour as 8888, the 16-bit formats, or
 *    none; texture coordinates as 16-bit or float texels.
 *  - Textures, CLUTs, the texture function, alpha test, blending, clear mode
 *    scissor and depth are decoded into psp_draw_state for the backend.
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

/* Where each component sits in one vertex, and the vertex's size. Components
 * appear in a fixed order (weights, texture, colour, normal, position), each
 * aligned to its own element size, and the whole vertex to the largest. With
 * morphing, a vertex is `morphs` such records back to back. */
typedef struct {
    int w_off, w_n, w_sz;           /* skinning weights: count, element size */
    int tex_off, col_off, norm_off, pos_off;  /* -1 when absent (pos never is) */
    int one;                        /* size of one morph record */
    int stride;                     /* size of the whole vertex */
    int morphs;
} vlayout;

static int vertex_layout(uint32_t vtype, vlayout *L) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };
    static const int w_sz[4]     = { 0, 1, 2, 4 };

    int off = 0, align = 1;

    L->w_sz = w_sz[VT_WEIGHT(vtype)];
    L->w_n  = L->w_sz ? (int)((vtype >> 14) & 7) + 1 : 0;
    L->w_off = 0;
    if (L->w_sz) { off = L->w_n * L->w_sz; align = L->w_sz; }

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); L->tex_off = off; off += ts * 2; if (ts > align) align = ts; }
    else L->tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); L->col_off = off; off += cs; if (cs > align) align = cs; }
    else L->col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); L->norm_off = off; off += ns * 3; if (ns > align) align = ns; }
    else L->norm_off = -1;
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); L->pos_off = off; off += ps * 3;
    if (ps > align) align = ps;

    L->one    = (off + align - 1) & ~(align - 1);
    L->morphs = (int)((vtype >> 18) & 7) + 1;
    L->stride = L->one * L->morphs;
    return L->stride;
}

/* Position, normal and texture coordinate of one morph record. */
static void read_record(uint32_t addr, uint32_t vtype, const vlayout *L, int through,
                        float pos[3], float nrm[3], float uv[2]) {
    uv[0] = uv[1] = 0;
    if (L->tex_off >= 0) {
        uint32_t t = addr + (uint32_t)L->tex_off;
        switch (VT_TEX(vtype)) {
        case 1: uv[0] = psp_read8(t);  uv[1] = psp_read8(t + 1);
                if (!through) { uv[0] /= 128.0f; uv[1] /= 128.0f; }
                break;
        case 2: uv[0] = psp_read16(t); uv[1] = psp_read16(t + 2);
                if (!through) { uv[0] /= 32768.0f; uv[1] /= 32768.0f; }
                break;
        default: uv[0] = psp_read_f32(t); uv[1] = psp_read_f32(t + 4); break;
        }
    }

    nrm[0] = nrm[1] = 0; nrm[2] = 1;
    if (L->norm_off >= 0) {
        const uint32_t n = addr + (uint32_t)L->norm_off;
        switch (VT_NORMAL(vtype)) {
        case 1:  for (int i = 0; i < 3; i++) nrm[i] = (float)(int8_t)psp_read8(n + (uint32_t)i) / 128.0f; break;
        case 2:  for (int i = 0; i < 3; i++) nrm[i] = (float)(int16_t)psp_read16(n + (uint32_t)i * 2) / 32768.0f; break;
        default: for (int i = 0; i < 3; i++) nrm[i] = psp_read_f32(n + (uint32_t)i * 4); break;
        }
    }

    const uint32_t a = addr + (uint32_t)L->pos_off;
    switch (VT_POS(vtype)) {
    case 1:   /* 8-bit */
        for (int i = 0; i < 3; i++) pos[i] = (float)(int8_t)psp_read8(a + (uint32_t)i);
        if (!through) for (int i = 0; i < 3; i++) pos[i] /= 128.0f;
        break;
    case 2:   /* 16-bit; through-mode depth is unsigned */
        for (int i = 0; i < 3; i++) pos[i] = (float)(int16_t)psp_read16(a + (uint32_t)i * 2);
        if (through) pos[2] = (float)psp_read16(a + 4);
        else for (int i = 0; i < 3; i++) pos[i] /= 32768.0f;
        break;
    default:  /* float */
        for (int i = 0; i < 3; i++) pos[i] = psp_read_f32(a + (uint32_t)i * 4);
        break;
    }
}

/* Skinning: position and normal become the weighted sums of the bone matrices
 * applied to them. Bones are 4x3, column by column like the world matrix. */
static void skin(uint32_t addr, uint32_t vtype, const vlayout *L, float pos[3], float nrm[3]) {
    float op[3] = { 0, 0, 0 }, on[3] = { 0, 0, 0 };
    for (int b = 0; b < L->w_n; b++) {
        const uint32_t wa = addr + (uint32_t)(L->w_off + b * L->w_sz);
        float w;
        switch (VT_WEIGHT(vtype)) {
        case 1:  w = (float)psp_read8(wa) / 128.0f; break;
        case 2:  w = (float)psp_read16(wa) / 32768.0f; break;
        default: w = psp_read_f32(wa); break;
        }
        if (w == 0.0f) continue;
        float tp[3], tn[3];
        mul43(g_xf.bone + b * 12, pos, 1.0f, tp);
        mul43(g_xf.bone + b * 12, nrm, 0.0f, tn);
        for (int r = 0; r < 3; r++) { op[r] += w * tp[r]; on[r] += w * tn[r]; }
    }
    for (int r = 0; r < 3; r++) { pos[r] = op[r]; nrm[r] = on[r]; }
}

/* A vertex as the clipper sees it: the backend's vertex, plus its clip-space
 * position. Through-mode vertices are already on screen and never clipped. */
typedef struct {
    psp_vertex v;
    float c[4];
} cvertex;

static int in_front(const cvertex *p) { return p->c[2] + p->c[3] >= 0.0f && p->c[3] > 0.0f; }

static void read_vertex(uint32_t addr, uint32_t vtype, const vlayout *L, cvertex *out) {
    /* Through mode reads 8/16-bit values as integers; transformed mode reads
     * them as fixed point, signed for positions. */
    const int through = (int)VT_THROUGH(vtype);
    uint32_t col = g_material;
    if (L->col_off >= 0) {
        if (VT_COLOR(vtype) == 7)
            col = psp_read32(addr + (uint32_t)L->col_off);
        else    /* 4, 5, 6: 565, 5551, 4444 -- the texture formats, numbered from 4 */
            col = psp_color_expand(psp_read16(addr + (uint32_t)L->col_off),
                                   (int)VT_COLOR(vtype) - 4);
    }

    float pos[3], nrm[3], uv[2];
    read_record(addr, vtype, L, through, pos, nrm, uv);
    if (L->morphs > 1) {
        /* Morph targets blend position, normal and texture coordinate by the
         * MW weights; colour comes from the first record. */
        float p[3], n[3], t[2];
        for (int i = 0; i < 3; i++) { pos[i] *= g_xf.morph[0]; nrm[i] *= g_xf.morph[0]; }
        uv[0] *= g_xf.morph[0]; uv[1] *= g_xf.morph[0];
        for (int m = 1; m < L->morphs; m++) {
            read_record(addr + (uint32_t)(m * L->one), vtype, L, through, p, n, t);
            for (int i = 0; i < 3; i++) { pos[i] += p[i] * g_xf.morph[m]; nrm[i] += n[i] * g_xf.morph[m]; }
            uv[0] += t[0] * g_xf.morph[m]; uv[1] += t[1] * g_xf.morph[m];
        }
    }

    out->v.rgba = col;
    out->v.fog = 1.0f;
    if (through) {
        out->v.u = uv[0]; out->v.v = uv[1];
        out->v.x = (int)pos[0];
        out->v.y = (int)pos[1];
        out->v.z = pos[2];
        out->v.w = 0;
        out->c[0] = out->c[1] = out->c[2] = 0; out->c[3] = 1;
        return;
    }

    if (L->w_n) skin(addr, vtype, L, pos, nrm);
    if (g_lt.reverse_normal) { nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2]; }

    float wpos[3], vpos[3], wn[3];
    mul43(g_xf.world, pos, 1.0f, wpos);
    mul43(g_xf.world, nrm, 0.0f, wn);
    normalize3(wn, 1.0f);
    world_to_clip(wpos, vpos, out->c);

    if (g_lt.enable) out->v.rgba = light_vertex(wpos, wn, col, L->col_off >= 0);

    /* Texture coordinates: scaled and offset (mode 0), through the texture
     * matrix (1), or from the normal's angle to two lights (2, "shade
     * mapping", for environment maps). Then into texels. */
    float s = uv[0], t = uv[1];
    if (g_xf.uv_mode == 1) {
        float src[3], tc[3];
        switch (g_xf.uv_proj) {
        case 0:  src[0] = pos[0]; src[1] = pos[1]; src[2] = pos[2]; break;
        case 1:  src[0] = uv[0]; src[1] = uv[1]; src[2] = 0; break;
        default: src[0] = nrm[0]; src[1] = nrm[1]; src[2] = nrm[2];
                 if (g_xf.uv_proj == 2) normalize3(src, 1.0f);
                 break;
        }
        mul43(g_xf.tgen, src, 1.0f, tc);
        const float q = tc[2] != 0.0f ? tc[2] : 1.0f;
        s = tc[0] / q * g_xf.uscale;
        t = tc[1] / q * g_xf.vscale;
    } else if (g_xf.uv_mode == 2) {
        float L0[3], L1[3];
        light_vector(g_xf.ls0, wpos, L0);
        light_vector(g_xf.ls1, wpos, L1);
        s = g_xf.uscale * (1.0f + dot3(L0, wn)) * 0.5f;
        t = g_xf.vscale * (1.0f + dot3(L1, wn)) * 0.5f;
    } else {
        s = s * g_xf.uscale + g_xf.uoff;
        t = t * g_xf.vscale + g_xf.voff;
    }
    out->v.u = s * (float)g_ds.tex_w;
    out->v.v = t * (float)g_ds.tex_h;

    if (g_ds.fog) {
        float f = (vpos[2] + g_xf.fog1) * g_xf.fog2;
        out->v.fog = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }
    if (in_front(out)) clip_to_screen(out->c, &out->v);
}

/* The point where the near plane cuts a-b, with every attribute interpolated
 * in clip space (where they are linear). */
static cvertex near_cut(const cvertex *a, const cvertex *b) {
    const float da = a->c[2] + a->c[3], db = b->c[2] + b->c[3];
    const float t = da / (da - db);
    cvertex o;
    for (int i = 0; i < 4; i++) o.c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
    uint32_t rgba = 0;
    for (int i = 0; i < 4; i++) {
        const float ca = (float)((a->v.rgba >> (i * 8)) & 0xFF), cb = (float)((b->v.rgba >> (i * 8)) & 0xFF);
        rgba |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << (i * 8);
    }
    o.v.rgba = rgba;
    o.v.u = a->v.u + (b->v.u - a->v.u) * t;
    o.v.v = a->v.v + (b->v.v - a->v.v) * t;
    o.v.fog = a->v.fog + (b->v.fog - a->v.fog) * t;
    /* Just inside, so the projection never divides by a w of exactly 0. */
    if (o.c[3] <= 0.0f) o.c[3] = 1e-6f;
    clip_to_screen(o.c, &o.v);
    return o;
}

/* One triangle against the near plane (z = -w): kept, dropped, or cut into
 * one or two. Appends to `tri`; returns the vertices added. Flat shading uses
 * the last vertex's colour, so the provoking vertex is kept last. */
static uint32_t clip_triangle(const cvertex *a, const cvertex *b, const cvertex *c, psp_vertex *tri) {
    const cvertex *in[3] = { a, b, c };
    const int fa = in_front(a), fb = in_front(b), fc = in_front(c);
    if (fa && fb && fc) { tri[0] = a->v; tri[1] = b->v; tri[2] = c->v; return 3; }
    if (!fa && !fb && !fc) { g_xf.behind++; return 0; }
    g_xf.clipped++;
    cvertex poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const cvertex *p = in[i], *q = in[(i + 1) % 3];
        const int fp = in_front(p), fq = in_front(q);
        if (fp) poly[n++] = *p;
        if (fp != fq) poly[n++] = near_cut(p, q);
    }
    const uint32_t provoking = c->v.rgba;
    uint32_t m = 0;
    for (int i = 1; i + 1 < n; i++) {
        tri[m] = poly[0].v; tri[m + 1] = poly[i].v; tri[m + 2] = poly[i + 1].v;
        m += 3;
    }
    if (!g_ds.gouraud)
        for (uint32_t i = 0; i < m; i++) tri[i].rgba = provoking;
    return m;
}

/* The index of vertex `i` of a primitive: from the index buffer when VTYPE
 * names one, else i itself. */
static uint32_t vertex_index(uint32_t vtype, uint32_t i) {
    switch (VT_INDEX(vtype)) {
    case 1:  return psp_read8(g_ge.iaddr + i);
    case 2:  return psp_read16(g_ge.iaddr + i * 2);
    case 3:  return psp_read32(g_ge.iaddr + i * 4);
    default: return i;
    }
}

/* PSPRECOMP_GE_DUMP=N[,K]: describe every primitive of lists N..N+K-1 (K
 * defaults to 4), with the matrices whenever they change. "The scene comes out
 * as a few huge triangles" is a question about specific vertices, and this is
 * how to see them. */
static uint32_t g_cmd_at[256];     /* where each command was last read from */
static uint64_t g_dump_from, g_dump_to, g_dump_list;
static int dumping(void) {
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("PSPRECOMP_GE_DUMP");
        if (e && *e) {
            unsigned long long a = 0, k = 4;
            sscanf(e, "%llu,%llu", &a, &k);
            g_dump_from = a; g_dump_to = a + k;
            g_dump = fopen("ge_dump.txt", "w");
        }
    }
    return g_dump && g_ge.lists >= g_dump_from && g_ge.lists < g_dump_to;
}

/* PSPRECOMP_GE_WATCH=addr: when a list reaches that word, say which list and
 * how it got there. For state that arrives from a place no list should be. */
static uint32_t g_ge_watch;
static void ge_watch_init(void) {
    const char *e = getenv("PSPRECOMP_GE_WATCH");
    if (e && *e) g_ge_watch = (uint32_t)strtoul(e, NULL, 16);
}
static void ge_watch_hit(const ge_queue *q, uint32_t word) {
    static int told;
    if (told++ >= 4) return;
    fprintf(stderr, "ge watch: list %u (started 0x%08X, list #%llu) at 0x%08X, word 0x%08X; last transfers:\n",
            q->id, q->start, (unsigned long long)g_ge.lists, g_ge_watch, word);
    for (int k = g_nflow > 16 ? g_nflow - 16 : 0; k < g_nflow; k++)
        fprintf(stderr, "    %c 0x%08X -> 0x%08X\n", g_flow[k % 16].kind,
                g_flow[k % 16].from, g_flow[k % 16].to);
    fprintf(stderr, "  words from 0x%08X:", g_ge_watch);
    for (uint32_t i = 0; i < 16; i++) fprintf(stderr, " %08X", psp_read32(g_ge_watch + i * 4));
    fprintf(stderr, "\n");
}

void psp_ge_dump_next(int lists) {
    dumping();
    if (!g_dump) g_dump = fopen("ge_dump.txt", "w");
    g_dump_from = g_ge.lists + 1;
    g_dump_to = g_dump_from + (uint64_t)(lists > 0 ? lists : 1);
}

static void dump_prim(uint32_t type, uint32_t count, const cvertex *v, uint32_t n) {
    if (g_dump_list != g_ge.lists) {
        g_dump_list = g_ge.lists;
        fprintf(g_dump, "=== list %llu\n", (unsigned long long)g_ge.lists);
    }
    static float last[40];
    float now[40];
    memcpy(now, g_xf.world, 12 * 4); memcpy(now + 12, g_xf.view, 12 * 4); memcpy(now + 24, g_xf.proj, 16 * 4);
    if (memcmp(now, last, sizeof now)) {
        memcpy(last, now, sizeof now);
        fprintf(g_dump, "  world"); for (int i = 0; i < 12; i++) fprintf(g_dump, " %g", g_xf.world[i]);
        fprintf(g_dump, "\n  view "); for (int i = 0; i < 12; i++) fprintf(g_dump, " %g", g_xf.view[i]);
        fprintf(g_dump, "\n  proj "); for (int i = 0; i < 16; i++) fprintf(g_dump, " %g", g_xf.proj[i]);
        fprintf(g_dump, "\n  vp scale %g %g %g pos %g %g %g off %g %g uv %g %g %g %g\n",
                g_xf.xscale, g_xf.yscale, g_xf.zscale, g_xf.xpos, g_xf.ypos, g_xf.zpos,
                g_xf.offx, g_xf.offy, g_xf.uscale, g_xf.vscale, g_xf.uoff, g_xf.voff);
        fprintf(g_dump, "  from: world 0x%08X view 0x%08X proj 0x%08X yscale 0x%08X offy 0x%08X\n",
                g_cmd_at[GE_WORLD], g_cmd_at[GE_VIEW], g_cmd_at[GE_PROJ], g_cmd_at[GE_YSCALE],
                g_cmd_at[GE_OFFSETY]);
    }
    fprintf(g_dump, "  %s x%u vtype %06X vaddr %08X fb %08X/%u%s%s%s%s z%d/%d/%d tex %d %08X fmt %d %dx%d fn %d uvmode %d\n",
            PRIM_NAME[type & 7], count, g_ge.vtype, g_ge.vaddr, g_ge.fbp, g_ge.fbw,
            g_ds.clear ? " CLEAR" : "", g_ds.blend ? " blend" : "", VT_THROUGH(g_ge.vtype) ? " through" : "",
            g_lt.enable ? " lit" : "", g_ds.ztest, g_ds.ztest_func, g_ds.zwrite,
            g_ds.tex_enable, g_ds.tex_addr, g_ds.tex_fmt, g_ds.tex_w, g_ds.tex_h, g_ds.tex_func, g_xf.uv_mode);
    for (uint32_t i = 0; i < n && i < 4; i++)
        fprintf(g_dump, "    v%u clip (%g %g %g %g) -> %d %d z %g w %g uv %g %g c %08X fog %g\n", i,
                v[i].c[0], v[i].c[1], v[i].c[2], v[i].c[3], v[i].v.x, v[i].v.y, v[i].v.z, v[i].v.w,
                v[i].v.u, v[i].v.v, v[i].v.rgba, v[i].v.fog);
    fflush(g_dump);
}

static void draw_prim(uint32_t type, uint32_t count) {
    if (!g_ge.vaddr) { g_unsupported += count; return; }

    vlayout L;
    int stride = vertex_layout(g_ge.vtype, &L);
    if (!stride) { g_unsupported += count; return; }
    const uint32_t vtype = g_ge.vtype;

    /* Decode the whole primitive, then hand it to the backend in one call.
     * Strips and fans are order-dependent, so they are never split.
     *
     * Format decoding, transform, lighting and clipping stay here rather than
     * in each backend: the stride arithmetic and component alignment are
     * fiddly, and duplicating them per backend means every backend is wrong in
     * its own way. Wrong once, centrally, is at least diagnosable. */
    static cvertex v[65536];
    static psp_vertex flat[65536], tri[65536 * 6];
    const psp_render_backend *be = psp_render_current();

    int behind = 0;
    for (uint32_t n = 0; n < count; n++) {
        read_vertex(g_ge.vaddr + vertex_index(vtype, n) * (uint32_t)stride, vtype, &L, &v[n]);
        if (!in_front(&v[n])) behind = 1;
    }
    if (dumping()) dump_prim(type, count, v, count);

    if (count && !behind) {
        for (uint32_t n = 0; n < count; n++) flat[n] = v[n].v;
        be->draw((int)type, flat, (int)count, &g_ds);
    } else if (count && type >= 3 && type <= 5) {
        /* Some vertex is behind the near plane: clip triangle by triangle.
         * Strips alternate winding; keeping each triangle's own vertex order
         * keeps the provoking (last) vertex where flat shading expects it. */
        uint32_t m = 0;
        for (uint32_t i = 0; i + 2 < count; i += (type == 3 ? 3 : 1)) {
            const cvertex *a = type == 5 ? &v[0] : &v[i];
            const cvertex *b = &v[i + 1], *c = &v[i + 2];
            m += clip_triangle(a, b, c, tri + m);
        }
        if (m) be->draw(PSP_PRIM_TRIANGLES, tri, (int)m, &g_ds);
    } else if (behind) {
        g_xf.behind++;             /* a sprite, line or point behind the eye */
    }

    /* The GE leaves its pointers just past what it consumed, and games issue
     * consecutive PRIMs without re-sending VADDR/IADDR. */
    static const uint32_t isz[4] = { 0, 1, 2, 4 };
    if (VT_INDEX(vtype)) g_ge.iaddr += count * isz[VT_INDEX(vtype)];
    else                 g_ge.vaddr += count * (uint32_t)stride;
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
static uint32_t rel_addr(const ge_queue *q, uint32_t arg) {
    return ((q->base | (arg & 0xFFFFFF)) + q->offset) & 0x0FFFFFFFu;
}

static void run_list(ge_queue *q) {
    uint32_t (*stack)[3] = q->stack;
    int sp = q->sp;
    uint64_t budget = 1u << 22;

    g_ge.lists++;
    if (dumping()) fprintf(g_dump, "=== run list %u at 0x%08X (lists %llu)\n", q->id, q->list,
                           (unsigned long long)g_ge.lists);

    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */

        uint32_t word = psp_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        q->list += 4;
        g_ge.commands++;
        g_cmd_at[cmd] = q->list - 4;
        if (g_ge_watch && q->list - 4 == g_ge_watch) ge_watch_hit(q, word);
        const uint32_t prev = q->prev;
        q->prev = word;

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
        case GE_CALL: {
            uint32_t to = rel_addr(q, arg & 0xFFFFFC);
            if (!psp_mem_ptr(to, 4)) {
                /* A jump into nothing: say where it came from, then stop this
                 * list instead of walking unmapped memory. */
                static int told;
                if (told++ < 8) {
                    fprintf(stderr, "ge: %s at 0x%08X to unmapped 0x%08X (word 0x%08X, base 0x%08X, offset 0x%08X)\n",
                            cmd == GE_JUMP ? "JUMP" : "CALL", q->list - 4, to, word, q->base, q->offset);
                    fprintf(stderr, "ge: list %u started at 0x%08X; last transfers"
                                    " (C call, J jump, R ret, S signal, T stall resume):\n", q->id, q->start);
                    for (int k = g_nflow > 16 ? g_nflow - 16 : 0; k < g_nflow; k++)
                        fprintf(stderr, "    %c 0x%08X -> 0x%08X\n", g_flow[k % 16].kind,
                                g_flow[k % 16].from, g_flow[k % 16].to);
                }
                q->done = 1;
                q->used = 0;
                return;
            }
            flow(q->list - 4, to, cmd == GE_CALL ? 'C' : 'J');
            if (cmd == GE_CALL && sp < GE_STACK) {
                stack[sp][0] = q->list; stack[sp][1] = q->offset; stack[sp][2] = q->base; sp++;
            }
            q->list = to;
            break;
        }
        case GE_RET:
            flow(q->list - 4, sp > 0 ? stack[sp - 1][0] : 0, 'R');
            if (sp > 0) { sp--; q->list = stack[sp][0]; q->offset = stack[sp][1]; q->base = stack[sp][2]; }
            break;
        case GE_BJUMP:
            /* Conditional on the bounding-box test, which needs geometry we do
             * not process. Not taking it means we walk the enclosed commands
             * rather than skipping them -- the conservative direction, since
             * skipping would under-report what the game drew. */
            break;

        case GE_END:
            /* END after SIGNAL is part of the signal, not the end of the list
             * (PPSSPP's Execute_End): the handler kinds carry on, and 0x10-0x16
             * jump, call or return, to (signal << 16) | END's low 16 bits. */
            if ((prev >> 24) == GE_SIGNAL) {
                const uint32_t behaviour = (prev >> 16) & 0xFF;
                uint32_t target = (((prev & 0xFFFF) << 16) | (arg & 0xFFFF)) & 0xFFFFFFFCu;
                if (behaviour == 0x13 || behaviour == 0x14) target += q->list - 8;          /* relative */
                else if (behaviour == 0x15 || behaviour == 0x16) target = rel_addr(q, target); /* origin */
                if (behaviour == 0x12) {                                                   /* RET */
                    if (sp > 0) { sp--; q->list = stack[sp][0]; q->offset = stack[sp][1]; q->base = stack[sp][2]; }
                } else if (behaviour >= 0x10 && behaviour <= 0x16) {
                    if (!psp_mem_ptr(target, 4)) break;
                    if (behaviour == 0x11 || behaviour == 0x14 || behaviour == 0x16) {    /* CALL */
                        if (sp >= GE_STACK) break;
                        stack[sp][0] = q->list; stack[sp][1] = q->offset; stack[sp][2] = q->base; sp++;
                    }
                    flow(q->list - 8, target, 'S');
                    q->list = target;
                }
                break;
            }
            /* fall through: a plain END ends the list */
        case GE_FINISH:
            if (cmd == GE_FINISH) {
                const ge_cb *cb = queue_cb(q);
                g_ge.finishes++;
                if (cb) raise_intr(cb->finish_func, arg & 0xFFFF, cb->finish_arg, q->list);
            }
            /* A finished list gives its slot back. Keeping it used meant the
             * ninth enqueue failed, and WTF then dropped 3,870 of 3,885
             * frames while running at full speed. */
            q->done = 1;
            q->used = 0;
            return;

        case GE_SIGNAL: {
            /* Behaviour in bits 16-23. SUSPEND (1) and CONTINUE (2) call the
             * signal handler; with the list already run to completion the two
             * look the same. The 0x10-0x13 behaviours (sync, jump, call, ret)
             * never raise one. */
            const ge_cb *cb = queue_cb(q);
            uint32_t behaviour = (arg >> 16) & 0xFF;
            if (cb && (behaviour == 1 || behaviour == 2))
                raise_intr(cb->signal_func, arg & 0xFFFF, cb->signal_arg, q->list);
            break;
        }

        /* Addresses in a list are 24 bits: BASE supplies bits 24-27, and
         * OFFSET_ADDR (or ORIGIN_ADDR, "relative to this command") is added
         * on top. Model data with its own CALLs into itself is written
         * ORIGIN-relative, so it works wherever it is loaded. */
        case GE_TRXSBP:  g_trx.sbp = arg; break;
        case GE_TRXSBW:  g_trx.sbw = arg; break;
        case GE_TRXDBP:  g_trx.dbp = arg; break;
        case GE_TRXDBW:  g_trx.dbw = arg; break;
        case GE_TRXSPOS: g_trx.spos = arg; break;
        case GE_TRXDPOS: g_trx.dpos = arg; break;
        case GE_TRXSIZE: g_trx.size = arg; break;
        case GE_TRXKICK: trx_kick(arg); break;

        case GE_BASE:        q->base = g_base = (arg & 0x0F0000) << 8; break;
        case GE_ORIGIN_ADDR: q->offset = q->list - 4; break;
        case GE_OFFSET_ADDR: q->offset = arg << 8; break;

        case GE_BOFS:  g_xf.bi = (int)(arg & 0x7F); break;
        case GE_BONE:  if (g_xf.bi < 96) g_xf.bone[g_xf.bi++] = f24(arg); break;
        case GE_MW0: case GE_MW0 + 1: case GE_MW0 + 2: case GE_MW0 + 3:
        case GE_MW0 + 4: case GE_MW0 + 5: case GE_MW0 + 6: case GE_MW0 + 7:
            g_xf.morph[cmd - GE_MW0] = f24(arg);
            break;

        case GE_ZTE:    g_ds.ztest = arg & 1; break;
        case GE_ZTST:   g_ds.ztest_func = (int)(arg & 7); break;
        case GE_ZMSK:   g_ds.zwrite = !(arg & 1); break;
        case GE_ZSCALE: g_xf.zscale = f24(arg); break;
        case GE_ZPOS:   g_xf.zpos = f24(arg); break;
        case GE_ZBP:
            g_ge.zbp = arg;
            g_ds.zb_addr = PSP_VRAM_BASE | (g_ge.zbp & (PSP_VRAM_SIZE - 1));
            break;
        case GE_ZBW:
            g_ge.zbw = arg;
            g_ds.zb_stride = (int)(arg & 0xFFFF);
            break;

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
        case GE_LTE:   g_lt.enable = arg & 1; break;
        case GE_LTE0: case GE_LTE0 + 1: case GE_LTE0 + 2: case GE_LTE0 + 3:
            g_lt.on[cmd - GE_LTE0] = arg & 1;
            break;
        case GE_LT0: case GE_LT0 + 1: case GE_LT0 + 2: case GE_LT0 + 3:
            g_lt.comp[cmd - GE_LT0] = (int)(arg & 3);
            g_lt.type[cmd - GE_LT0] = (int)((arg >> 8) & 3);
            break;
        case GE_RNORM: g_lt.reverse_normal = arg & 1; break;
        case GE_CMAT:  g_lt.mat_update = (int)(arg & 7); break;
        case GE_EMC:   g_lt.emissive = arg; break;
        case GE_DMC:   g_lt.diffuse = arg; break;
        case GE_SMC:   g_lt.specular = arg; break;
        case GE_SPOW:  g_lt.spec_exp = light_exponent(f24(arg)); break;
        case GE_ALC:   g_lt.ambient = (g_lt.ambient & 0xFF000000u) | arg; break;
        case GE_ALA:   g_lt.ambient = (g_lt.ambient & 0x00FFFFFFu) | ((arg & 0xFF) << 24); break;
        case GE_TMAP:  g_xf.uv_mode = (int)(arg & 3); g_xf.uv_proj = (int)((arg >> 8) & 3); break;
        case GE_TSHADE: g_xf.ls0 = (int)(arg & 3); g_xf.ls1 = (int)((arg >> 8) & 3); break;
        case GE_TMS:   g_xf.ti = (int)(arg & 0xF); break;
        case GE_TMATRIX: if (g_xf.ti < 12) g_xf.tgen[g_xf.ti++] = f24(arg); break;
        case GE_FGE:   g_ds.fog = arg & 1; break;
        case GE_FFAR:  g_xf.fog1 = f24(arg); break;
        case GE_FDIST: g_xf.fog2 = f24(arg); break;
        case GE_FCOL:  g_ds.fog_color = arg | 0xFF000000u; break;
        case GE_ABE:   g_ds.blend = arg & 1; break;
        case GE_ATE:   g_ds.atest = arg & 1; break;
        case GE_SHADE: g_ds.gouraud = arg & 1; break;
        case GE_AMC:   g_material = (g_material & 0xFF000000u) | arg; break;
        case GE_AMA:   g_material = (g_material & 0x00FFFFFFu) | ((arg & 0xFF) << 24); break;
        case GE_CLEAR:
            g_ds.clear = arg & 1;
            g_ds.clear_color = (arg >> 8) & 1;
            g_ds.clear_alpha = (arg >> 9) & 1;
            g_ds.clear_depth = (arg >> 10) & 1;
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

        case GE_VADDR: g_ge.vaddr = rel_addr(q, arg); break;
        case GE_IADDR: g_ge.iaddr = rel_addr(q, arg); break;

        default:
            if (cmd >= GE_LXP0 && cmd < GE_LXP0 + 12) {
                g_lt.pos[(cmd - GE_LXP0) / 3][(cmd - GE_LXP0) % 3] = f24(arg); break;
            }
            if (cmd >= GE_LXD0 && cmd < GE_LXD0 + 12) {
                g_lt.dir[(cmd - GE_LXD0) / 3][(cmd - GE_LXD0) % 3] = f24(arg); break;
            }
            if (cmd >= GE_LCA0 && cmd < GE_LCA0 + 12) {
                g_lt.att[(cmd - GE_LCA0) / 3][(cmd - GE_LCA0) % 3] = f24(arg); break;
            }
            if (cmd >= GE_SPOTEXP0 && cmd < GE_SPOTEXP0 + 4) {
                g_lt.spot_exp[cmd - GE_SPOTEXP0] = light_exponent(f24(arg)); break;
            }
            if (cmd >= GE_SPOTCUT0 && cmd < GE_SPOTCUT0 + 4) {
                g_lt.spot_cut[cmd - GE_SPOTCUT0] = f24(arg); break;
            }
            if (cmd >= GE_LAC0 && cmd < GE_LAC0 + 12) {
                const int l = (int)(cmd - GE_LAC0) / 3;
                switch ((cmd - GE_LAC0) % 3) {
                case 0:  g_lt.lac[l] = arg; break;
                case 1:  g_lt.ldc[l] = arg; break;
                default: g_lt.lsc[l] = arg; break;
                }
                break;
            }
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
    q->sp = sp;                         /* stalled: the next run resumes here */
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
    q->start = q->list;
    q->stall = psp_arg(1) & ~3u;
    q->used  = 1;
    q->cbid  = (int32_t)psp_arg(2);
    q->base  = g_base;
    (void)head;

    /* Hardware runs the list asynchronously. We run it here and finish before
     * returning, which is indistinguishable from the game's point of view
     * because every way it can observe progress -- ListSync, DrawSync -- then
     * reports completion. */
    run_list(q);
    psp_ret(q->id);
    psp_ge_deliver_pending();
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    uint32_t id = psp_arg(0);
    ge_queue *q = find_queue(id);
    if (!q) {
        /* A list that already finished has released its slot; moving its
         * stall is harmless. An id never issued is an error. */
        psp_ret(id >= 0x00080000u && id < g_next_id ? SCE_KERNEL_ERROR_OK
                                                    : SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    q->stall = psp_arg(1) & ~3u;
    if (!q->done) { flow(q->list, q->list, 'T'); run_list(q); }   /* the new stall released more commands */
    psp_ret(SCE_KERNEL_ERROR_OK);
    psp_ge_deliver_pending();
}

/* Lists are complete by the time they are enqueued, so every sync succeeds
 * immediately. */
static void hle_ListSync(void) { psp_ge_deliver_pending(); psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_DrawSync(void) { psp_ge_deliver_pending(); psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_Break(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Continue(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* (PspGeCallbackData *): { signal_func, signal_arg, finish_func, finish_arg } */
static void hle_SetCallback(void) {
    uint32_t d = psp_arg(0);
    for (int i = 0; i < MAX_GE_CB; i++) {
        if (g_cb[i].used) continue;
        g_cb[i].used = 1;
        g_cb[i].signal_func = psp_read32(d + 0);
        g_cb[i].signal_arg  = psp_read32(d + 4);
        g_cb[i].finish_func = psp_read32(d + 8);
        g_cb[i].finish_arg  = psp_read32(d + 12);
        psp_ret((uint32_t)i);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_UnsetCallback(void) {
    uint32_t id = psp_arg(0);
    if (id >= MAX_GE_CB) { psp_ret(0x80000100); return; }   /* invalid id */
    g_cb[id].used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

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
