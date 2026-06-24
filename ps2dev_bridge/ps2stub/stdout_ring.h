#ifndef STDOUT_RING_H
#define STDOUT_RING_H

/**
 * stdout_ring.h
 *
 * PS2-side stdout ring buffer writer.
 *
 * Writes bytes into the dev channel stdout ring in PSRAM via MC writes.
 * The RP2040 firmware exposes the ring to the PC via STDOUT_PULL commands.
 *
 * Usage:
 *   stdout_ring_init();          // once, after mc_access_init()
 *   stdout_ring_write(buf, len); // write bytes into the ring
 *   stdout_ring_printf(fmt, ...) // printf into the ring
 *
 * The ring is DEV_STDOUT_RING_SIZE bytes. If the PC isn't draining fast
 * enough, writes wrap and overwrite old data — fire and forget semantics,
 * same as a UART.
 *
 * stdout_ring_install() redirects the C library's stdout to this ring
 * so that printf() from loaded ELFs automatically goes to the PC.
 */

#include <tamtypes.h>

void stdout_ring_init(void);
void stdout_ring_write(const void *buf, u32 len);
void stdout_ring_printf(const char *fmt, ...);

/**
 * stdout_ring_install()
 * Patches the EE's stdout file descriptor to write to the ring.
 * After this call, printf() in any subsequently-loaded ELF will
 * automatically appear in the PC terminal via stream_stdout().
 */
void stdout_ring_install(void);

#endif /* STDOUT_RING_H */
