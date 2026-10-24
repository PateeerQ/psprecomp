/* VFPU tests — synthetic data only.
 *
 * Two things get pinned here. The first is register addressing, because the
 * layout is what makes a matrix row and column alias correctly and everything
 * else is built on it. The second is the prefixes: every op reads and writes
 * through them, and a prefix that is ignored produces silently wrong numbers.
 */

#include "psprecomp/vfpu.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define CHECK_F(got, want, label)                                            \
    CHECK((got) > (want) - 1e-5f && (got) < (want) + 1e-5f,                   \
          "%s: got %f, want %f", (label), (double)(got), (double)(want))

static void test_register_addressing(void) {
    int r[4];

    /* A quad in matrix 0, column 0 is four consecutive rows: indices
     * 0, 1, 2, 3 under the mtx*4 + col*32 + row layout. */
    int n = psp_vfpu_regs(0x00, 4, r);
    CHECK(n == 4, "a quad names four registers, got %d", n);
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3,
          "quad M000: got %d,%d,%d,%d", r[0], r[1], r[2], r[3]);

    /* A single names exactly one. */
    n = psp_vfpu_regs(0x00, 1, r);
    CHECK(n == 1 && r[0] == 0, "single names one register");

    /* The transpose bit (bit 5) walks the other axis. Column-major access of
     * the same matrix must land on a *different* set of registers -- if it did
     * not, transposing a matrix would be a no-op and every rotation would be
     * wrong. */
    int rowwise[4], colwise[4];
    psp_vfpu_regs(0x00, 4, rowwise);
    psp_vfpu_regs(0x20, 4, colwise);
    CHECK(memcmp(rowwise, colwise, sizeof rowwise) != 0,
          "transposed access reaches different registers");
    CHECK(colwise[0] == 0 && colwise[1] == 32 && colwise[2] == 64 && colwise[3] == 96,
          "transposed quad strides by column: got %d,%d,%d,%d",
          colwise[0], colwise[1], colwise[2], colwise[3]);

    /* Every index must stay inside the 128-register file, for every width and
     * every possible field value. */
    for (uint32_t vreg = 0; vreg < 128; vreg++) {
        for (int size = 1; size <= 4; size++) {
            int idx[4];
            int len = psp_vfpu_regs(vreg, size, idx);
            for (int i = 0; i < len; i++) {
                if (idx[i] < 0 || idx[i] >= 128) {
                    printf("FAIL vreg=0x%02X size=%d lane %d -> %d (out of range)\n",
                           vreg, size, i, idx[i]);
                    failures++;
                }
            }
        }
    }
}

static void test_load_store(void) {
    const uint32_t AT = 0x08860000u;

    /* Quad round-trip through guest memory. */
    float in[4] = { 1.5f, -2.25f, 3.75f, 0.5f };
    for (int i = 0; i < 4; i++) psp_write_f32(AT + (uint32_t)i * 4, in[i]);

    psp_lv_q(0x00, AT);
    int r[4];
    psp_vfpu_regs(0x00, 4, r);
    for (int i = 0; i < 4; i++) CHECK_F(psp_cpu.v[r[i]], in[i], "lv.q lane");

    psp_sv_q(0x00, AT + 64);
    for (int i = 0; i < 4; i++)
        CHECK_F(psp_read_f32(AT + 64 + (uint32_t)i * 4), in[i], "sv.q lane");

    /* Quad addresses are 16-byte aligned; a misaligned address is masked down
     * rather than faulting, which is what the hardware does. */
    psp_lv_q(0x04, AT + 7);
    int r2[4];
    psp_vfpu_regs(0x04, 4, r2);
    CHECK_F(psp_cpu.v[r2[0]], in[0], "misaligned lv.q masks the address");

    psp_lv_s(0x08, AT + 4);
    int r3[4];
    psp_vfpu_regs(0x08, 1, r3);
    CHECK_F(psp_cpu.v[r3[0]], in[1], "lv.s");
}

/* Load a quad register from an array, for the arithmetic tests. */
static void set_quad(uint32_t vreg, const float f[4]) {
    int r[4];
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) psp_cpu.v[r[i]] = f[i];
}
static void get_quad(uint32_t vreg, float f[4]) {
    int r[4];
    psp_vfpu_regs(vreg, 4, r);
    for (int i = 0; i < 4; i++) f[i] = psp_cpu.v[r[i]];
}

static void test_arithmetic(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
    float out[4];

    /* Three different matrices so vd, vs and vt do not alias. */
    set_quad(0x00, a);
    set_quad(0x04, b);

    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] + b[i], "vadd lane");

    psp_vmul(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] * b[i], "vmul lane");

    psp_vsub(0x08, 0x04, 0x00, 4);
    get_quad(0x08, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], b[i] - a[i], "vsub lane");

    /* vdot collapses to one lane: 1*5 + 2*6 + 3*7 + 4*8 = 70. */
    psp_vdot(0x08, 0x00, 0x04, 4);
    int d[4];
    psp_vfpu_regs(0x08, 1, d);
    CHECK_F(psp_cpu.v[d[0]], 70.0f, "vdot");

    /* vscl multiplies every lane by a scalar. */
    int s[4];
    psp_vfpu_regs(0x0C, 1, s);
    psp_cpu.v[s[0]] = 3.0f;
    psp_vscl(0x10, 0x00, 0x0C, 4);
    get_quad(0x10, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] * 3.0f, "vscl lane");

    /* Destination aliasing a source must still be correct: every source lane
     * has to be read before any destination lane is written. */
    set_quad(0x00, a);
    set_quad(0x04, b);
    psp_vadd(0x00, 0x00, 0x04, 4);
    get_quad(0x00, out);
    for (int i = 0; i < 4; i++) CHECK_F(out[i], a[i] + b[i], "vadd into its own source");
}

static void test_prefixes(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 10.0f, 20.0f, 30.0f, 40.0f };
    set_quad(0x00, a);
    set_quad(0x04, b);
    float out[4];

    /* S: swizzle wzyx (3,2,1,0), negate lane 1. */
    psp_vfpu_set_prefix(0, 0x1B | (1u << 17));
    CHECK(psp_vfpu_prefix_pending(), "prefix registers as pending");
    psp_vadd(0x08, 0x00, 0x04, 4);
    CHECK(!psp_vfpu_prefix_pending(), "the prefix is consumed by the op");
    get_quad(0x08, out);
    CHECK_F(out[0], 14.0f, "swizzled lane 0");
    CHECK_F(out[1], 17.0f, "swizzled, negated lane 1");
    CHECK_F(out[3], 41.0f, "swizzled lane 3");

    /* T: constants 1/2 (swizzle 3), 3 (swizzle 0 + abs) in lanes 0 and 1. */
    psp_vfpu_set_prefix(1, 0x03 | (1u << 9) | (1u << 12) | (1u << 13) | (0xE4 & 0xF0));
    psp_vmul(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 0.5f, "constant 1/2");
    CHECK_F(out[1], 6.0f, "constant 3 via abs");
    CHECK_F(out[2], 90.0f, "unprefixed lane");

    /* D: saturate lane 0 to [0,1], mask lane 3. */
    const float keep[4] = { 7, 7, 7, 7 };
    set_quad(0x08, keep);
    psp_vfpu_set_prefix(2, 1u | (1u << 11));
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 1.0f, "saturated lane");
    CHECK_F(out[1], 22.0f, "lane 1 unchanged by D");
    CHECK_F(out[3], 7.0f, "masked lane not written");

    /* No prefix: plain arithmetic, and no trap is ever counted. */
    psp_vadd(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 11.0f, "no prefix after consumption");
    CHECK(psp_vfpu_trap_count() == 0, "prefixed ops compute instead of trapping");
}

static void test_more_ops(void) {
    psp_vfpu_reset();
    float out[4];
    const float x[4] = { 1, 0, 0, 0 }, y[4] = { 0, 1, 0, 0 };
    set_quad(0x00, x);
    set_quad(0x04, y);

    psp_vcrsp(0x08, 0x00, 0x04, 3);                /* x cross y = z */
    get_quad(0x08, out);
    CHECK_F(out[0], 0.0f, "vcrsp x");
    CHECK_F(out[2], 1.0f, "vcrsp z");

    const float p[4] = { 3, 4, 0, 0 }, q[4] = { 1, 2, 0, 0 };
    set_quad(0x00, p);
    set_quad(0x04, q);
    psp_vdet(0x08, 0x00, 0x04, 2);
    get_quad(0x08, out);
    CHECK_F(out[0], 2.0f, "vdet 3*2 - 4*1");

    psp_vhdp(0x08, 0x00, 0x04, 3);                 /* 3*1 + 4*2 + q.z(0) */
    get_quad(0x08, out);
    CHECK_F(out[0], 11.0f, "vhdp");

    psp_vsge(0x08, 0x00, 0x04, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 1.0f, "vsge 3 >= 1");
    CHECK_F(out[2], 1.0f, "vsge 0 >= 0");
    psp_vslt(0x08, 0x04, 0x00, 4);
    get_quad(0x08, out);
    CHECK_F(out[1], 1.0f, "vslt 2 < 4");
    CHECK_F(out[2], 0.0f, "vslt 0 < 0");

    const float v[4] = { -2, 0, 5, 1 };
    set_quad(0x00, v);
    psp_vsgn(0x08, 0x00, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], -1.0f, "vsgn negative");
    CHECK_F(out[1], 0.0f, "vsgn zero");
    CHECK_F(out[2], 1.0f, "vsgn positive");
    psp_vfad(0x08, 0x00, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 4.0f, "vfad");
    psp_vavg(0x08, 0x00, 4);
    get_quad(0x08, out);
    CHECK_F(out[0], 1.0f, "vavg");
    psp_vocp(0x08, 0x00, 4);
    get_quad(0x08, out);
    CHECK_F(out[2], -4.0f, "vocp 1 - 5");

    /* vrot.q with cosine in lane 0, sine in lane 1, at a quarter turn/2. */
    const float ang[4] = { 0.5f, 0, 0, 0 };
    set_quad(0x00, ang);
    psp_vrot(0x08, 0x00, 4, (1u << 2) | 0u);
    get_quad(0x08, out);
    CHECK_F(out[0], 0.70710678f, "vrot cosine lane");
    CHECK_F(out[1], 0.70710678f, "vrot sine lane");
    CHECK_F(out[2], 0.0f, "vrot other lane");

    /* vcmov: copy where CC bit 0 is set (tf = 0 means "if true"), per lane. */
    const float src[4] = { 9, 9, 9, 9 }, dst[4] = { 1, 1, 1, 1 };
    set_quad(0x00, src);
    set_quad(0x08, dst);
    psp_cpu.vfpu_cc = 0x5;                          /* lanes 0 and 2 */
    psp_vcmov(0x08, 0x00, 4, 6u << 16);
    get_quad(0x08, out);
    CHECK_F(out[0], 9.0f, "vcmov lane 0 copied");
    CHECK_F(out[1], 1.0f, "vcmov lane 1 kept");
    CHECK_F(out[2], 9.0f, "vcmov lane 2 copied");

    /* mfv/mtv round-trip, and the CC control register. */
    psp_mtv(0x00, 0x40490FDBu);
    CHECK(psp_mfv(0x00) == 0x40490FDBu, "mtv/mfv keep the bits");
    psp_mtv(131, 0x21);
    CHECK(psp_cpu.vfpu_cc == 0x21 && psp_mfv(131) == 0x21, "mtvc/mfvc on CC");
}

static void test_compare(void) {
    psp_vfpu_reset();

    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 1.0f, 9.0f, 0.0f, 4.0f };
    set_quad(0x00, a);
    set_quad(0x04, b);

    psp_vcmp(1 /* EQ */, 0x00, 0x04, 4);
    /* Lanes 0 and 3 are equal, so bits 0 and 3, plus "any" but not "all". */
    CHECK((psp_cpu.vfpu_cc & 0xF) == 0x9,
          "vcmp EQ per-lane bits: got 0x%X", psp_cpu.vfpu_cc & 0xF);
    CHECK(psp_cpu.vfpu_cc & (1u << 4), "the any-lane bit is set");
    CHECK(!(psp_cpu.vfpu_cc & (1u << 5)), "the all-lanes bit is not");

    psp_vcmp(1, 0x00, 0x00, 4);
    CHECK(psp_cpu.vfpu_cc & (1u << 5), "comparing a vector to itself sets all-lanes");
}

/* Read/write a whole matrix through the same addressing the ops use. */
static void set_matrix(uint32_t v, int n, const float m[4][4]) {
    const uint32_t mtx = (v >> 2) & 7;
    for (int c = 0; c < n; c++) {
        int r[4];
        psp_vfpu_regs((mtx << 2) | (uint32_t)c, n, r);
        for (int i = 0; i < n; i++) psp_cpu.v[r[i]] = m[c][i];
    }
}
static void get_matrix(uint32_t v, int n, float m[4][4]) {
    const uint32_t mtx = (v >> 2) & 7;
    for (int c = 0; c < n; c++) {
        int r[4];
        psp_vfpu_regs((mtx << 2) | (uint32_t)c, n, r);
        for (int i = 0; i < n; i++) m[c][i] = psp_cpu.v[r[i]];
    }
}

static void test_matrix_ops(void) {
    psp_vfpu_reset();
    float m[4][4], out[4][4];

    /* vmidt must produce a real identity: ones on the diagonal, zeros
     * elsewhere. This is what catches the column-addressing bug -- a helper
     * that walks rows while claiming to walk columns still writes four ones,
     * just in the wrong places, so only checking the off-diagonal zeros
     * distinguishes them. */
    psp_vmidt(0x00, 4);
    get_matrix(0x00, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            CHECK_F(out[c][r], (c == r) ? 1.0f : 0.0f, "vmidt element");

    psp_vmzero(0x04, 4);
    get_matrix(0x04, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], 0.0f, "vmzero element");

    /* vmmov must copy every element, including off-diagonal ones -- a
     * diagonal-only copy would pass an identity round-trip. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) m[c][r] = (float)(c * 4 + r + 1);
    set_matrix(0x08, 4, m);
    psp_vmmov(0x0C, 0x08, 4);
    get_matrix(0x0C, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r], "vmmov element");

    /* Scaling is orientation-independent, so it can be checked outright. */
    int k[4];
    psp_vfpu_regs(0x40, 1, k);
    psp_cpu.v[k[0]] = 2.5f;
    psp_vmscl(0x10, 0x08, 0x40, 4);
    get_matrix(0x10, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r] * 2.5f, "vmscl element");
}

/* vmmul's orientation, from hand-worked numbers. With A = [1 2; 3 4] and
 * B = [5 6; 7 8] (rows), A*B = [19 22; 43 50]. The code idiom for that is
 * vmmul.q Md, E_A, M_B; the plain form gives A^T * B = [26 30; 38 44]. */
static void test_vmmul_orientation(void) {
    psp_vfpu_reset();
    float a[4][4] = {{0}}, b[4][4] = {{0}}, out[4][4];
    a[0][0] = 1; a[1][0] = 2; a[0][1] = 3; a[1][1] = 4;    /* [col][row] */
    b[0][0] = 5; b[1][0] = 6; b[0][1] = 7; b[1][1] = 8;
    set_matrix(0x08, 2, a);                 /* matrix 2 */
    set_matrix(0x10, 2, b);                 /* matrix 4 */

    psp_vmmul(0x00, 0x28, 0x10, 2);         /* M000 = E200 x M400 */
    get_matrix(0x00, 2, out);
    CHECK_F(out[0][0], 19.0f, "E-form vmmul is A*B: row 0 col 0");
    CHECK_F(out[1][0], 22.0f, "E-form vmmul is A*B: row 0 col 1");
    CHECK_F(out[0][1], 43.0f, "E-form vmmul is A*B: row 1 col 0");
    CHECK_F(out[1][1], 50.0f, "E-form vmmul is A*B: row 1 col 1");

    psp_vmmul(0x00, 0x08, 0x10, 2);         /* plain form: A^T * B */
    get_matrix(0x00, 2, out);
    CHECK_F(out[0][0], 26.0f, "plain vmmul is A^T*B: row 0 col 0");
    CHECK_F(out[1][1], 44.0f, "plain vmmul is A^T*B: row 1 col 1");
}

static void test_matrix_transform(void) {
    psp_vfpu_reset();
    float m[4][4], v[4];

    /* vtfm takes each output lane as the dot of a matrix column with the
     * vector (PPSSPP's Int_Vtfm, whose ReadMatrix has this [col][row] order):
     * the first operand transposed, like vmmul. So a basis vector picks the
     * same lane out of every column. This property is what tells the
     * convention from its transpose -- an identity test passes either way --
     * and the transpose made MHFU's hunter walk mirrored from where it faced. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) m[c][r] = (float)(c * 4 + r + 1);
    set_matrix(0x08, 4, m);

    for (int basis = 0; basis < 4; basis++) {
        int t[4], d[4];
        psp_vfpu_regs(0x40, 4, t);
        for (int i = 0; i < 4; i++) psp_cpu.v[t[i]] = (i == basis) ? 1.0f : 0.0f;

        psp_vtfm(0x44, 0x08, 0x40, 4);
        psp_vfpu_regs(0x44, 4, d);
        for (int i = 0; i < 4; i++)
            CHECK_F(psp_cpu.v[d[i]], m[i][basis], "vtfm of a basis vector is that lane of each column");
    }

    /* vhtfm4: a three-lane vector with a 1 appended, through a 4x4 matrix
     * (side 4 in bits 4-6, vector size 3). */
    {
        int t[4], d[4];
        psp_vfpu_regs(0x40, 3, t);
        const float h[3] = { 2.0f, -1.0f, 0.5f };
        for (int i = 0; i < 3; i++) psp_cpu.v[t[i]] = h[i];
        psp_vtfm(0x44, 0x08, 0x40, 3 | 4 << 4);
        psp_vfpu_regs(0x44, 4, d);
        for (int i = 0; i < 4; i++)
            CHECK_F(psp_cpu.v[d[i]], m[i][0] * h[0] + m[i][1] * h[1] + m[i][2] * h[2] + m[i][3],
                    "vhtfm4 appends a 1 to the vector");
    }

    /* Multiplying by the identity must be a no-op, in both operand positions.
     * vmmul uses its first operand transposed, so M * I is written with M's
     * transposed view, 0x28 (the E form of matrix 2). */
    psp_vmidt(0x00, 4);
    psp_vmmul(0x0C, 0x28, 0x00, 4);
    float out[4][4];
    get_matrix(0x0C, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r], "M * I == M");

    psp_vmmul(0x0C, 0x00, 0x08, 4);
    get_matrix(0x0C, 4, out);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK_F(out[c][r], m[c][r], "I * M == M");

    /* vmmul and vtfm must agree: transforming by a product is the same as
     * transforming twice. Both use their first operand transposed, so with
     * P = vmmul(E-form A, B) = AB, vtfm(P, x) = (AB)^T x = B^T (A^T x). */
    float a[4][4], b[4][4];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            a[c][r] = (float)((c + 1) * (r + 2) % 7) - 3.0f;
            b[c][r] = (float)((c + 3) * (r + 1) % 5) - 2.0f;
        }
    /* Every operand needs its OWN matrix. A register's matrix is (vreg>>2)&7,
     * so 0x08 and 0x48 are both matrix 2 -- an earlier version of this test
     * used both and quietly overwrote A while computing B*x, then blamed the
     * implementation. Matrices here: A=2, B=4, AB=5, x=6, results in 7/0/1. */
    set_matrix(0x08, 4, a);   /* matrix 2 */
    set_matrix(0x10, 4, b);   /* matrix 4 */

    int t[4];
    psp_vfpu_regs(0x18, 4, t);            /* matrix 6 */
    const float in[4] = { 1.0f, -2.0f, 0.5f, 3.0f };
    for (int i = 0; i < 4; i++) psp_cpu.v[t[i]] = in[i];

    /* (A*B) * x */
    psp_vmmul(0x14, 0x28, 0x10, 4);       /* matrix 5 = A*B, via E-form A */
    psp_vtfm(0x1C, 0x14, 0x18, 4);        /* matrix 7 */
    float combined[4];
    int d[4];
    psp_vfpu_regs(0x1C, 4, d);
    for (int i = 0; i < 4; i++) combined[i] = psp_cpu.v[d[i]];

    /* B^T (A^T x) */
    psp_vtfm(0x00, 0x08, 0x18, 4);        /* matrix 0 = A^T x */
    psp_vtfm(0x04, 0x10, 0x00, 4);        /* matrix 1 = B^T (A^T x) */
    psp_vfpu_regs(0x04, 4, d);
    for (int i = 0; i < 4; i++)
        CHECK_F(psp_cpu.v[d[i]], combined[i],
                "vmmul and vtfm agree: (AB)^T x == B^T (A^T x)");
}

/* The conversions. Every row is a measurement from a PSP-3000, firmware 6.61
 * (pspautotests cpu/vfpu/convert_scaled.expected, and convert.expected for the
 * scale-0 rows): register bits in, register bits out. Not models, so the
 * edges are the hardware's: saturation, NaN of either sign, ties, and inputs
 * only the scale makes nonzero. */
static void test_conversions(void) {
    static const struct {
        int op, scale;
        uint32_t in[4], want[4];
    } T[] = {
        /* NaN, -NaN, +Inf, -Inf */
        { PSP_VU_F2IZ, 1,  { 0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000 },
                           { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000 } },
        /* 0.3, -0.3, 0.7, -0.7: truncation toward zero */
        { PSP_VU_F2IZ, 5,  { 0x3E99999A, 0xBE99999A, 0x3F333333, 0xBF333333 },
                           { 0x00000009, 0xFFFFFFF7, 0x00000016, 0xFFFFFFEA } },
        /* 1000, -1000, 65535.5, -65535.5: the last two saturate */
        { PSP_VU_F2IZ, 16, { 0x447A0000, 0xC47A0000, 0x477FFF80, 0xC77FFF80 },
                           { 0x03E80000, 0xFC180000, 0x7FFFFFFF, 0x80000000 } },
        /* 1e-6, -1e-6, 1e-9, -1e-9: nonzero only because of the scale */
        { PSP_VU_F2IZ, 31, { 0x358637BD, 0xB58637BD, 0x3089705F, 0xB089705F },
                           { 0x00000863, 0xFFFFF79D, 0x00000002, 0xFFFFFFFE } },
        { PSP_VU_I2F,  1,  { 0x7FFFFFFF, 0x80000000, 0x40000000, 0xC0000000 },
                           { 0x4E800000, 0xCE800000, 0x4E000000, 0xCE000000 } },
        /* 16.16 fixed point */
        { PSP_VU_I2F,  16, { 0x12345678, 0xEDCBA988, 0x00000003, 0xFFFFFFFD },
                           { 0x4591A2B4, 0xC591A2B4, 0x38400000, 0xB8400000 } },
        { PSP_VU_I2F,  31, { 0x7FFFFFFF, 0x80000000, 0x40000000, 0xC0000000 },
                           { 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000 } },

        /* vf2in: 2, 2.5, 3.5, 1000 and their negatives -- ties go to even */
        { PSP_VU_F2IN, 0,  { 0x40000000, 0x40200000, 0x40600000, 0x447A0000 },
                           { 0x00000002, 0x00000002, 0x00000004, 0x000003E8 } },
        { PSP_VU_F2IN, 0,  { 0xC0000000, 0xC0200000, 0xC0600000, 0xC47A0000 },
                           { 0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFC18 } },
        { PSP_VU_F2IN, 5,  { 0x3E99999A, 0xBE99999A, 0x3F333333, 0xBF333333 },
                           { 0x0000000A, 0xFFFFFFF6, 0x00000016, 0xFFFFFFEA } },
        { PSP_VU_F2IN, 31, { 0x358637BD, 0xB58637BD, 0x3089705F, 0xB089705F },
                           { 0x00000863, 0xFFFFF79D, 0x00000002, 0xFFFFFFFE } },
        /* vf2iu rounds up */
        { PSP_VU_F2IU, 5,  { 0x3E99999A, 0xBE99999A, 0x3F333333, 0xBF333333 },
                           { 0x0000000A, 0xFFFFFFF7, 0x00000017, 0xFFFFFFEA } },
        { PSP_VU_F2IU, 5,  { 0x358637BD, 0xB58637BD, 0x3089705F, 0xB089705F },
                           { 0x00000001, 0x00000000, 0x00000001, 0x00000000 } },
        /* 1.5, -1.5, 2.5, -2.5 */
        { PSP_VU_F2IU, 31, { 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000 },
                           { 0x7FFFFFFF, 0x80000000, 0x7FFFFFFF, 0x80000000 } },
        /* vf2id rounds down */
        { PSP_VU_F2ID, 5,  { 0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000 },
                           { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000 } },
        { PSP_VU_F2ID, 5,  { 0x3E99999A, 0xBE99999A, 0x3F333333, 0xBF333333 },
                           { 0x00000009, 0xFFFFFFF6, 0x00000016, 0xFFFFFFE9 } },
        { PSP_VU_F2ID, 31, { 0x358637BD, 0xB58637BD, 0x3089705F, 0xB089705F },
                           { 0x00000863, 0xFFFFF79C, 0x00000002, 0xFFFFFFFD } },
    };
    static const char *const NAME[] = {
        [PSP_VU_F2IN] = "vf2in", [PSP_VU_F2IZ] = "vf2iz", [PSP_VU_F2IU] = "vf2iu",
        [PSP_VU_F2ID] = "vf2id", [PSP_VU_I2F]  = "vi2f",
    };

    psp_vfpu_reset();
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {
        int s[4], d[4];
        psp_vfpu_regs(0x04, 4, s);                   /* C100 */
        psp_vfpu_regs(0x08, 4, d);                   /* C200 */
        for (int i = 0; i < 4; i++) memcpy(&psp_cpu.v[s[i]], &T[t].in[i], 4);
        psp_vcvt(T[t].op, 0x08, 0x04, 4, T[t].scale);
        for (int i = 0; i < 4; i++) {
            uint32_t got;
            memcpy(&got, &psp_cpu.v[d[i]], 4);
            CHECK(got == T[t].want[i], "%s scale %d, lane %d: 0x%08X -> 0x%08X, want 0x%08X",
                  NAME[T[t].op], T[t].scale, i,
                  T[t].in[i], got, T[t].want[i]);
        }
    }
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_vfpu_reset();

    test_register_addressing();
    test_load_store();
    test_arithmetic();
    test_prefixes();
    test_more_ops();
    test_compare();
    test_matrix_ops();
    test_matrix_transform();
    test_vmmul_orientation();
    test_conversions();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all VFPU checks passed\n");
    return 0;
}
