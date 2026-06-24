#ifndef DEV_CHANNEL_PS2_H
#define DEV_CHANNEL_PS2_H

/**
 * dev_channel_ps2.h
 *
 * PS2 EE-side view of the SD2PSX dev channel.
 *
 * Must match dev_channel.h on the RP2040 side exactly.
 *
 * The PS2 stub accesses this region via normal memory card reads/writes
 * through the SIO2 / mc driver. The RP2040 exposes DEV_CHANNEL_BASE..
 * PS2_CARD_SIZE-1 as a transparent PSRAM window — the PS2 reads and writes
 * these addresses like any other card sector, but the firmware never flushes
 * them to SD and never marks them dirty.
 *
 * All offsets are relative to the start of the dev channel region.
 * Card byte address = DEV_CARD_BASE + offset.
 */

#include <tamtypes.h>

/* ── Region geometry ────────────────────────────────────────────────────── */

#define PS2_CARD_SIZE           (8 * 1024 * 1024)
#define DEV_CHANNEL_SIZE        (512 * 1024)
#define DEV_CARD_BASE           (PS2_CARD_SIZE - DEV_CHANNEL_SIZE)

/* Sector size the PS2 MC driver uses */
#define DEV_MC_SECTOR_SIZE      512

/* ── Header field offsets (bytes from DEV_CARD_BASE) ────────────────────── */

#define DEV_OFF_DOORBELL        0x0000
#define DEV_OFF_ELF_SIZE        0x0004
#define DEV_OFF_ELF_LOAD_ADDR   0x0008
#define DEV_OFF_FLAGS           0x000C
#define DEV_OFF_ACK             0x0010
#define DEV_OFF_STDOUT_HEAD     0x0014
#define DEV_OFF_STDOUT_TAIL     0x0018
/* 0x001C reserved */
#define DEV_OFF_ELF_DATA        0x0020

/* ── Sizes ──────────────────────────────────────────────────────────────── */

#define DEV_STDOUT_RING_SIZE    (32 * 1024)
#define DEV_ELF_MAX_SIZE        (DEV_CHANNEL_SIZE - DEV_OFF_ELF_DATA - DEV_STDOUT_RING_SIZE)
#define DEV_OFF_STDOUT_RING     (DEV_OFF_ELF_DATA + DEV_ELF_MAX_SIZE)

/* ── Magic values ───────────────────────────────────────────────────────── */

#define DEV_DOORBELL_MAGIC      0x50533244u   /* 'PS2D' */
#define DEV_ACK_MAGIC           0x41434B21u   /* 'ACK!' */
#define DEV_DOORBELL_CLEAR      0x00000000u

/* ── Flags ──────────────────────────────────────────────────────────────── */

#define DEV_FLAG_RESET_ON_LOAD  (1u << 0)
#define DEV_FLAG_KEEP_STDOUT    (1u << 1)

/* ── Sector numbers (for mc read/write calls) ───────────────────────────── */

/*
 * The PS2 MC driver addresses data in 512-byte sectors from the start
 * of the card. DEV_CARD_BASE is always a multiple of 512 (it's 7.5 MB).
 */
#define DEV_SECTOR_BASE         (DEV_CARD_BASE / DEV_MC_SECTOR_SIZE)

/* Sector that contains the doorbell (first sector of dev channel) */
#define DEV_DOORBELL_SECTOR     DEV_SECTOR_BASE

/* Number of sectors the header occupies (rounds up to cover DEV_OFF_ELF_DATA) */
#define DEV_HEADER_SECTORS      1   /* header fits in first 512-byte sector */

/* ── Inline helpers ─────────────────────────────────────────────────────── */

/* Read a big-endian u32 from a raw byte buffer at a given byte offset */
static inline u32 dev_read_u32(const u8 *buf, u32 off) {
    return ((u32)buf[off]     << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] <<  8)
         |  (u32)buf[off + 3];
}

/* Write a big-endian u32 into a raw byte buffer at a given byte offset */
static inline void dev_write_u32(u8 *buf, u32 off, u32 val) {
    buf[off]     = (val >> 24) & 0xFF;
    buf[off + 1] = (val >> 16) & 0xFF;
    buf[off + 2] = (val >>  8) & 0xFF;
    buf[off + 3] =  val        & 0xFF;
}

#endif /* DEV_CHANNEL_PS2_H */
