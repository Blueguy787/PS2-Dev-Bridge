/**
 * ioppeek.c — Phase 2b
 *
 * Reads IOP-side memory through the EE→IOP SIF bridge and streams
 * it back as a hex dump via the stdout ring.
 *
 * The IOP has 2MB of RAM at IOP physical 0x00000000. The EE can
 * access it via the SIF interface mapped at EE virtual 0xBC000000
 * (uncached KSEG2 window into IOP bus). No special IOP-side code
 * needed — this is a one-way read from the EE side.
 *
 * Useful for:
 *   - Inspecting loaded IOP modules in memory
 *   - Reading IOP-side data structures (SPU2, USB, pad driver state)
 *   - Verifying IRX load addresses
 *
 * IOP RAM EE-side window: 0xBC000000 + iop_offset
 * IOP RAM size: 2MB (0x200000)
 *
 * Push with:
 *   python sd2psx_cli.py elf ioppeek.elf
 *
 * Expected output:
 *   [INF] IOP RAM peek  iop_addr=0x00000000  len=256
 *   [HEX] 00000000  XX XX XX XX ...
 *   ...
 *   [INF] done
 *
 * Notable IOP addresses:
 *   0x00000000  IOP reset vector / exception table
 *   0x00012C00  Start of loadable IOP RAM (approx, varies)
 *   0x000A8000  SYSMEM module (typical position)
 *   0x001F0000  Top of usable IOP RAM
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <string.h>

#include "../phase1/dev_io.h"

/* EE-side window into IOP address space */
#define IOP_RAM_BASE_EE     0xBC000000u
#define IOP_RAM_SIZE        0x00200000u   /* 2MB */

/* Default peek: first 256 bytes (IOP exception/reset vectors) */
#ifndef PEEK_IOP_ADDR
#define PEEK_IOP_ADDR  0x00000000u
#endif

#ifndef PEEK_LEN
#define PEEK_LEN       256u
#endif

#define CHUNK_SIZE  128u

static inline volatile u8 *iop_ptr(u32 iop_addr) {
    return (volatile u8 *)(IOP_RAM_BASE_EE + iop_addr);
}

/* Copy from IOP window into a local buffer using byte reads.
 * The SIF window is uncached — word-aligned reads are safer but
 * byte reads work fine for inspection. */
static void iop_read(u32 iop_addr, u8 *dst, u32 len) {
    volatile u8 *src = iop_ptr(iop_addr);
    for (u32 i = 0; i < len; i++)
        dst[i] = src[i];
}

static u8 _ibuf[CHUNK_SIZE] __attribute__((aligned(16)));

int main(void) {
    dev_io_init();

    /* SIF must be initialised to use the IOP window reliably */
    SifInitRpc(0);

    u32 iop_addr = PEEK_IOP_ADDR;
    u32 len      = PEEK_LEN;

    if (iop_addr + len > IOP_RAM_SIZE) {
        DEV_ERR("range 0x%08lX+%lu exceeds IOP RAM (2MB)",
                (unsigned long)iop_addr, (unsigned long)len);
        return -1;
    }

    DEV_INF("IOP RAM peek  iop_addr=0x%08lX  len=%lu",
            (unsigned long)iop_addr, (unsigned long)len);
    DEV_INF("EE window: 0x%08lX",
            (unsigned long)(IOP_RAM_BASE_EE + iop_addr));

    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        iop_read(iop_addr + done, _ibuf, chunk);
        DEV_HEX(iop_addr + done, _ibuf, chunk);
        done += chunk;

        for (volatile int d = 0; d < 4500000; d++) {}
    }

    DEV_INF("done  %lu bytes read from IOP", (unsigned long)len);
    return 0;
}
