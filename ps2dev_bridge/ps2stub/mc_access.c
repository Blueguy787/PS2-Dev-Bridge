/**
 * mc_access.c
 *
 * Sector-aligned memory card read/write for the PS2 dev channel stub.
 *
 * The PS2 MC driver (ps2sdk libmc) operates on 512-byte sectors indexed
 * from the start of the card. We expose a byte-addressed API on top of it
 * so the stub can read/write header fields and ring buffer bytes without
 * caring about sector boundaries.
 *
 * Port 0, slot 0 — the SD2PSX is always in port 0.
 *
 * mcRead/mcWrite in ps2sdk use a callback-based async API. We wrap them
 * synchronously here using mcSync().
 */

#include "mc_access.h"
#include "dev_channel_ps2.h"

#include <stdio.h>
#include <string.h>
#include <libmc.h>

#define MC_PORT     0
#define MC_SLOT     0

/* Scratch buffer for partial-sector read-modify-write */
static u8 _sector_buf[DEV_MC_SECTOR_SIZE] __attribute__((aligned(64)));

/* ── Init ────────────────────────────────────────────────────────────────── */

int mc_access_init(void) {
    int ret, type, free_space, format;

    ret = mcInit(MC_TYPE_XMC);
    if (ret < 0) {
        printf("mc_access: mcInit failed (%d)\n", ret);
        return -1;
    }

    /* Wait for card to enumerate */
    mcGetInfo(MC_PORT, MC_SLOT, &type, &free_space, &format);
    ret = mcSync(MC_WAIT, NULL, NULL);
    if (ret < 0) {
        printf("mc_access: mcGetInfo failed (%d)\n", ret);
        return -1;
    }

    if (type != MC_TYPE_PS2) {
        printf("mc_access: unexpected card type %d\n", type);
        /* SD2PSX emulates a PS2 card — if type is wrong something is off,
         * but continue anyway and let read/write errors surface naturally. */
    }

    printf("mc_access: card ready (type=%d free=%d format=%d)\n",
           type, free_space, format);
    return 0;
}

/* ── Single-sector primitives ────────────────────────────────────────────── */

int mc_read_sector(u32 sector, void *buf) {
    int ret;
    mcRead(MC_PORT, MC_SLOT, sector, 1, DEV_MC_SECTOR_SIZE, buf);
    mcSync(MC_WAIT, NULL, &ret);
    if (ret < 0) {
        printf("mc_access: read sector %lu failed (%d)\n", (unsigned long)sector, ret);
        return -1;
    }
    return 0;
}

int mc_write_sector(u32 sector, const void *buf) {
    int ret;
    /* ps2sdk mcWrite takes non-const but doesn't modify the buffer */
    mcWrite(MC_PORT, MC_SLOT, sector, 1, DEV_MC_SECTOR_SIZE, (void *)buf);
    mcSync(MC_WAIT, NULL, &ret);
    if (ret < 0) {
        printf("mc_access: write sector %lu failed (%d)\n", (unsigned long)sector, ret);
        return -1;
    }
    return 0;
}

/* ── Byte-addressed API ──────────────────────────────────────────────────── */

int mc_read_bytes(u32 card_byte_addr, void *buf, u32 len) {
    u8   *dst        = (u8 *)buf;
    u32   sector     = card_byte_addr / DEV_MC_SECTOR_SIZE;
    u32   offset     = card_byte_addr % DEV_MC_SECTOR_SIZE;
    u32   remaining  = len;

    while (remaining > 0) {
        if (mc_read_sector(sector, _sector_buf) < 0)
            return -1;

        u32 take = DEV_MC_SECTOR_SIZE - offset;
        if (take > remaining) take = remaining;

        memcpy(dst, _sector_buf + offset, take);

        dst       += take;
        remaining -= take;
        sector++;
        offset = 0;   /* only non-zero on the first iteration */
    }
    return 0;
}

int mc_write_bytes(u32 card_byte_addr, const void *buf, u32 len) {
    const u8 *src       = (const u8 *)buf;
    u32       sector    = card_byte_addr / DEV_MC_SECTOR_SIZE;
    u32       offset    = card_byte_addr % DEV_MC_SECTOR_SIZE;
    u32       remaining = len;

    while (remaining > 0) {
        u32 put = DEV_MC_SECTOR_SIZE - offset;
        if (put > remaining) put = remaining;

        /* Partial sector — read first, then modify */
        if (offset != 0 || put < DEV_MC_SECTOR_SIZE) {
            if (mc_read_sector(sector, _sector_buf) < 0)
                return -1;
        }

        memcpy(_sector_buf + offset, src, put);

        if (mc_write_sector(sector, _sector_buf) < 0)
            return -1;

        src       += put;
        remaining -= put;
        sector++;
        offset = 0;
    }
    return 0;
}
