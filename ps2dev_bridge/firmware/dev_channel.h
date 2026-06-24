#pragma once

/**
 * dev_channel.h
 *
 * PS2 dev channel — reserved region at the top of the 8MB PSRAM.
 *
 * Layout (all offsets from PS2_CARD_SIZE - DEV_CHANNEL_SIZE):
 *
 *   +0x0000  [4B]  DOORBELL_MAGIC   — written by PC when ELF is ready
 *   +0x0004  [4B]  ELF_SIZE         — size of ELF payload in bytes
 *   +0x0008  [4B]  ELF_LOAD_ADDR    — EE RAM target address (default 0x00100000)
 *   +0x000C  [4B]  FLAGS            — DEV_FLAG_* bitmask
 *   +0x0010  [4B]  ACK              — written by PS2 stub when it has claimed the ELF
 *   +0x0014  [4B]  STDOUT_HEAD      — stub write pointer into stdout ring
 *   +0x0018  [4B]  STDOUT_TAIL      — PC drain pointer into stdout ring
 *   +0x001C  [4B]  reserved
 *   +0x0020  [NB]  ELF_DATA         — ELF binary (up to DEV_ELF_MAX_SIZE bytes)
 *   +0x8020  [NB]  STDOUT_RING      — stdout ring buffer (DEV_STDOUT_RING_SIZE bytes)
 *
 * The PS2 stub polls DOORBELL at the card sector corresponding to
 * DEV_CHANNEL_BASE. When it sees DEV_DOORBELL_MAGIC it copies ELF_DATA
 * into EE RAM at ELF_LOAD_ADDR, writes ACK = DEV_ACK_MAGIC, then jumps.
 *
 * The RP2040 firmware never marks DEV_CHANNEL sectors dirty, so they are
 * never flushed to SD and never corrupt the card image the PS2 sees as
 * its memory card.
 *
 * Card address space:
 *   0x000000 .. DEV_CHANNEL_BASE-1   normal PS2 memory card data
 *   DEV_CHANNEL_BASE .. 0x7FFFFF     dev channel (invisible to card manager)
 */

#include <stdint.h>
#include <stdbool.h>

/* ── Region geometry ────────────────────────────────────────────────────── */

#define PS2_CARD_SIZE           (8 * 1024 * 1024)   /* 8 MB                */
#define DEV_CHANNEL_SIZE        (512 * 1024)         /* 512 KB reserved     */
#define DEV_CHANNEL_BASE        (PS2_CARD_SIZE - DEV_CHANNEL_SIZE)

/* ── Header field offsets (relative to DEV_CHANNEL_BASE) ───────────────── */

#define DEV_OFF_DOORBELL        0x0000
#define DEV_OFF_ELF_SIZE        0x0004
#define DEV_OFF_ELF_LOAD_ADDR   0x0008
#define DEV_OFF_FLAGS           0x000C
#define DEV_OFF_ACK             0x0010
#define DEV_OFF_STDOUT_HEAD     0x0014
#define DEV_OFF_STDOUT_TAIL     0x0018
/* 0x001C reserved */
#define DEV_OFF_ELF_DATA        0x0020

/* ── Derived absolute PSRAM addresses ──────────────────────────────────── */

#define DEV_ADDR_DOORBELL       (DEV_CHANNEL_BASE + DEV_OFF_DOORBELL)
#define DEV_ADDR_ELF_SIZE       (DEV_CHANNEL_BASE + DEV_OFF_ELF_SIZE)
#define DEV_ADDR_ELF_LOAD_ADDR  (DEV_CHANNEL_BASE + DEV_OFF_ELF_LOAD_ADDR)
#define DEV_ADDR_FLAGS          (DEV_CHANNEL_BASE + DEV_OFF_FLAGS)
#define DEV_ADDR_ACK            (DEV_CHANNEL_BASE + DEV_OFF_ACK)
#define DEV_ADDR_STDOUT_HEAD    (DEV_CHANNEL_BASE + DEV_OFF_STDOUT_HEAD)
#define DEV_ADDR_STDOUT_TAIL    (DEV_CHANNEL_BASE + DEV_OFF_STDOUT_TAIL)
#define DEV_ADDR_ELF_DATA       (DEV_CHANNEL_BASE + DEV_OFF_ELF_DATA)

/* ── ELF staging ────────────────────────────────────────────────────────── */

/*
 * Max ELF size = DEV_CHANNEL_SIZE - header (0x0020) - stdout ring
 * Leaves plenty of room for typical PS2 homebrew ELFs.
 */
#define DEV_STDOUT_RING_SIZE    (32 * 1024)          /* 32 KB stdout ring   */
#define DEV_ELF_MAX_SIZE        (DEV_CHANNEL_SIZE - DEV_OFF_ELF_DATA - DEV_STDOUT_RING_SIZE)

#define DEV_ADDR_STDOUT_RING    (DEV_ADDR_ELF_DATA + DEV_ELF_MAX_SIZE)

/* Default EE RAM load address — above the kernel, below the heap */
#define DEV_DEFAULT_LOAD_ADDR   0x00100000u

/* ── Magic values ───────────────────────────────────────────────────────── */

#define DEV_DOORBELL_MAGIC      0x50533244u   /* 'PS2D' */
#define DEV_ACK_MAGIC           0x41434B21u   /* 'ACK!' */
#define DEV_DOORBELL_CLEAR      0x00000000u

/* ── Flags ──────────────────────────────────────────────────────────────── */

#define DEV_FLAG_RESET_ON_LOAD  (1u << 0)    /* stub resets EE before jump */
#define DEV_FLAG_KEEP_STDOUT    (1u << 1)    /* don't clear ring on new ELF */

/* ── Guard macro ────────────────────────────────────────────────────────── */

/**
 * DEV_CHANNEL_ADDR(addr, len)
 * Returns true if [addr, addr+len) overlaps the dev channel region.
 * Use in usb_bridge.c and ps2_dirty.c to gate dirty marking and SD flushes.
 */
static inline bool dev_channel_overlaps(uint32_t addr, uint32_t len) {
    return (addr + len) > DEV_CHANNEL_BASE;
}

/**
 * dev_channel_clamp_len(addr, len)
 * Clamp len so that [addr, addr+len) does not enter the dev channel.
 * Returns the safe length to use. If addr is already in the dev channel,
 * returns 0.
 */
static inline uint32_t dev_channel_clamp_len(uint32_t addr, uint32_t len) {
    if (addr >= DEV_CHANNEL_BASE)
        return 0;
    if (addr + len > DEV_CHANNEL_BASE)
        return DEV_CHANNEL_BASE - addr;
    return len;
}
