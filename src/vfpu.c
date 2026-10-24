/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

static uint32_t g_prefix[3];      /* vpfxs, vpfxt, vpfxd */
static int      g_prefix_set[3];
static uint64_t g_traps;

void psp_vfpu_reset(void) {
    memset(g_prefix, 0, sizeof g_prefix);
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    g_prefix[which] = value;
    g_prefix_set[which] = 1;
}

int psp_vfpu_prefix_pending(void) {
    return g_prefix_set[0] || g_prefix_set[1] || g_prefix_set[2];
}

uint64_t psp_vfpu_trap_count(void) { return g_traps; }

void psp_vfpu_unimplemented(uint32_t addr, const char *what) {
    /* First few only: a VFPU-heavy inner loop would otherwise produce
     * megabytes of identical lines and hide everything else. */
    if (g_traps < 16)
        fprintf(stderr, "psprecomp: VFPU %s at 0x%08X not implemented\n", what, addr);
    else if (g_traps == 16)
        fprintf(stderr, "psprecomp: (further VFPU traps suppressed)\n");
    g_traps++;
}

/* ---- prefixes -------------------------------------------------------------
 *
 * vpfxs/vpfxt rewrite each source lane i: bits 2i..2i+1 pick which lane to
 * read (swizzle), bit 8+i takes the absolute value, bit 12+i replaces the
 * lane with a constant, bit 16+i negates. vpfxd saturates each result lane
 * (bits 2i..2i+1: 1 = [0,1], 3 = [-1,1]) and bit 8+i masks its write. All
 * three apply to the next VFPU op only. Semantics follow PPSSPP's interpreter
 * (Core/MIPS/InterpreterVFPU.cpp), which is checked against hardware tests. */

static const float PFX_CONST[8] = {
    0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f
};

static float f_bits(float f, uint32_t and_mask, uint32_t xor_mask) {
    uint32_t u;
    memcpy(&u, &f, 4);
    u = (u & and_mask) ^ xor_mask;
    memcpy(&f, &u, 4);
    return f;
}

static void apply_st(uint32_t data, float r[4], int n) {
    float o[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) o[i] = r[i];
    for (int i = 0; i < n; i++) {
        const int sw  = (int)(data >> (2 * i)) & 3;
        const int abs = (int)(data >> (8 + i)) & 1;
        const int cst = (int)(data >> (12 + i)) & 1;
        const int neg = (int)(data >> (16 + i)) & 1;
        if (cst) r[i] = PFX_CONST[sw + (abs << 2)];
        else     r[i] = abs ? f_bits(o[sw], 0x7FFFFFFFu, 0) : o[sw];
        if (neg) r[i] = f_bits(r[i], 0xFFFFFFFFu, 0x80000000u);
    }
}

/* Read a vector operand through the S (which=0) or T (which=1) prefix. */
static int vread(int which, uint32_t vreg, int size, float out[4]) {
    int r[4];
    const int n = psp_vfpu_regs(vreg, size, r);
    for (int i = 0; i < 4; i++) out[i] = i < n ? psp_cpu.v[r[i]] : 0.0f;
    if (g_prefix_set[which]) apply_st(g_prefix[which], out, n);
    return n;
}

/* Write a result through the D prefix, then consume all three prefixes.
 * `sat` is 0 for results that are integer bit patterns: only the write mask
 * applies to those. */
static void diag(const char *what, const float *in, const float *out);

static void vwrite_x(uint32_t vreg, int size, const float in[4], int sat) {
    if (psp_vfpu_prefix_pending()) diag("pfx", in, in);
    int r[4];
    const int n = psp_vfpu_regs(vreg, size, r);
    const uint32_t d = g_prefix_set[2] ? g_prefix[2] : 0;
    for (int i = 0; i < n; i++) {
        float x = in[i];
        if (d) {
            if ((d >> (8 + i)) & 1) continue;               /* masked */
            const int s = (int)(d >> (2 * i)) & 3;
            if (sat && s == 1) x = x != x ? x : (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x));
            if (sat && s == 3) x = x != x ? x : (x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x));
        }
        psp_cpu.v[r[i]] = x;
    }
    memset(g_prefix_set, 0, sizeof g_prefix_set);
}

static void vwrite(uint32_t vreg, int size, const float in[4]) { vwrite_x(vreg, size, in, 1); }

/* An op that writes nothing through the D prefix still consumes them all. */
static void vdone(void) { memset(g_prefix_set, 0, sizeof g_prefix_set); }

/* PSPRECOMP_VFPU_COMPAT restores the old behaviour for comparison. "1"
 * means all of it: an op with a prefix pending traps without writing, and the
 * ops added with the prefix work (vdiv, vsge, vcmov, vrot, ...) do nothing.
 * A comma list picks parts: "prefix" for the first, op names for the rest,
 * e.g. PSPRECOMP_VFPU_COMPAT=vrot,vdiv. */
static int g_compat = -1;
static char g_compat_list[256];
static int compat_has(const char *what) {
    if (g_compat < 0) {
        const char *e = getenv("PSPRECOMP_VFPU_COMPAT");
        g_compat = e && *e && *e != '0';
        if (g_compat) {
            snprintf(g_compat_list, sizeof g_compat_list, ",%s,", e);
            fprintf(stderr, "psprecomp: VFPU compat: %s\n", e);
        }
    }
    if (!g_compat) return 0;
    if (!strcmp(g_compat_list, ",1,")) return 1;
    char key[40];
    snprintf(key, sizeof key, ",%s,", what);
    return strstr(g_compat_list, key) != NULL;
}
static int compat_prefix_trap(const char *what) {
    if (!psp_vfpu_prefix_pending() || !compat_has("prefix")) return 0;
    vdone();
    psp_vfpu_unimplemented(psp_cpu.pc, what);
    return 1;
}
static int compat_new_op(const char *what) {
    if (!compat_has(what)) return 0;
    vdone();
    psp_vfpu_unimplemented(psp_cpu.pc, what);
    return 1;
}

/* The first few results of each newly implemented op, for the log, and per
 * op how often it ran and produced a NaN or infinity. */
static struct { const char *n; uint64_t c, bad; } g_seen[32];
static void diag(const char *what, const float *in, const float *out) {
    int i = 0;
    while (i < 32 && g_seen[i].n && strcmp(g_seen[i].n, what)) i++;
    if (i == 32) return;
    if (!g_seen[i].n) g_seen[i].n = what;
    for (int k = 0; k < 4; k++) if (!isfinite(out[k])) { g_seen[i].bad++; break; }
    if (g_seen[i].c++ >= 3) return;
    fprintf(stderr, "vfpu: %-6s in %g %g %g %g -> %g %g %g %g%s\n", what,
            (double)in[0], (double)in[1], (double)in[2], (double)in[3],
            (double)out[0], (double)out[1], (double)out[2], (double)out[3],
            psp_vfpu_prefix_pending() ? " (prefixed)" : "");
}

/* ---- register addressing ------------------------------------------------- */

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    const int mtx       = (vreg >> 2) & 7;
    const int col       = vreg & 3;
    int transpose       = (vreg >> 5) & 1;
    int row = 0, len = 1;

    switch (size) {
    case 1: row = (vreg >> 5) & 3; transpose = 0; len = 1; break;
    case 2: row = (vreg >> 5) & 2;                len = 2; break;
    case 3: row = (vreg >> 6) & 1;                len = 3; break;
    default:row = (vreg >> 5) & 2;                len = 4; break;
    }

    for (int i = 0; i < len; i++) {
        /* Transposed access walks columns instead of rows -- the same storage
         * seen the other way round, which is what makes a matrix transpose
         * free on this hardware. */
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
    return len;
}

/* ---- load / store -------------------------------------------------------- */

void psp_lv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_cpu.v[r[0]] = psp_read_f32(addr & ~3u);
}

void psp_sv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_write_f32(addr & ~3u, psp_cpu.v[r[0]]);
}

void psp_lv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;                       /* quad access is 16-byte aligned */
    for (int i = 0; i < 4; i++)
        psp_cpu.v[r[i]] = psp_read_f32(addr + (uint32_t)i * 4);
}

void psp_sv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;
    for (int i = 0; i < 4; i++)
        psp_write_f32(addr + (uint32_t)i * 4, psp_cpu.v[r[i]]);
}

/* ---- arithmetic ---------------------------------------------------------- */

#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        /* Read every source before writing any destination: vd may alias vs \
         * or vt, and a lane-by-lane read/write would then feed results back  \
         * into later lanes. */                                              \
        float sv[4], tv[4], out[4] = { 0, 0, 0, 0 };                         \
        const int fresh = !strcmp(#name, "vdiv") || !strcmp(#name, "vsge") ||\
                          !strcmp(#name, "vslt");                            \
        if (fresh ? compat_new_op(#name) : compat_prefix_trap(#name)) return;\
        int n = vread(0, vs, size, sv);                                      \
        vread(1, vt, size, tv);                                              \
        for (int i = 0; i < n; i++) {                                        \
            float a = sv[i], b = tv[i];                                      \
            out[i] = (expr);                                                 \
        }                                                                    \
        if (fresh) diag(#name, sv, out);                                     \
        vwrite(vd, size, out);                                               \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)
BINOP(vsge, a >= b ? 1.0f : 0.0f)
BINOP(vslt, a <  b ? 1.0f : 0.0f)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_prefix_trap("vdot")) return;
    float s[4], t[4], out[4] = { 0, 0, 0, 0 };
    int n = vread(0, vs, size, s);
    vread(1, vt, size, t);
    for (int i = 0; i < n; i++) out[0] += s[i] * t[i];
    vwrite(vd, 1, out);
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_prefix_trap("vscl")) return;
    float s[4], t[4], out[4] = { 0, 0, 0, 0 };
    int n = vread(0, vs, size, s);
    vread(1, vt, 1, t);
    for (int i = 0; i < n; i++) out[i] = s[i] * t[0];
    vwrite(vd, size, out);
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */

static float sat0(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sat1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

/* Round half to even, without depending on the host's rounding mode. */
static double round_even(double v) {
    double r = floor(v);
    const double frac = v - r;                   /* exact for |v| < 2^52 */
    if (frac > 0.5 || (frac == 0.5 && fmod(r, 2.0) != 0.0)) r += 1.0;
    return r;
}

/* The conversions, as measured on a PSP-3000 (pspautotests cpu/vfpu/convert
 * and convert_scaled). vf2in/vf2iz/vf2iu/vf2id round x * 2^scale to nearest
 * (ties to even), toward zero, up, and down, then saturate to int32; NaN of
 * either sign gives 0x7FFFFFFF. A plain C cast would be undefined for all
 * three of those edges. */
static uint32_t f2i(float x, int scale, int op) {
    if (x != x) return 0x7FFFFFFFu;
    const double v = ldexp((double)x, scale);    /* exact: float * 2^31 fits a double */
    double r;
    switch (op) {
    case PSP_VU_F2IN: r = round_even(v); break;
    case PSP_VU_F2IU: r = ceil(v);       break;
    case PSP_VU_F2ID: r = floor(v);      break;
    default:          r = trunc(v);      break;   /* PSP_VU_F2IZ */
    }
    if (r >= 2147483648.0)  return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;                 /* an in-range integer: exact */
}

/* vi2f: int * 2^-scale, rounded to float once. */
static float i2f(uint32_t bits, int scale) {
    return (float)ldexp((double)(int32_t)bits, -scale);
}

void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    if (compat_prefix_trap("vunary")) return;
    float s[4], out[4] = { 0, 0, 0, 0 };
    int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) {
        const float a = s[i];
        float r;
        switch (op) {
        case PSP_VU_MOV:  r = a;            break;
        case PSP_VU_ABS:  r = a < 0 ? -a : a; break;
        case PSP_VU_NEG:  r = -a;           break;
        case PSP_VU_ZERO: r = 0.0f;         break;
        case PSP_VU_ONE:  r = 1.0f;         break;
        case PSP_VU_RCP:  r = 1.0f / a;     break;
        case PSP_VU_NRCP: r = -1.0f / a;    break;
        case PSP_VU_RSQ:  r = 1.0f / psp_fsqrt(a); break;
        case PSP_VU_SQRT: r = psp_fsqrt(a); break;
        /* The PSP's trig takes its argument in *quarter turns*: vsin(x) is
         * sin(x * pi/2), not sin(x). Treating it as radians gives a result
         * that is smooth, plausible, and wrong -- rotations end up at the
         * wrong angle rather than visibly broken. */
        case PSP_VU_SIN:  r = sinf(a * 1.5707963267948966f);  break;
        case PSP_VU_COS:  r = cosf(a * 1.5707963267948966f);  break;
        case PSP_VU_NSIN: r = -sinf(a * 1.5707963267948966f); break;
        case PSP_VU_ASIN: r = asinf(a) * 0.6366197723675814f; break;  /* 2/pi */
        case PSP_VU_EXP2: r = powf(2.0f, a);   break;
        case PSP_VU_REXP2:r = 1.0f / powf(2.0f, a); break;
        case PSP_VU_LOG2: r = logf(a) * 1.4426950408889634f; break;   /* 1/ln2 */
        case PSP_VU_SAT0: r = sat0(a);      break;
        case PSP_VU_SAT1: r = sat1(a);      break;
        /* Conversions move between the float and integer *interpretations* of
         * a vector register; the bits are reinterpreted, not just cast. */
        case PSP_VU_F2IZ: r = psp_bits_to_f32(f2i(a, 0, op)); break;
        case PSP_VU_I2F:  r = i2f(psp_f32_to_bits(a), 0);     break;
        default:          vdone(); psp_vfpu_unimplemented(psp_cpu.pc, "vunary"); return;
        }
        out[i] = r;
    }
    vwrite_x(vd, size, out, op != PSP_VU_F2IZ);
}

void psp_vcvt(int op, uint32_t vd, uint32_t vs, int size, int scale) {
    if (compat_prefix_trap("vcvt")) return;
    float s[4], out[4] = { 0, 0, 0, 0 };
    int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) {
        const float a = s[i];
        switch (op) {
        case PSP_VU_F2IN: case PSP_VU_F2IZ: case PSP_VU_F2IU: case PSP_VU_F2ID:
                          out[i] = psp_bits_to_f32(f2i(a, scale, op)); break;
        case PSP_VU_I2F:  out[i] = i2f(psp_f32_to_bits(a), scale);    break;
        default:          vdone(); psp_vfpu_unimplemented(psp_cpu.pc, "vcvt"); return;
        }
    }
    vwrite_x(vd, size, out, op == PSP_VU_I2F);
}

/* ---- matrix ops without a multiply --------------------------------------- */

/* A matrix register names `size` consecutive columns, each a vector of `size`
 * lanes. Writing one column at a time through the same addressing the vector
 * ops use keeps the two consistent -- which matters because a game builds a
 * matrix with these and then transforms with vtfm. */
static void matrix_cols(uint32_t vd, int size, int cols[4][4]) {
    /* Build a fresh vector register per column: matrix, column index, row 0.
     * An earlier version modified bits 6:5 of the incoming register, which
     * varies the *row* field and so walked rows while claiming to walk
     * columns -- every matrix op addressed the wrong elements. */
    const uint32_t mtx       = (vd >> 2) & 7;
    const uint32_t transpose = (vd >> 5) & 1;
    for (int c = 0; c < size; c++) {
        uint32_t vreg = (mtx << 2) | (uint32_t)c | (transpose << 5);
        psp_vfpu_regs(vreg, size, cols[c]);
    }
}

/* Read a whole matrix out into [col][row] order. */
static void matrix_read(uint32_t v, int size, float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] = psp_cpu.v[cols[c][r]];
}

static void matrix_write(uint32_t v, int size, const float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = m[c][r];
}

void psp_vmidt(uint32_t vd, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            psp_cpu.v[cols[c][r]] = (c == r) ? 1.0f : 0.0f;
}

/* vidt -- an identity *vector*: all zeroes but for a single 1.0.
 *
 * Which element gets the 1 is encoded in the destination register number
 * rather than given as an operand, which is how one instruction builds any
 * basis vector. Bits 6-7 of vd select the position.
 *
 * This is matrix-setup code. Trapping it to a no-op leaves whatever was in the
 * register, so downstream geometry is built on a basis that is not a basis. */
void psp_vidt(uint32_t vd, int size) {
    if (compat_prefix_trap("vidt")) return;
    float out[4];
    const int one = (int)((vd >> 6) & 3);
    for (int i = 0; i < 4; i++) out[i] = (i == one) ? 1.0f : 0.0f;
    vwrite(vd, size, out);
}

/* vcst -- load a constant from the VFPU's built-in table.
 *
 * The index is in the vs field. Values follow the hardware table; index 0 is
 * zero and anything past the end reads as zero rather than as garbage. */
void psp_vcst(uint32_t vd, uint32_t which, int size) {

    static const float K[20] = {
        0.0f,
        3.4028235e38f,          /* max float          */
        1.41421356f,            /* sqrt(2)            */
        0.70710678f,            /* sqrt(1/2)          */
        1.12837917f,            /* 2/sqrt(pi)         */
        0.63661977f,            /* 2/pi               */
        0.31830989f,            /* 1/pi               */
        0.78539816f,            /* pi/4               */
        1.57079633f,            /* pi/2               */
        3.14159265f,            /* pi                 */
        2.71828183f,            /* e                  */
        1.44269504f,            /* log2(e)            */
        0.43429448f,            /* log10(e)           */
        0.69314718f,            /* ln(2)              */
        2.30258509f,            /* ln(10)             */
        6.28318531f,            /* 2*pi               */
        0.52359878f,            /* pi/6               */
        0.30103000f,            /* log10(2)           */
        3.32192809f,            /* log2(10)           */
        0.86602540f,            /* sqrt(3)/2          */
    };

    const float k = which < 20 ? K[which] : 0.0f;
    const float out[4] = { k, k, k, k };
    vwrite(vd, size, out);
}

void psp_vmzero(uint32_t vd, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 0.0f;
}

void psp_vmone(uint32_t vd, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 1.0f;
}

void psp_vmmov(uint32_t vd, uint32_t vs, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    int dc[4][4], sc[4][4];
    matrix_cols(vd, size, dc);
    matrix_cols(vs, size, sc);
    float tmp[4][4];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) tmp[c][r] = psp_cpu.v[sc[c][r]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[dc[c][r]] = tmp[c][r];
}

/* ---- matrix multiply and transform --------------------------------------- */

/* Scale every element of a matrix by a scalar. Orientation-independent, so
 * this one is unambiguous. */
void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    float m[4][4];
    int t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, 1, t);
    const float k = psp_cpu.v[t[0]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] *= k;
    matrix_write(vd, size, m);
}

/* Transform a vector by a matrix: vd[r] = sum over c of M[c][r] * v[c].
 *
 * This is the column-major product a graphics pipeline wants -- the matrix
 * columns are the transformed basis vectors, so transforming (1,0,0,0) yields
 * column 0. That property is what the test pins, and it is the one that
 * distinguishes this from its transpose. */
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    float m[4][4];
    int d[4], t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, size, t);
    psp_vfpu_regs(vd, size, d);

    float in[4], out[4];
    for (int i = 0; i < size; i++) in[i] = psp_cpu.v[t[i]];
    for (int r = 0; r < size; r++) {
        float sum = 0.0f;
        for (int c = 0; c < size; c++) sum += m[c][r] * in[c];
        out[r] = sum;
    }
    /* vd may be one of the sources, so write only after the whole result is
     * computed. */
    for (int r = 0; r < size; r++) psp_cpu.v[d[r]] = out[r];
}

/* Matrix product: Md = Ms^T * Mt. The first operand is used transposed, which
 * is why code writes `vmmul.q M000, E100, M200` (E = the transposed view) to
 * get M100 * M200.
 *
 * Verified against a game rather than assumed. WTF's matrix multiply at
 * 0x0002F494 is exactly that idiom, and with it the projection matrix reaches
 * the GE column-major, with its translation in elements 12-14, as the GE
 * expects. Without the transpose it arrived transposed, and the first
 * transformed vertex landed at (-83408, -47266). */
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    vdone();     /* prefixes do not apply to matrix ops */
    float a[4][4], b[4][4], out[4][4];
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    if (compat_prefix_trap("vcmp")) return;
    float sv[4], tv[4];
    int n = vread(0, vs, size, sv);
    vread(1, vt, size, tv);
    vdone();

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        float a = sv[i], b = tv[i];
        int r;
        switch (cond & 0xF) {
        case 0:  r = 0;              break;   /* FL  */
        case 1:  r = (a == b);       break;   /* EQ  */
        case 2:  r = (a <  b);       break;   /* LT  */
        case 3:  r = (a <= b);       break;   /* LE  */
        case 4:  r = 1;              break;   /* TR  */
        case 5:  r = (a != b);       break;   /* NE  */
        case 6:  r = (a >= b);       break;   /* GE  */
        case 7:  r = (a >  b);       break;   /* GT  */
        case 8:  r = (a == 0.0f);    break;   /* EZ  */
        case 9:  r = (a != a);       break;   /* EN: NaN */
        case 10: r = isinf(a);       break;   /* EI  */
        case 11: r = !isfinite(a);   break;   /* ES: NaN or inf */
        case 12: r = (a != 0.0f);    break;   /* NZ  */
        case 13: r = (a == a);       break;   /* NN  */
        case 14: r = !isinf(a);      break;   /* NI  */
        default: r = isfinite(a);    break;   /* NS  */
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;
    /* Only the lanes compared, and the two summary bits, change. */
    const uint32_t affected = ((1u << n) - 1) | 0x30u;
    psp_cpu.vfpu_cc = (psp_cpu.vfpu_cc & ~affected) | (cc & affected);
}

/* viim / vfim -- write a single lane from an immediate encoded in the
 * instruction. No prefixes apply: there is no source operand to rewrite. */
void psp_vimm(uint32_t vd, float value) {
    vdone();
    int d[4];
    psp_vfpu_regs(vd, 1, d);
    psp_cpu.v[d[0]] = value;
}

/* ---- the rest of the vector ops --------------------------------------------
 * Each follows PPSSPP's interpreter, including how it reads its operands. */

/* vcmov: copy vs into vd where the condition holds. imm3 < 6 tests one CC bit
 * for every lane; 6 tests each lane's own bit. The T prefix applies to the
 * old value of vd. */
void psp_vcmov(uint32_t vd, uint32_t vs, int size, uint32_t raw) {
    if (compat_new_op("vcmov")) return;
    const int tf = (int)(raw >> 19) & 1, imm3 = (int)(raw >> 16) & 7;
    float s[4], d[4];
    const int n = vread(0, vs, size, s);
    vread(1, vd, size, d);
    const uint32_t cc = psp_cpu.vfpu_cc;
    if (imm3 == 6) {
        for (int i = 0; i < n; i++)
            if ((int)((cc >> i) & 1) == !tf) d[i] = s[i];
    } else if ((int)((cc >> imm3) & 1) == !tf) {
        for (int i = 0; i < n; i++) d[i] = s[i];
    }
    diag("vcmov", s, d);
    vwrite(vd, size, d);
}

/* vcrsp.t is the cross product; vqmul.q (same encoding, quad) multiplies
 * quaternions. */
void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_new_op("vcrsp")) return;
    float s[4] = { 0 }, t[4] = { 0 }, d[4] = { 0 };
    int r[4];
    const int n = psp_vfpu_regs(vs, size, r);
    for (int i = 0; i < n; i++) s[i] = psp_cpu.v[r[i]];
    psp_vfpu_regs(vt, size, r);
    for (int i = 0; i < n; i++) t[i] = psp_cpu.v[r[i]];
    if (size == 3) {
        d[0] = s[1] * t[2] - s[2] * t[1];
        d[1] = s[2] * t[0] - s[0] * t[2];
        d[2] = s[0] * t[1] - s[1] * t[0];
    } else {
        d[0] =  s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
        d[1] = -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1];
        d[2] =  s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
        d[3] = -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3];
    }
    diag("vcrsp", s, d);
    vwrite(vd, size, d);
}

/* vcrs.t: half a cross product, (s.y*t.z, s.z*t.x, s.x*t.y). */
void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_new_op("vcrs")) return;
    float s[4], t[4], d[4] = { 0 };
    vread(0, vs, size, s);
    vread(1, vt, size, t);
    d[0] = s[1] * t[2];
    d[1] = s[2] * t[0];
    d[2] = s[0] * t[1];
    diag("vcrs", s, d);
    vwrite(vd, size, d);
}

/* vdet.p: the 2x2 determinant s.x*t.y - s.y*t.x, into one lane. */
void psp_vdet(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_new_op("vdet")) return;
    float s[4], t[4], d[4] = { 0 };
    vread(0, vs, size, s);
    vread(1, vt, size, t);
    d[0] = s[0] * t[1] - s[1] * t[0];
    diag("vdet", s, d);
    vwrite(vd, 1, d);
}

/* vhdp: homogeneous dot product -- vs with its last lane taken as 1. */
void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (compat_new_op("vhdp")) return;
    float s[4], t[4], d[4] = { 0 };
    const int n = vread(0, vs, size, s);
    vread(1, vt, size, t);
    for (int i = 0; i < n - 1; i++) d[0] += s[i] * t[i];
    d[0] += t[n - 1];
    diag("vhdp", s, d);
    vwrite(vd, 1, d);
}

/* vrot: a rotation row from one angle (in quarter turns). imm bits 0-1 name
 * the cosine lane, 2-3 the sine lane (all lanes when they are equal), bit 4
 * negates the sine; other lanes are zero. */
void psp_vrot(uint32_t vd, uint32_t vs, int size, uint32_t imm) {
    if (compat_new_op("vrot")) return;
    float s[4];
    vread(0, vs, 1, s);
    const float a = s[0] * 1.5707963267948966f;
    float sine = sinf(a), cosine = cosf(a);
    if (imm & 0x10) sine = -sine;
    const int sl = (int)(imm >> 2) & 3, cl = (int)imm & 3;
    float d[4] = { 0, 0, 0, 0 };
    if (sl == cl) for (int i = 0; i < 4; i++) d[i] = sine;
    else d[sl] = sine;
    d[cl] = cosine;
    diag("vrot", s, d);
    vwrite(vd, size, d);
}

/* VFPU9 reductions and friends. */
void psp_vfad(uint32_t vd, uint32_t vs, int size) {
    if (compat_new_op("vfad")) return;
    float s[4], d[4] = { 0 };
    const int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) d[0] += s[i];
    diag("vfad", s, d);
    vwrite(vd, 1, d);
}

void psp_vavg(uint32_t vd, uint32_t vs, int size) {
    if (compat_new_op("vavg")) return;
    float s[4], d[4] = { 0 };
    const int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) d[0] += s[i];
    d[0] /= (float)n;
    diag("vavg", s, d);
    vwrite(vd, 1, d);
}

/* vsgn: -1, 0 or 1 per lane; zero and denormals give 0. */
void psp_vsgn(uint32_t vd, uint32_t vs, int size) {
    if (compat_new_op("vsgn")) return;
    float s[4], d[4] = { 0 };
    const int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &s[i], 4);
        d[i] = (u & 0x7F800000u) == 0 ? 0.0f : ((u >> 31) ? -1.0f : 1.0f);
    }
    diag("vsgn", s, d);
    vwrite(vd, size, d);
}

/* vocp: one's complement, 1 - x. */
void psp_vocp(uint32_t vd, uint32_t vs, int size) {
    if (compat_new_op("vocp")) return;
    float s[4], d[4] = { 0 };
    const int n = vread(0, vs, size, s);
    for (int i = 0; i < n; i++) d[i] = s[i] != s[i] ? fabsf(s[i]) : 1.0f - s[i];
    diag("vocp", s, d);
    vwrite(vd, size, d);
}

/* ---- moves between the GPR and VFPU files ----------------------------------
 * mfv/mtv move one lane's bits. Register numbers 128 and up are the control
 * registers (mfvc/mtvc): 128-130 the S/T/D prefixes, 131 the condition codes. */
uint32_t psp_mfv(uint32_t reg) {
    if (reg < 128) {
        int r[4];
        psp_vfpu_regs(reg, 1, r);
        return psp_f32_to_bits(psp_cpu.v[r[0]]);
    }
    switch (reg) {
    case 128: case 129: return g_prefix_set[reg - 128] ? g_prefix[reg - 128] : 0xE4u;
    case 130: return g_prefix_set[2] ? g_prefix[2] : 0;
    case 131: return psp_cpu.vfpu_cc;
    case 135: return 0x7772CEABu;           /* VFPU revision, as hardware reports */
    default:  return 0;
    }
}

void psp_mtv(uint32_t reg, uint32_t value) {
    if (reg < 128) {
        int r[4];
        psp_vfpu_regs(reg, 1, r);
        psp_cpu.v[r[0]] = psp_bits_to_f32(value);
        return;
    }
    switch (reg) {
    case 128: case 129: case 130: psp_vfpu_set_prefix((int)reg - 128, value); break;
    case 131: psp_cpu.vfpu_cc = value & 0x3F; break;
    default:  break;
    }
}

void psp_vfpu_report(FILE *out) {
    fprintf(out, "VFPU new ops (calls, NaN/inf results):");
    for (int i = 0; i < 32 && g_seen[i].n; i++)
        fprintf(out, " %s %llu/%llu", g_seen[i].n, (unsigned long long)g_seen[i].c,
                (unsigned long long)g_seen[i].bad);
    fprintf(out, "\n");
}
