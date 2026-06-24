/**
 * hello.c — Phase 1a
 *
 * Simplest possible dev ELF. Proves:
 *   - ELF loaded correctly into EE RAM
 *   - Entry point reached
 *   - stdout ring write works
 *   - PC terminal receives output
 *
 * Push with:
 *   python sd2psx_cli.py elf hello.elf
 *
 * Expected PC output:
 *   [INF] dev_io ready  (ring=yes)
 *   [OK]  ============ PS2 Dev Hello ============
 *   [OK]  ELF loaded and running on EE processor
 *   [OK]  Load addr : 0x00100000
 *   [OK]  GP        : 0xXXXXXXXX
 *   [OK]  SP        : 0xXXXXXXXX
 *   [OK]  Tick 0
 *   [OK]  Tick 1
 *   ...
 *   [OK]  Tick 9
 *   [OK]  ============ done =====================
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include "dev_io.h"

int main(void) {
    dev_io_init();

    DEV_OK("============ PS2 Dev Hello ============");
    DEV_OK("ELF loaded and running on EE processor");

    /* Print our own addresses so we can verify load address on PC side */
    u32 sp, gp;
    asm volatile ("move %0, $sp" : "=r"(sp));
    asm volatile ("move %0, $gp" : "=r"(gp));

    DEV_OK("Load addr : 0x00100000");
    DEV_OK("GP        : 0x%08lX", (unsigned long)gp);
    DEV_OK("SP        : 0x%08lX", (unsigned long)sp);

    /* Tick loop — 10 iterations, ~100ms each via busy spin */
    for (int i = 0; i < 10; i++) {
        DEV_OK("Tick %d", i);
        /* ~100ms at 294MHz EE clock */
        for (volatile int d = 0; d < 9000000; d++) {}
    }

    DEV_OK("============ done =====================");
    return 0;
}
