/**
 * ramdump.c — Phase 1b
 *
 * Reads a range of EE RAM and streams it back as a hex dump
 * through the stdout ring to the PC terminal.
 *
 * Proves:
 *   - Arbitrary EE RAM is readable from an injected ELF
 *   - hex dump framing works end to end
 *   - Large stdout ring transfers drain cleanly
 *
 * Default range: 0x00100000 .. 0x00100000 + 512 bytes (ourselves)
 * Override at build time:
 *   make DUMP_ADDR=0x00080000 DUMP_LEN=0x1000
 *
 * Push with:
 *   python sd2psx_cli.py elf ramdump.elf
 *
 * Expected PC output:
 *   [INF] dev_io ready  (ring=yes)
 *   [INF] EE RAM dump  addr=0x00100000  len=512
 *   [HEX] 00100000  7F 45 4C 46 ...  |.ELF...|
 *   [HEX] 00100010  ...
 *   ...
 *   [INF] dump complete  512 bytes
 *
 * Capture to file on PC:
 *   python sd2psx_cli.py stdout > dump.txt
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include "dev_io.h"

#ifndef DUMP_ADDR
#define DUMP_ADDR  0x00100000u   /* default: dump ourselves */
#endif

#ifndef DUMP_LEN
#define DUMP_LEN   512u
#endif

/* Chunk size for streaming — keeps stdout ring from filling up
 * faster than the PC can drain it. One chunk = one ring write burst. */
#define CHUNK_SIZE  256u

int main(void) {
    dev_io_init();

    u32 addr = DUMP_ADDR;
    u32 len  = DUMP_LEN;

    DEV_INF("EE RAM dump  addr=0x%08lX  len=%lu",
            (unsigned long)addr, (unsigned long)len);

    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        DEV_HEX(addr + done, (void *)(addr + done), chunk);
        done += chunk;

        /* Brief pause between chunks — gives PC drain loop time to catch up.
         * At 115200 baud CDC, ~14KB/s. Each 256-byte chunk hex-expands to
         * ~640 bytes of text. Pause ~50ms between chunks. */
        for (volatile int d = 0; d < 4500000; d++) {}
    }

    DEV_INF("dump complete  %lu bytes", (unsigned long)len);
    return 0;
}
