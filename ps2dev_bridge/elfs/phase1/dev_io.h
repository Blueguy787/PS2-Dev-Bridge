#ifndef DEV_IO_H
#define DEV_IO_H

/**
 * dev_io.h
 *
 * Unified output layer for dev ELFs.
 *
 * All test and tool ELFs use dev_print / dev_printf / dev_hexdump
 * instead of calling printf() directly. This lets us route output
 * through the stdout ring regardless of whether the sdk stdio hook
 * installed cleanly, and gives us a consistent framing format the
 * PC side can parse if needed.
 *
 * Output format on the PC terminal:
 *   [TAG] message\n
 *
 * Tags:
 *   [OK]   normal output
 *   [ERR]  error
 *   [HEX]  hex dump line
 *   [REG]  register dump line
 *   [INF]  info / metadata
 */

#include <tamtypes.h>
#include <stdarg.h>

void dev_io_init(void);

void dev_print  (const char *tag, const char *msg);
void dev_printf (const char *tag, const char *fmt, ...);
void dev_hexdump(u32 addr, const void *data, u32 len);

/* Shorthand macros */
#define DEV_OK(fmt, ...)   dev_printf("[OK] ",  fmt, ##__VA_ARGS__)
#define DEV_ERR(fmt, ...)  dev_printf("[ERR]",  fmt, ##__VA_ARGS__)
#define DEV_INF(fmt, ...)  dev_printf("[INF]",  fmt, ##__VA_ARGS__)
#define DEV_REG(fmt, ...)  dev_printf("[REG]",  fmt, ##__VA_ARGS__)
#define DEV_HEX(addr, buf, len) dev_hexdump(addr, buf, len)

#endif /* DEV_IO_H */
