/* Emitter tests — synthetic code only, no game data.
 *
 * The function under test is eight instructions of hand-assembled MIPS chosen
 * to exercise the parts of emission that are actually hard: a conditional
 * branch with a delay slot, a backward-reachable label, and a `jr $ra` whose
 * delay slot must run before the return.
 *
 * The assertions are about the *shape* of the generated C, because that shape
 * is the contract. In particular the branch must read its condition into a
 * temporary before the delay slot runs — if that ever regresses, a delay slot
 * that writes a condition register silently changes which way the branch goes,
 * once in a thousand iterations, in a game nobody can debug.
 */

#include "analyze.h"
#include "emit.h"

#include <stdio.h>
#include <stdlib.h>
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

#define BASE 0x08804000u

/* addiu $sp,$sp,-16      prologue
 * sw    $ra,12($sp)
 * addu  $v0,$a0,$a1
 * beq   $v0,$zero,+2     -> the `jr $ra` at BASE+24
 * addiu $v0,$v0,1        delay slot: runs either way, and writes $v0 which
 *                        the branch above just read
 * lw    $ra,12($sp)
 * jr    $ra
 * addiu $sp,$sp,16       delay slot: runs before the return
 */
static const uint32_t CODE[] = {
    0x27BDFFF0u, 0xAFBF000Cu, 0x00851021u, 0x10400002u,
    0x24420001u, 0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
};

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = '\0';
    fclose(f);
    if (len) *len = got;
    return b;
}

/* Assert a substring appears, reporting what was missing if not. */
static void expect_contains(const char *hay, const char *needle, const char *why) {
    if (!strstr(hay, needle)) {
        printf("FAIL %s\n  expected to find: %s\n", why, needle);
        failures++;
    }
}

/* ---- a delay slot the branch does not own ---------------------------------
 *
 * A branch that leaves its body still runs its delay slot: the hardware
 * executes it whichever way the branch goes. When the slot belongs to *this*
 * body the emitter folds it into the branch, and the body that owns it skips
 * its own copy. When it belongs to another body -- discovery promotes a slot
 * address to a function entry on every real module -- there is no copy here to
 * fold in, and the walk used to step over the slot anyway. The body's `end`
 * then pointed one instruction past the address control actually reaches, and
 * the fall-out was looked for at an address that is neither a function entry
 * nor a label, so nothing was emitted at all: a silent `return`. A body that
 * stops early usually still holds a frame that only the continuation pops, so
 * its caller resumes with a $sp that is short by a frame.
 *
 * Hand-built analyses, not discovered ones: this shape is about where a body
 * *ends*, and discovery promotes and merges entries in an order-dependent way,
 * so a test for it must not itself depend on the discovery order.
 *
 *   08804000  sltiu $v0,$t2,16        the body under test
 *   08804004  beq   $v0,$zero,08804014
 *   08804008  addiu $v0,$zero,76      <- the delay slot: the first instruction
 *   0880400C  jr    $ra                  of the body below
 *   08804010  nop
 *   08804014  jr    $ra               the branch target, a function of its own
 *   08804018  nop
 */
static const uint32_t SLOT_NOT_OWNED_BRANCH[] = {
    0x2D420010u,   /* 08804000  sltiu $v0,$t2,16                  */
    0x10400003u,   /* 08804004  beq   $v0,$zero,08804014          */
    0x2402004Cu,   /* 08804008  addiu $v0,$zero,76   <- delay slot, not ours */
    0x03E00008u,   /* 0880400C  jr    $ra                         */
    0x00000000u,   /* 08804010  nop                               */
    0x03E00008u,   /* 08804014  jr    $ra                         */
    0x00000000u,   /* 08804018  nop                               */
};

/*  08804000  jal   08804010        the body under test
 *  08804004  addiu $v0,$zero,76     <- the call's delay slot, another body's
 *  08804008  jr    $ra                 first instruction
 *  0880400C  nop
 *  08804010  jr    $ra             the call target, a function of its own
 *  08804014  nop
 */
static const uint32_t SLOT_NOT_OWNED_CALL[] = {
    0x0C201004u,   /* 08804000  jal   08804010                    */
    0x2402004Cu,   /* 08804004  addiu $v0,$zero,76   <- delay slot, not ours */
    0x03E00008u,   /* 08804008  jr    $ra                         */
    0x00000000u,   /* 0880400C  nop                               */
    0x03E00008u,   /* 08804010  jr    $ra                         */
    0x00000000u,   /* 08804014  nop                               */
};

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* Build an analysis by hand over `code` with the given ownership. */
static void hand_built(a_analysis *an, uint32_t *owners, uint32_t nwords,
                       a_func *funcs, int nfuncs, const uint32_t *code) {
    memset(an, 0, sizeof *an);
    an->code = (uint8_t *)code;
    an->base = BASE;
    an->size = nwords * 4;
    an->nwords = nwords;
    an->owner = owners;
    an->funcs = funcs;
    an->nfuncs = nfuncs;
}

static char *emit_case(const a_analysis *an, const char *prefix) {
    emit_opts o;
    memset(&o, 0, sizeof o);
    o.outdir = ".";
    o.prefix = prefix;
    o.module = "synthetic";
    CHECK(a_emit(an, &o) == 0, "%s: emission succeeds", prefix);
    char path[128];
    snprintf(path, sizeof path, "./%s_funcs.c", prefix);
    return slurp(path, NULL);
}

static void check_slot_of_another_body(void) {
    /* Case one: a conditional branch whose delay slot is another body's first
     * instruction. On the previous behaviour this body ended in a silent
     * `return` -- one that also skipped the delay slot on the way. */
    {
        uint32_t owners[7] = { BASE, BASE,
                               BASE + 0x08u, BASE + 0x08u, BASE + 0x08u,
                               BASE + 0x14u, BASE + 0x14u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x0Cu;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x14u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x14u;  funcs[2].start = BASE + 0x14u;
        funcs[2].end  = BASE + 0x1Cu;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 3, SLOT_NOT_OWNED_BRANCH);
        char *src = emit_case(&an, "t_slot_branch");
        CHECK(src != NULL, "branch case: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls into 0x08804008, owned by another function */",
                            "the not-taken path continues at the delay slot");
            expect_contains(src, "psp_func_08804008();",
                            "and reaches it as a function, which is what it is");
            expect_contains(src, "PSP_SP_CHECK(0x08804008u);",
                            "with the stack observed across the call");
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *call = strstr(src, "psp_func_08804008();");
            CHECK(cond && call && cond < call,
                  "the continuation is emitted after the branch, not instead of it");
            /* The slot is emitted twice: once inside the taken path, once as
             * the first statement of the body that owns it, which is where the
             * NOT-taken path reaches it. Neither path can run both, and that is
             * the property to pin -- the two copies are checked where they
             * live, because a lone count would also accept the mistake of
             * folding the slot in on the fall-through path. */
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 2, "one copy per path, got %d", n);
            const char *take = strstr(src, "if (_c) {");
            const char *in_taken = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *gap = strstr(src, "/* falls into 0x08804008, owned by another function */");
            const char *in_owner = gap ? strstr(gap, "r_v0 = r_zero + 76;") : NULL;
            CHECK(in_taken && in_owner,
                  "one copy in the taken path, one after the fall-out");
            free(src);
        }
    }

    /* Case two: the same shape under a call, which is a separate line of the
     * emitter and would otherwise drift. */
    {
        uint32_t owners[6] = { BASE,
                               BASE + 0x04u, BASE + 0x04u, BASE + 0x04u,
                               BASE + 0x10u, BASE + 0x10u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 1;
        funcs[1].addr = BASE + 0x04u;  funcs[1].start = BASE + 0x04u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x10u;  funcs[2].start = BASE + 0x10u;
        funcs[2].end  = BASE + 0x18u;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 6, funcs, 3, SLOT_NOT_OWNED_CALL);
        char *src = emit_case(&an, "t_slot_call");
        CHECK(src != NULL, "call case: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls into 0x08804004, owned by another function */",
                            "after the call, control continues at the delay slot");
            expect_contains(src, "psp_func_08804004();",
                            "and reaches it as a function");
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 1, "the delay slot is still emitted exactly once, got %d", n);
            free(src);
        }
    }
}

/* The three shapes of "a branch whose delay slot another body owns", each built
 * by hand for the reason above: what matters is where the body ends and who
 * owns the slot, and discovery decides that by an order-dependent walk.
 *
 *   08804000  sltiu $v0,$t2,16
 *   08804004  beq   $v0,$zero,08804010     the body under test
 *   08804008  addiu $v0,$zero,76           <- the delay slot: another body's
 *   0880400C  jr    $ra
 *   08804010  nop                          the branch target, inside this body
 *   08804014  jr    $ra
 *   08804018  nop
 */
static const uint32_t TAKEN_SLOT_GOTO[] = {
    0x2D420010u, 0x10400002u, 0x2402004Cu, 0x03E00008u,
    0x00000000u, 0x03E00008u, 0x00000000u,
};

/* The same, with the branch leaving the body: the slot has to run before the
 * transfer, not after it.
 *
 *   08804000  beq   $v0,$zero,08804010     the body under test
 *   08804004  addiu $v0,$zero,76           <- the delay slot: another body's
 *   08804008  jr    $ra
 *   0880400C  nop
 *   08804010  jr    $ra                    the target, a function of its own
 *   08804014  nop
 */
static const uint32_t TAKEN_SLOT_TAIL[] = {
    0x10400003u, 0x2402004Cu, 0x03E00008u, 0x00000000u,
    0x03E00008u, 0x00000000u,
};

/* A likely branch: it nullifies its slot when NOT taken, so the slot belongs in
 * the taken path and nowhere else.
 *
 *   08804000  blezl $v0,08804010           the body under test
 *   08804004  addiu $v0,$zero,76           <- the delay slot: another body's
 *   08804008  jr    $ra
 *   0880400C  nop
 *   08804010  nop                          the branch target, inside this body
 *   08804014  jr    $ra
 *   08804018  nop
 */
static const uint32_t TAKEN_SLOT_LIKELY[] = {
    0x58400003u, 0x2402004Cu, 0x03E00008u, 0x00000000u,
    0x00000000u, 0x03E00008u, 0x00000000u,
};

/* A branch runs its delay slot whether or not it is taken. When the slot is
 * another body's the walk only reaches it by falling through, which is the
 * NOT-taken path -- so the taken path has to run it too, or it transfers with a
 * register the slot was supposed to set still holding its old value.
 *
 * Measured on LocoRoco (UCES00304): 0x1571C is `beq $v0,$zero,0x15774` whose
 * slot at 0x15720 is `lui $v0,0x1C`, and 0x15774 loads through $v0. The taken
 * path left it at zero and the game read the module's own code as a table. */
static void check_taken_path_runs_the_slot(void) {
    /* --- the branch stays inside its body --- */
    {
        uint32_t owners[7] = { BASE, BASE,
                               BASE + 0x08u, BASE + 0x08u,
                               BASE, BASE, BASE };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x1Cu;  funcs[0].insns = 5;
        funcs[0].has_return = 1;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 2, TAKEN_SLOT_GOTO);
        char *src = emit_case(&an, "t_taken_goto");
        CHECK(src != NULL, "goto case: generated .c is readable");
        if (src) {
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *take = cond ? strstr(cond, "if (_c) {") : NULL;
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *jump = slot ? strstr(slot, "goto L_08804010;") : NULL;
            CHECK(cond && take && slot && jump,
                  "the taken path runs the delay slot before it branches inside");
            /* One copy per path: this one for the taken path, the other in the
             * body that owns it, which is where the not-taken path picks it up.
             * Neither path can reach both. */
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 2, "one copy per path, got %d", n);
            free(src);
        }
    }

    /* --- the branch leaves the body through a transfer --- */
    {
        uint32_t owners[6] = { BASE,
                               BASE + 0x04u, BASE + 0x04u, BASE + 0x04u,
                               BASE + 0x10u, BASE + 0x10u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x04u;  funcs[0].insns = 1;
        funcs[1].addr = BASE + 0x04u;  funcs[1].start = BASE + 0x04u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x10u;  funcs[2].start = BASE + 0x10u;
        funcs[2].end  = BASE + 0x18u;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 6, funcs, 3, TAKEN_SLOT_TAIL);
        char *src = emit_case(&an, "t_taken_tail");
        CHECK(src != NULL, "tail case: generated .c is readable");
        if (src) {
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *take = cond ? strstr(cond, "if (_c) {") : NULL;
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *call = slot ? strstr(slot, "psp_func_08804010();") : NULL;
            CHECK(cond && take && slot && call,
                  "the taken path runs the delay slot before the transfer");
            free(src);
        }
    }

    /* --- a likely branch, where the slot belongs to the taken path alone --- */
    {
        uint32_t owners[7] = { BASE,
                               BASE + 0x08u, BASE + 0x08u, BASE + 0x08u,
                               BASE, BASE, BASE };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x1Cu;  funcs[0].insns = 5;
        funcs[0].has_return = 1;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 2, TAKEN_SLOT_LIKELY);
        char *src = emit_case(&an, "t_taken_likely");
        CHECK(src != NULL, "likely case: generated .c is readable");
        if (src) {
            const char *take = strstr(src, "if ((int32_t)r_v0 <= 0) {");
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *jump = slot ? strstr(slot, "goto L_08804010;") : NULL;
            CHECK(take && slot && jump,
                  "a likely branch runs its delay slot inside the taken path");
            free(src);
        }
    }
}

int main(void) {
    uint8_t code[sizeof CODE];
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BASE;
    an.size = (uint32_t)sizeof code;

    uint32_t seed = BASE;
    CHECK(a_discover(&an, &seed, 1) == 0, "discovery runs");
    CHECK(an.nfuncs == 1, "one function found, got %d", an.nfuncs);
    if (an.nfuncs != 1) return 1;
    CHECK(an.funcs[0].addr == BASE, "function entry is the seed");
    CHECK(an.funcs[0].has_return, "function reaches a `jr $ra`");
    CHECK(an.insns == 8, "all eight instructions visited, got %llu",
          (unsigned long long)an.insns);

    emit_opts o;
    o.outdir = ".";
    o.prefix = "t_emit";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "emission succeeds");

    char *src = slurp("./t_emit_funcs.c", NULL);
    CHECK(src != NULL, "generated .c is readable");
    if (!src) return 1;

    expect_contains(src, "void psp_func_08804000(void)",
                    "function is named after its address");

    /* The prologue, lowered to plain C. */
    expect_contains(src, "r_sp = r_sp + -16;", "addiu lowers to arithmetic");
    expect_contains(src, "psp_write32(r_sp + 12, r_ra);", "sw lowers to a memory write");
    expect_contains(src, "r_v0 = r_a0 + r_a1;", "addu lowers to addition");

    /* Every statement carries its address and disassembly. */
    expect_contains(src, "/* 08804000  addiu", "statements carry their disassembly");

    /* THE important one: the branch condition is captured into a temporary
     * *before* the delay slot executes, because the delay slot writes $v0 and
     * the hardware read the old value. */
    expect_contains(src, "int _c = (r_v0 == r_zero);",
                    "branch condition is captured before the delay slot");
    {
        const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
        const char *slot = strstr(src, "r_v0 = r_v0 + 1;");
        const char *jump = cond ? strstr(cond, "if (_c) goto") : NULL;
        CHECK(cond && slot && jump && cond < slot && slot < jump,
              "ordering must be condition -> delay slot -> branch");
    }

    /* The branch target is a label, and the return runs its delay slot first. */
    expect_contains(src, "L_08804018:", "branch target becomes a label");
    {
        const char *lbl = strstr(src, "L_08804018:");
        const char *slot = lbl ? strstr(lbl, "r_sp = r_sp + 16;") : NULL;
        const char *ret = slot ? strstr(slot, "return;") : NULL;
        CHECK(slot && ret, "the `jr $ra` delay slot is emitted before the return");
    }

    /* $zero is never assigned: `addu $v0,$a0,$a1` reads it, but nothing may
     * write it. A generated `r_zero =` would corrupt every later use. */
    CHECK(strstr(src, "r_zero =") == NULL, "nothing ever assigns $zero");

    free(src);

    /* The header declares the function and the registration entry point. */
    char *hdr = slurp("./t_emit_funcs.h", NULL);
    CHECK(hdr != NULL, "generated .h is readable");
    if (hdr) {
        expect_contains(hdr, "void psp_func_08804000(void);", "header declares the function");
        expect_contains(hdr, "void psp_recomp_register(void);", "header declares registration");
        expect_contains(hdr, "#define r_sp", "header defines register aliases");
        free(hdr);
    }

    /* Registration wires the address to the function. */
    src = slurp("./t_emit_funcs.c", NULL);
    if (src) {
        expect_contains(src, "psp_register(0x08804000u, psp_func_08804000);",
                        "the function is registered for indirect dispatch");
        free(src);
    }

    a_analysis_free(&an);

    check_slot_of_another_body();
    check_taken_path_runs_the_slot();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all emitter checks passed (synthetic code, no game data)\n");
    return 0;
}
