/* psprecomp — Allegrex CPU state. See include/psprecomp/cpu.h. */

#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

psp_cpu_state psp_cpu;

/* The frame stack of the first thread (and of every thread when they all run
 * on one host stack); other host threads get theirs in psp_cpu_frames_attach. */
static psp_frame g_boot_frames[PSP_FRAME_STACK];

#if defined(_MSC_VER)
#  define CPU_TLS __declspec(thread)
#else
#  define CPU_TLS _Thread_local
#endif

void psp_cpu_frames_attach(void) {
    static CPU_TLS psp_frame *mine;
    if (!mine) mine = (psp_frame *)malloc(sizeof(psp_frame) * PSP_FRAME_STACK);
    psp_frame *base = mine ? mine : g_boot_frames;
    psp_cpu.frame = NULL;
    psp_cpu.fsp = base;
    psp_cpu.fend = base + PSP_FRAME_STACK;
}

psp_frame *psp_frame_overflow(void) {
    fprintf(stderr, "psprecomp: guest calls nested deeper than %d\n", PSP_FRAME_STACK);
    fflush(stderr);
    abort();
}

const char *const psp_reg_names[PSP_NUM_GPR] = {
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
};

void psp_cpu_reset(void) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu.fsp = g_boot_frames;
    psp_cpu.fend = g_boot_frames + PSP_FRAME_STACK;
    /* $sp is set by the loader from the module's stack allocation, not here —
     * a reset CPU with a null stack is the correct starting point, and a game
     * that faults on a null $sp is telling us the loader did not run. */
}
