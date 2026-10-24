/* psprecomp — the VFPU.
 *
 * 128 single-precision registers, addressed as 8 matrices of 4x4. A single
 * 7-bit register field can name a scalar, a 2/3/4-element row, a column, or a
 * whole matrix, depending on the instruction's width bits and a transpose bit.
 *
 * ## Register layout
 *
 * The file is indexed `v[matrix*4 + column*32 + row]`. That looks arbitrary
 * and is not: it is the layout that makes a row and a column of the same
 * matrix alias the same storage the way the hardware does. A game that writes
 * a matrix by columns and reads it by rows -- which is exactly what a
 * transpose does -- only works if this matches.
 *
 * ## What is implemented, and what deliberately is not
 *
 * Measured against a real module (WTF's Lumberjack, 1276 VFPU-space words in
 * .text), the distribution is heavily skewed: quad load/store is 48% of it,
 * and vscl/vmul/vadd/vdot another 18%. Those, plus the compare/min/max family,
 * are implemented here and unit-tested.
 *
 * The **prefix** instructions `vpfxs`/`vpfxt`/`vpfxd` do not compute
 * anything -- they set a register that rewrites the *operands of the next
 * instruction*, swizzling lanes, negating, forcing constants, saturating and
 * masking writes. Every vector op reads its operands and writes its result
 * through them (vfpu.c: vread/vwrite), following PPSSPP's interpreter.
 * Matrix ops ignore and consume them.
 */
#ifndef PSPRECOMP_VFPU_H
#define PSPRECOMP_VFPU_H

#include "cpu.h"
#include "mem.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a 7-bit register field and width into up to four indices into
 * psp_cpu.v[]. Returns the number of lanes. */
int psp_vfpu_regs(uint32_t vreg, int size, int out[4]);

/* Load/store. Quad forms are 16-byte aligned on hardware; the address is
 * masked rather than faulting, which is what the hardware does. */
void psp_lv_s(uint32_t vt, uint32_t addr);
void psp_lv_q(uint32_t vt, uint32_t addr);
void psp_sv_s(uint32_t vt, uint32_t addr);
void psp_sv_q(uint32_t vt, uint32_t addr);

/* Element-wise arithmetic across `size` lanes. */
void psp_vadd(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vsub(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vdiv(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmin(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmax(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Reductions. vdot writes one lane; vscl scales a vector by a scalar. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Comparison, writing the VFPU condition codes. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size);
void psp_vsge(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vslt(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Conditional move, cross products, determinant, homogeneous dot, rotation.
 * vcmov takes the raw instruction word for its condition fields; vrot its
 * 5-bit immediate (bits 16..20). */
void psp_vcmov(uint32_t vd, uint32_t vs, int size, uint32_t raw);
void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vdet(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vrot(uint32_t vd, uint32_t vs, int size, uint32_t imm);

/* VFPU9: sum, average, sign, 1 - x. */
void psp_vfad(uint32_t vd, uint32_t vs, int size);
void psp_vavg(uint32_t vd, uint32_t vs, int size);
void psp_vsgn(uint32_t vd, uint32_t vs, int size);
void psp_vocp(uint32_t vd, uint32_t vs, int size);

/* mfv/mtv (and mfvc/mtvc for reg >= 128): one lane's bits to or from a GPR. */
uint32_t psp_mfv(uint32_t reg);
void     psp_mtv(uint32_t reg, uint32_t value);

/* Unary element-wise ops (VFPU4). One entry point rather than eighteen, since
 * they differ only in the scalar function applied per lane. */
enum {
    PSP_VU_MOV = 0, PSP_VU_ABS, PSP_VU_NEG, PSP_VU_ZERO, PSP_VU_ONE,
    PSP_VU_RCP, PSP_VU_RSQ, PSP_VU_SQRT, PSP_VU_SIN, PSP_VU_COS,
    PSP_VU_EXP2, PSP_VU_LOG2, PSP_VU_SAT0, PSP_VU_SAT1,
    PSP_VU_NRCP, PSP_VU_NSIN, PSP_VU_REXP2, PSP_VU_ASIN,
    PSP_VU_F2IZ, PSP_VU_I2F,
    PSP_VU_F2IN, PSP_VU_F2IU, PSP_VU_F2ID    /* psp_vcvt only */
};
void psp_vunary(int op, uint32_t vd, uint32_t vs, int size);

/* The conversions with their 5-bit scale immediate. vf2in/vf2iz/vf2iu/vf2id
 * (PSP_VU_F2IN..F2ID) convert x * 2^scale, rounding to nearest, toward zero,
 * up and down; vi2f (PSP_VU_I2F) produces int * 2^-scale. psp_vunary's
 * F2IZ and I2F cases are this with scale 0. */
void psp_vcvt(int op, uint32_t vd, uint32_t vs, int size, int scale);

/* Matrix ops that need no multiply: identity, zero, one, and copy. `size` is
 * the matrix order (2, 3 or 4). */
void psp_vmidt(uint32_t vd, int size);
void psp_vidt(uint32_t vd, int size);
void psp_vimm(uint32_t vd, float value);
void psp_vcst(uint32_t vd, uint32_t which, int size);
void psp_vmzero(uint32_t vd, int size);
void psp_vmone(uint32_t vd, int size);
void psp_vmmov(uint32_t vd, uint32_t vs, int size);

/* Matrix multiply, transform and scale. See the note in vfpu.c: the operand
 * orientation of vmmul is not independently verified. */
void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size);
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size);

/* Prefix state. Set by vpfxs/vpfxt/vpfxd; consumed (and cleared) by the next
 * vector instruction. */
void psp_vfpu_set_prefix(int which, uint32_t value);
int  psp_vfpu_prefix_pending(void);
void psp_vfpu_reset(void);

/* Reports an instruction the VFPU cannot yet execute, by address and name. */
void psp_vfpu_unimplemented(uint32_t addr, const char *what);
uint64_t psp_vfpu_trap_count(void);

/* For a host's report: how often each newly implemented op ran and how many
 * results were NaN or infinite. */
void psp_vfpu_report(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_VFPU_H */
