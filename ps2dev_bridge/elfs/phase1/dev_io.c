/**
 * dev_io.c
 *
 * Routes dev output through the stdout ring so it reaches the PC terminal.
 * Falls back to ps2sdk printf if the ring isn't available (standalone run).
 */

#include "dev_io.h"

#include <stdio.h>
#include <string.h>

/* stdout_ring_write is exported by the stub — weak ref so the ELF can
 * also be run standalone without the stub (output goes to TTY/nowhere). */
extern void stdout_ring_write(const void *buf, u32 len) __attribute__((weak));

static char _linebuf[256];

/* ── Init ────────────────────────────────────────────────────────────────── */

void dev_io_init(void) {
    DEV_INF("dev_io ready  (ring=%s)",
            stdout_ring_write ? "yes" : "no, stdio only");
}

/* ── Internal emit ───────────────────────────────────────────────────────── */

static void emit(const char *s, int len) {
    if (stdout_ring_write) {
        stdout_ring_write(s, (u32)len);
    } else {
        /* Fallback — visible on a serial console or debug TTY */
        fwrite(s, 1, len, stdout);
        fflush(stdout);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void dev_print(const char *tag, const char *msg) {
    int n = snprintf(_linebuf, sizeof(_linebuf), "%s %s\n", tag, msg);
    if (n > 0) emit(_linebuf, n);
}

void dev_printf(const char *tag, const char *fmt, ...) {
    char body[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    int n = snprintf(_linebuf, sizeof(_linebuf), "%s %s\n", tag, body);
    if (n > 0) emit(_linebuf, n);
}

void dev_hexdump(u32 addr, const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 off = 0;

    while (off < len) {
        /* Address */
        int n = snprintf(_linebuf, sizeof(_linebuf),
                         "[HEX] %08lX  ", (unsigned long)(addr + off));

        /* Up to 16 hex bytes */
        u32 row = len - off;
        if (row > 16) row = 16;

        for (u32 i = 0; i < 16; i++) {
            if (i < row)
                n += snprintf(_linebuf + n, sizeof(_linebuf) - n,
                              "%02X ", p[off + i]);
            else
                n += snprintf(_linebuf + n, sizeof(_linebuf) - n, "   ");
            if (i == 7)
                n += snprintf(_linebuf + n, sizeof(_linebuf) - n, " ");
        }

        /* ASCII */
        n += snprintf(_linebuf + n, sizeof(_linebuf) - n, " |");
        for (u32 i = 0; i < row; i++) {
            u8 c = p[off + i];
            _linebuf[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        _linebuf[n++] = '|';
        _linebuf[n++] = '\n';

        emit(_linebuf, n);
        off += row;
    }
}
