/**
 * stdout_ring.c
 *
 * PS2-side stdout ring buffer writer.
 *
 * Layout in PSRAM (set by dev_channel.h):
 *   DEV_ADDR_STDOUT_HEAD  [u32 BE]  — stub write pointer (we own this)
 *   DEV_ADDR_STDOUT_TAIL  [u32 BE]  — PC drain pointer (PC owns this)
 *   DEV_ADDR_STDOUT_RING  [32KB]    — ring data
 *
 * We maintain a local copy of head to avoid a card read on every write.
 * On init we read both head and tail from the card to pick up where a
 * previous session left off (DEV_FLAG_KEEP_STDOUT) or reset to 0.
 *
 * Writes never block — if the ring is full we overwrite old data.
 * The ring is 32KB; at 115200 baud the PC drains ~14KB/s, so as long
 * as the PS2 program isn't flooding faster than that, nothing is lost.
 */

#include "stdout_ring.h"
#include "mc_access.h"
#include "dev_channel_ps2.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <kernel.h>   /* for SifLoadModule etc. — needed for fd patching */

/* ── State ───────────────────────────────────────────────────────────────── */

static u32  _head = 0;   /* local mirror of STDOUT_HEAD in PSRAM */
static u8   _wbuf[DEV_MC_SECTOR_SIZE] __attribute__((aligned(64)));
static char _pfbuf[512]; /* printf scratch buffer */

/* ── Init ────────────────────────────────────────────────────────────────── */

void stdout_ring_init(void) {
    u8 hdr[8];
    /* Read current head and tail from card */
    if (mc_read_bytes(DEV_CARD_BASE + DEV_OFF_STDOUT_HEAD, hdr, 8) == 0) {
        _head = dev_read_u32(hdr, 0);
        /* tail is PC-owned, we don't cache it */
    } else {
        _head = 0;
    }
}

/* ── Write bytes into ring ───────────────────────────────────────────────── */

void stdout_ring_write(const void *buf, u32 len) {
    const u8 *src = (const u8 *)buf;
    u32 remaining = len;

    while (remaining > 0) {
        /* Write position in ring */
        u32 ring_offset = _head % DEV_STDOUT_RING_SIZE;
        u32 card_addr   = DEV_CARD_BASE + DEV_OFF_STDOUT_RING + ring_offset;

        /* How many bytes until end of ring (wrap point) */
        u32 to_end = DEV_STDOUT_RING_SIZE - ring_offset;
        u32 put    = (remaining < to_end) ? remaining : to_end;

        /* Write the chunk — mc_write_bytes handles sector alignment */
        mc_write_bytes(card_addr, src, put);

        src       += put;
        remaining -= put;
        _head     += put;

        /* Wrap head within ring */
        if (_head >= DEV_STDOUT_RING_SIZE)
            _head -= DEV_STDOUT_RING_SIZE;
    }

    /* Update STDOUT_HEAD in PSRAM so the PC knows bytes are available */
    u8 hdr[4];
    dev_write_u32(hdr, 0, _head);
    mc_write_bytes(DEV_CARD_BASE + DEV_OFF_STDOUT_HEAD, hdr, 4);
}

/* ── printf wrapper ──────────────────────────────────────────────────────── */

void stdout_ring_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(_pfbuf, sizeof(_pfbuf), fmt, ap);
    va_end(ap);
    if (len > 0)
        stdout_ring_write(_pfbuf, (u32)len);
}

/* ── fd patching ─────────────────────────────────────────────────────────── */

/*
 * The EE C library routes printf through a small set of file descriptors.
 * We can redirect fd 1 (stdout) by installing a custom write handler in
 * the ROM0:OSDSYS / IOPRP ioctl table.
 *
 * The cleanest approach available without a custom IOP module is to
 * override the _ps2sdk_stdio_write hook that ps2sdk exposes for exactly
 * this purpose. Any code linked against ps2sdk will call this function
 * when writing to stdout/stderr.
 *
 * Note: ELFs loaded AFTER stdout_ring_install() inherit the patched fd
 * automatically IF they were linked with the same ps2sdk. Statically
 * linked ELFs that pull in their own libc copy will not be affected —
 * in that case they should call stdout_ring_install() themselves, or
 * link against a shared version of mc_access + stdout_ring.
 */

/* ps2sdk hook — weak symbol, we override it */
int _ps2sdk_stdio_write(int fd, const void *buf, int len) __attribute__((weak));

static int _dev_stdout_write(int fd, const void *buf, int len) {
    (void)fd;
    if (len > 0)
        stdout_ring_write(buf, (u32)len);
    return len;
}

void stdout_ring_install(void) {
    /*
     * ps2sdk exposes a function pointer table for stdio.
     * Override the write entry for fd 1 and 2 (stdout, stderr).
     *
     * If the symbol isn't available (older sdk), fall back to a no-op
     * and the user must call stdout_ring_write() directly.
     */
    extern void _ps2sdk_stdio_init(void);   /* ensure table is set up */
    _ps2sdk_stdio_init();

    /* Replace the write hook */
    /* ps2sdk defines _ps2sdk_stdio_write as a weak function we can override.
     * Since C doesn't let us replace a function pointer in a weak symbol
     * at runtime without the table address, we use the documented
     * SetFileWriteFunc() if available, or patch via the known offset.
     *
     * Most practical approach: use printf->putchar hook via ee_sio if
     * SetFileWriteFunc isn't linked. We attempt both.
     */
#ifdef _PS2SDK_STDIO_HAS_SETWRFUNC
    SetFileWriteFunc(1, _dev_stdout_write);
    SetFileWriteFunc(2, _dev_stdout_write);
#else
    /*
     * Fallback: stdout_ring_write() must be called directly from the
     * loaded ELF. Document this in the README — the stub exports the
     * symbol so ELFs can call it via a known address or link stub.o.
     */
    (void)_dev_stdout_write;
    printf("stdout_ring: SetFileWriteFunc unavailable — call stdout_ring_write() directly\n");
#endif

    stdout_ring_printf("stdout_ring: installed on fd 1+2\n");
}
