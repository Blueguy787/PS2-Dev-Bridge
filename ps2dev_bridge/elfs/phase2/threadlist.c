/**
 * threadlist.c — Phase 2c
 *
 * Walks the EE kernel thread table and dumps all threads with:
 *   - Thread ID
 *   - Status (RUN / READY / WAIT / SUSPEND / DORMANT / DEAD)
 *   - Current priority
 *   - PC (program counter at last context switch)
 *   - SP (stack pointer at last context switch)
 *   - Stack base and size
 *   - GP
 *
 * The kernel thread table lives at a fixed address in the EE kernel
 * (loaded into the scratchpad / kernel segment). We use the ps2sdk
 * ReferThreadStatus() syscall to query each thread by ID rather than
 * walking raw kernel memory, which is safer across BIOS versions.
 *
 * We probe thread IDs 0..MAX_THREADS and skip ones that return error.
 *
 * Push with:
 *   python sd2psx_cli.py elf threadlist.elf
 *
 * Expected output:
 *   [INF] ======== EE Thread List ========
 *   [INF] TID  Status    Pri  PC          SP          StackBase   StackSz
 *   [REG] 0001 RUN        16  0x001004A0  0x01FFFF00  0x01F00000  0x100000
 *   [REG] 0002 DORMANT    32  0x00100120  0x01500000  0x01500000  0x080000
 *   ...
 *   [INF] total: 3 threads
 *   [INF] ======== done ========
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include "../phase1/dev_io.h"

#define MAX_THREAD_ID   256

/* Thread status codes from ps2sdk kernel.h */
static const char *thread_status_str(int status) {
    switch (status) {
        case THS_RUN:     return "RUN    ";
        case THS_READY:   return "READY  ";
        case THS_WAIT:    return "WAIT   ";
        case THS_SUSPEND: return "SUSPEND";
        case THS_WAITSUS: return "WAITSUS";
        case THS_DORMANT: return "DORMANT";
        default:          return "UNKNOWN";
    }
}

int main(void) {
    dev_io_init();

    DEV_INF("======== EE Thread List ========");
    DEV_INF("TID   Status    Pri  PC          SP          StackBase   StackSz");

    int found = 0;

    ee_thread_status_t st;

    for (int tid = 0; tid <= MAX_THREAD_ID; tid++) {
        memset(&st, 0, sizeof(st));
        int r = ReferThreadStatus(tid, &st);
        if (r < 0)
            continue;   /* thread doesn't exist */

        found++;

        DEV_REG("%04d  %s  %3d  0x%08lX  0x%08lX  0x%08lX  0x%06lX",
                tid,
                thread_status_str(st.status),
                st.current_priority,
                (unsigned long)st.pc,
                (unsigned long)st.sp,
                (unsigned long)st.stack,
                (unsigned long)st.stack_size);

        /* Brief pause to avoid flooding stdout ring */
        for (volatile int d = 0; d < 1000000; d++) {}
    }

    DEV_INF("total: %d thread(s)", found);
    DEV_INF("======== done ========");
    return 0;
}
