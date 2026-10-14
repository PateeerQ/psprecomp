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

/* ---- the end of a body: where its flow falls out to ---------------------- */

/* Hand-built analyses, not discovered ones.
 *
 * The shape under test is "the address after this body belongs to another
 * body", and discovery promotes and merges entries in an order-dependent way
 * (that ordering is half of what went wrong here), so a test for it must not
 * itself depend on the discovery order. The maps are filled in directly.
 *
 * Case one: 0x08804000 ends after two instructions and falls into 0x08804008,
 * which is a label of the body at 0x0880400C -- pulled below that entry by its
 * own backward jump. Case two: the same fall-out, but the address after the
 * body *is* another function's entry.
 */
static const uint32_t FALL_OUT_LABEL[] = {
    0x3C020016u,   /* 08804000  lui   $v0,0x16      -- falls out of its body */
    0x2442D8DCu,   /* 08804004  addiu $v0,$v0,-10084 */
    0x03E00008u,   /* 08804008  jr    $ra           -- owned by the body below */
    0x0A201002u,   /* 0880400C  j     0x08804008    -- which lives below its entry */
};

static const uint32_t FALL_OUT_FUNC[] = {
    0x3C020016u,   /* 08804000  lui   $v0,0x16      -- falls out of its body */
    0x2442D8DCu,   /* 08804004  addiu $v0,$v0,-10084 */
    0x03E00008u,   /* 08804008  jr    $ra           -- this one IS a function entry */
};

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

static void check_fall_out(void) {
    /* Case one: continue at the label, through the thunk that exists for it.
     * On the previous behaviour this file ended the C function here, i.e. a
     * silent `return`, and the flow it was supposed to continue was lost. */
    {
        uint32_t owners[4] = { BASE, BASE, BASE + 0x0Cu, BASE + 0x0Cu };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x0Cu;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;
        funcs[1].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 4, funcs, 2, FALL_OUT_LABEL);
        char *src = emit_case(&an, "t_fallout_label");
        CHECK(src != NULL, "case one: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls through into 0x08804008, a label of another body */",
                            "a body that falls out into another body's label says so");
            expect_contains(src, "psp_at_08804008();",
                            "and continues there, through that label's thunk");
            /* The thunk is defined with the body that owns the label, which is
             * emitted AFTER the body that calls it: without a declaration the
             * generated file does not compile at all. */
            const char *decl = strstr(src, "void psp_at_08804008(void);");
            const char *call = strstr(src, "psp_at_08804008();");
            const char *first_body = strstr(src, "static void psp_body_");
            CHECK(decl != NULL, "the thunk is declared");
            CHECK(decl && call && decl < call,
                  "the declaration precedes the call that needs it");
            CHECK(!first_body || (decl && decl < first_body),
                  "the declarations precede the bodies");
            free(src);
        }
    }

    /* Case two: the address after the body is a function entry. Unchanged, and
     * locked here so the new case cannot swallow this one. */
    {
        uint32_t owners[3] = { BASE, BASE, BASE + 0x08u };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x0Cu;  funcs[1].insns = 1;
        funcs[1].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 3, funcs, 2, FALL_OUT_FUNC);
        char *src = emit_case(&an, "t_fallout_func");
        CHECK(src != NULL, "case two: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls through into the next function */",
                            "a fall-out into a function entry still says so");
            expect_contains(src, "psp_func_08804008();",
                            "and calls that function, as before");
            CHECK(strstr(src, "psp_at_08804008") == NULL,
                  "an entry is called directly, not through a label thunk");
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

    check_fall_out();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all emitter checks passed (synthetic code, no game data)\n");
    return 0;
}
