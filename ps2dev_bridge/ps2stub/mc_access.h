#ifndef MC_ACCESS_H
#define MC_ACCESS_H

/**
 * mc_access.h
 *
 * Thin sector-aligned read/write wrappers around the ps2sdk MC driver.
 *
 * The dev channel lives at fixed card byte offsets. These functions let
 * the stub read and write arbitrary byte ranges by rounding to 512-byte
 * sector boundaries and issuing the minimum number of MC transactions.
 *
 * All functions block until the operation completes.
 * Returns 0 on success, -1 on error.
 */

#include <tamtypes.h>

/**
 * mc_access_init()
 * Call once before any mc_read_bytes / mc_write_bytes calls.
 * Initialises the MC library and waits for card enumeration.
 * Returns 0 on success, -1 if no card found within timeout.
 */
int mc_access_init(void);

/**
 * mc_read_bytes(card_byte_addr, buf, len)
 * Read `len` bytes from absolute card byte address `card_byte_addr`.
 * Handles sector alignment and partial sector reads internally.
 * `buf` must be at least `len` bytes.
 */
int mc_read_bytes(u32 card_byte_addr, void *buf, u32 len);

/**
 * mc_write_bytes(card_byte_addr, buf, len)
 * Write `len` bytes to absolute card byte address `card_byte_addr`.
 * Handles sector alignment and read-modify-write for partial sectors.
 * `buf` must be at least `len` bytes.
 */
int mc_write_bytes(u32 card_byte_addr, const void *buf, u32 len);

/**
 * mc_read_sector(sector, buf)
 * Read a single 512-byte sector. `buf` must be >= 512 bytes.
 */
int mc_read_sector(u32 sector, void *buf);

/**
 * mc_write_sector(sector, buf)
 * Write a single 512-byte sector. `buf` must be >= 512 bytes.
 */
int mc_write_sector(u32 sector, const void *buf);

#endif /* MC_ACCESS_H */
