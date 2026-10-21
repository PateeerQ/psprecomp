/* Synthetic lifecycle regressions; no game data or host scheduling sleeps. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "crypto/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(1); \
} } while (0)

#define ENTRY 0x08801000u
#define OUT   0x08809000u
static unsigned stage, checkpoint, deliveries;
#if PSPRECOMP_REAL_THREADS
static unsigned finish;
#endif
static uint32_t handler_created;

static uint32_t call(const char *name, uint32_t a, uint32_t b,
                     uint32_t c, uint32_t d) {
    psp_cpu.r[PSP_REG_A0] = a;
    psp_cpu.r[PSP_REG_A1] = b;
    psp_cpu.r[PSP_REG_A2] = c;
    psp_cpu.r[PSP_REG_A3] = d;
    psp_hle_call(psp_nid(name));
    return psp_cpu.r[PSP_REG_V0];
}

static uint32_t create(void) {
    psp_write32(psp_cpu.r[PSP_REG_SP] + 16, 0);
    psp_write32(psp_cpu.r[PSP_REG_SP] + 20, 0);
    uint32_t id = call("sceKernelCreateThread", 0, ENTRY, 32, 0x4000);
    REQUIRE((int32_t)id > 0);
    return id;
}

static void normal_entry(void) { stage++; psp_cpu.r[PSP_REG_V0] = 0x55; }
static void delete_entry(void) {
    stage++;
    call("sceKernelExitDeleteThread", 0x1234, 0, 0, 0);
    REQUIRE(0); /* ExitDeleteThread must not return. */
}
#if PSPRECOMP_REAL_THREADS
static void worker_entry(void) {
    stage = 1;
    while (!finish) {
        switch (checkpoint) {
        case 0: call("sceKernelSleepThread", 0, 0, 0, 0); break;
        case 1: call("sceKernelDelayThread", 1000, 0, 0, 0); break;
        case 2: psp_threadman_yield(); break;
        case 3: psp_threadman_frame_wait(); break;
        default: call("sceKernelWaitSema", 0xDEADBEEF, 1, 0, 0); break;
        }
    }
    stage = 2;
    psp_cpu.r[PSP_REG_V0] = 0x55;
}
#endif
static void handler(void) { deliveries++; }

/* WaitThreadEnd delivers ticks before testing readiness. Creating from that
 * tick must not reuse an ExitDelete record the waiter has not consumed yet. */
static void reclaim_handler(void) {
    if (stage && !handler_created) handler_created = create();
}

static void test_pinned(void) {
    psp_register(ENTRY, delete_entry);
    psp_register(ENTRY + 4, reclaim_handler);
    REQUIRE(call("sceKernelRegisterSubIntrHandler", 1, 2, ENTRY + 4, 0) == 0);
    call("sceKernelEnableSubIntr", 1, 2, 0, 0);
    uint32_t id = create();
    REQUIRE(call("sceKernelStartThread", id, 0, 0, 0) == 0);
    REQUIRE(call("sceKernelWaitThreadEnd", id, OUT, 0, 0) == 0);
    REQUIRE(psp_read32(OUT) == 0x1234);
    REQUIRE(handler_created != 0 && handler_created != id);
    REQUIRE(call("sceKernelDeleteThread", handler_created, 0, 0, 0) == 0);
    psp_threadman_reset();
}

static void test_delete(void) {
    psp_register(ENTRY, delete_entry);
    uint32_t before = psp_sysmem_free();
    for (unsigned i = 0; i < 512; i++) {
        uint32_t id = create();
        REQUIRE(call("sceKernelStartThread", id, 0, 0, 0) == 0);
        REQUIRE(call("sceKernelWaitThreadEnd", id, OUT, 0, 0) == 0);
        REQUIRE(psp_read32(OUT) == 0x1234);
        REQUIRE(psp_sysmem_free() == before);
        /* No explicit DeleteThread: creation must reap the previous host. */
    }
    REQUIRE(stage == 512);
    psp_threadman_reset(); /* Last ExitDelete host is still joinable. */
}

static void test_terminate(int pending) {
#if PSPRECOMP_REAL_THREADS
    psp_register(ENTRY, worker_entry);
    uint32_t id = create();
    REQUIRE(call("sceKernelStartThread", id, 0, 0, 0) == 0);
    if (!pending) {
        psp_threadman_yield();
        REQUIRE(stage == 1);
    }
    REQUIRE(call("sceKernelTerminateThread", id, 0, 0, 0) == 0);
    REQUIRE(call("sceKernelWaitThreadEnd", id, OUT, 0, 0) == 0);
    REQUIRE(psp_read32(OUT) == SCE_KERNEL_ERROR_THREAD_TERMINATED);
    REQUIRE(stage == (pending ? 0u : 1u));
    finish = 1;
    for (unsigned i = 0; i < 8; i++) psp_threadman_yield();
    REQUIRE(stage == (pending ? 0u : 1u));
    REQUIRE(call("sceKernelGetThreadExitStatus", id, OUT, 0, 0) == 0);
    REQUIRE(psp_read32(OUT) == SCE_KERNEL_ERROR_THREAD_TERMINATED);
    REQUIRE(call("sceKernelDeleteThread", id, 0, 0, 0) == 0);
#else
    /* Inline StartThread cannot be terminated concurrently. Its completed
     * exit code must nevertheless survive a later termination request. */
    (void)pending;
    psp_register(ENTRY, normal_entry);
    uint32_t id = create();
    REQUIRE(call("sceKernelStartThread", id, 0, 0, 0) == 0);
    REQUIRE(call("sceKernelTerminateThread", id, 0, 0, 0) == 0);
    REQUIRE(call("sceKernelWaitThreadEnd", id, OUT, 0, 0) == 0);
    REQUIRE(psp_read32(OUT) == 0x55);
    REQUIRE(call("sceKernelDeleteThread", id, 0, 0, 0) == 0);
#endif
}

static void test_mask(void) {
    psp_register(ENTRY, handler);
    REQUIRE(call("sceKernelRegisterSubIntrHandler", 1, 2, ENTRY, 0) == 0);
    call("sceKernelEnableSubIntr", 1, 2, 0, 0);
    int outer = psp_threadman_intr_suspend();
    int inner = psp_threadman_intr_suspend();
    psp_intr_tick(); /* Direct callers obey masking too. */
    REQUIRE(deliveries == 0);
    call("sceKernelDelayThread", 1, 0, 0, 0);
    REQUIRE(deliveries == 0);
    psp_threadman_intr_resume((uint32_t)inner);
    call("sceKernelSleepThread", 0, 0, 0, 0);
    REQUIRE(deliveries == 0);
    psp_threadman_intr_resume((uint32_t)outer);
    call("sceKernelDelayThread", 1, 0, 0, 0);
    REQUIRE(deliveries == 1);
    REQUIRE(psp_intr_deliveries() == 1);
}

static void test_reset(void) {
    psp_register(ENTRY, normal_entry);
    for (unsigned round = 0; round < 8; round++) {
        for (unsigned i = 0; i < 32; i++) {
            uint32_t id = create();
            REQUIRE(call("sceKernelStartThread", id, 0, 0, 0) == 0);
            REQUIRE(call("sceKernelWaitThreadEnd", id, OUT, 0, 0) == 0);
            REQUIRE(psp_read32(OUT) == 0x55);
            /* Deliberately do NOT delete: reset must join these hosts before
             * destroying their synchronization objects and reusing records. */
        }
        psp_threadman_reset();
        psp_sysmem_reset();
    }
    REQUIRE(stage == 256);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    REQUIRE(psp_mem_init() == 0);
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x08810000;
    psp_hle_init();
    if (!strcmp(argv[1], "delete")) test_delete();
    else if (!strcmp(argv[1], "pinned")) test_pinned();
    else if (!strcmp(argv[1], "pending")) test_terminate(1);
    else if (!strncmp(argv[1], "checkpoint", 10)) {
        checkpoint = (unsigned)atoi(argv[1] + 10);
        test_terminate(0);
    } else if (!strcmp(argv[1], "mask")) test_mask();
    else if (!strcmp(argv[1], "reset")) test_reset();
    else REQUIRE(0);
    printf("thread lifecycle %s: passed (real_threads=%d)\n",
           argv[1], PSPRECOMP_REAL_THREADS);
    return 0;
}
