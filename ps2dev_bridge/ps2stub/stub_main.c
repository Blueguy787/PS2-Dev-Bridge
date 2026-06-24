/**
 * stub_main.c
 *
 * PS2 dev channel stub — EE-side loader.
 *
 * This ELF is placed on the SD2PSX SD card and auto-booted via FMCB.
 * It runs on the EE processor and does three things forever:
 *
 *   1. POLL — read the doorbell sector from the dev channel via MC.
 *             When DEV_DOORBELL_MAGIC appears, an ELF is ready.
 *
 *   2. LOAD — read ELF data from the dev channel staging area into EE RAM
 *             at the address specified in the header.
 *
 *   3. ACK + JUMP — write DEV_ACK_MAGIC back to the card so the PC knows
 *             the ELF was claimed, then jump to the ELF entry point.
 *             After the ELF returns (or crashes), loop back to POLL.
 *
 * Stdout from the loaded ELF is captured via the ring buffer mechanism:
 * the stub installs a stdout hook before jumping, so printf() in the
 * loaded program automatically routes back to the PC.
 *
 * Build:
 *   ee-gcc -O2 -G0 -mno-gpopt -I$(PS2SDK)/ee/include \
 *          -I$(PS2SDK)/common/include \
 *          stub_main.c mc_access.c stdout_ring.c \
 *          -L$(PS2SDK)/ee/lib -lmc -lkernel -lc \
 *          -T$(PS2SDK)/ee/startup/linkfile \
 *          -o ps2dev_stub.elf
 *
 * Then place ps2dev_stub.elf on the SD card and add it as an FMCB launch item.
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <tamtypes.h>

#include "dev_channel_ps2.h"
#include "mc_access.h"
#include "stdout_ring.h"

/* ── ELF types ───────────────────────────────────────────────────────────── */

#define ELF_MAGIC       0x464C457Fu   /* '\x7fELF' little-endian u32     */
#define ET_EXEC         2
#define PT_LOAD         1
#define ELF_MIPS_ARCH   0x20000000u

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
} Elf32_Phdr;

/* ── Staging buffer ──────────────────────────────────────────────────────── */

/*
 * We DMA the ELF from the MC into this buffer first, then copy LOAD
 * segments to their target VAddrs. The buffer must be large enough for
 * the largest ELF we expect.
 *
 * DEV_ELF_MAX_SIZE = 512KB - 0x20 (header) - 32KB (stdout ring) ≈ 447KB.
 * Place in uncached segment to avoid cache coherency issues during
 * the MC DMA and the subsequent segment copies.
 */
#define STAGE_BUF_SIZE  DEV_ELF_MAX_SIZE
static u8 _stage_buf[STAGE_BUF_SIZE] __attribute__((aligned(64)));

/* One-sector header scratch */
static u8 _hdr_buf[DEV_MC_SECTOR_SIZE] __attribute__((aligned(64)));

/* ── ELF loading ─────────────────────────────────────────────────────────── */

typedef void (*EntryFn)(void);

/*
 * load_elf_from_staging()
 * Interpret the ELF in _stage_buf. Copy PT_LOAD segments to their
 * target addresses, flush the instruction cache, return entry point.
 * Returns 0 on error.
 */
static u32 load_elf_from_staging(u32 elf_size) {
    if (elf_size < sizeof(Elf32_Ehdr)) {
        printf("stub: ELF too small (%lu)\n", (unsigned long)elf_size);
        return 0;
    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)_stage_buf;

    /* Validate magic */
    u32 magic = ((u32)ehdr->e_ident[0])
              | ((u32)ehdr->e_ident[1] << 8)
              | ((u32)ehdr->e_ident[2] << 16)
              | ((u32)ehdr->e_ident[3] << 24);
    if (magic != ELF_MAGIC) {
        printf("stub: bad ELF magic %08lX\n", (unsigned long)magic);
        return 0;
    }

    if (ehdr->e_type != ET_EXEC) {
        printf("stub: not an executable ELF (type=%d)\n", ehdr->e_type);
        return 0;
    }

    /* Walk program headers and copy LOAD segments */
    u32 phoff = ehdr->e_phoff;
    u16 phnum = ehdr->e_phnum;

    for (u16 i = 0; i < phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(_stage_buf + phoff + i * sizeof(Elf32_Phdr));

        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_filesz == 0)
            continue;

        u8 *src  = _stage_buf + ph->p_offset;
        u8 *dst  = (u8 *)(u32)(ph->p_vaddr & 0x0FFFFFFFu); /* strip kseg bits */

        printf("stub: LOAD seg %d: 0x%08lX → 0x%08lX (%lu bytes)\n",
               i,
               (unsigned long)ph->p_offset,
               (unsigned long)ph->p_vaddr,
               (unsigned long)ph->p_filesz);

        memcpy(dst, src, ph->p_filesz);

        /* Zero BSS (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    /* Flush dcache and icache so the CPU sees the new code */
    FlushCache(0);   /* DCACHE writeback */
    FlushCache(2);   /* ICACHE invalidate */

    return ehdr->e_entry;
}

/* ── ACK ─────────────────────────────────────────────────────────────────── */

static void send_ack(void) {
    u8 ack_buf[4];
    dev_write_u32(ack_buf, 0, DEV_ACK_MAGIC);
    if (mc_write_bytes(DEV_CARD_BASE + DEV_OFF_ACK, ack_buf, 4) < 0)
        printf("stub: WARNING — failed to write ACK\n");
}

/* ── Clear doorbell ──────────────────────────────────────────────────────── */

static void clear_doorbell(void) {
    u8 clr[4] = { 0, 0, 0, 0 };
    mc_write_bytes(DEV_CARD_BASE + DEV_OFF_DOORBELL, clr, 4);
}

/* ── Main poll loop ──────────────────────────────────────────────────────── */

int main(void) {
    printf("\n");
    printf("===========================================\n");
    printf("  PS2 Dev Channel Stub\n");
    printf("  Waiting for ELF from PC via SD2PSX...\n");
    printf("===========================================\n");

    /* Init MC driver */
    if (mc_access_init() < 0) {
        printf("stub: FATAL — MC init failed. Is SD2PSX inserted?\n");
        SleepThread();
        return -1;
    }

    /* Init stdout ring — clears ring pointers */
    stdout_ring_init();
    stdout_ring_install();

    stdout_ring_printf("stub: online, polling doorbell at card sector %d\n",
                       DEV_DOORBELL_SECTOR);

    for (;;) {
        /*
         * Poll the doorbell sector. We read the full first sector of the
         * dev channel (512 bytes) which contains the entire header.
         * This is one MC transaction per poll cycle.
         */
        if (mc_read_sector(DEV_DOORBELL_SECTOR, _hdr_buf) < 0) {
            /* MC read failure — SD2PSX may be busy. Retry after short delay. */
            printf("stub: MC read error, retrying...\n");
            for (volatile int d = 0; d < 1000000; d++) {}
            continue;
        }

        u32 doorbell = dev_read_u32(_hdr_buf, DEV_OFF_DOORBELL);

        if (doorbell != DEV_DOORBELL_MAGIC) {
            /*
             * No ELF waiting. Brief spin delay then re-poll.
             * ~100ms at ~300MHz EE clock. Keeps the MC from being
             * hammered continuously while being responsive enough
             * to pick up a new ELF within 200ms of the PC committing it.
             */
            for (volatile int d = 0; d < 3000000; d++) {}
            continue;
        }

        /* ── ELF is ready ─────────────────────────────────────────────── */

        u32 elf_size  = dev_read_u32(_hdr_buf, DEV_OFF_ELF_SIZE);
        u32 load_addr = dev_read_u32(_hdr_buf, DEV_OFF_ELF_LOAD_ADDR);
        u32 flags     = dev_read_u32(_hdr_buf, DEV_OFF_FLAGS);

        printf("stub: doorbell! ELF %lu bytes → EE 0x%08lX flags=0x%08lX\n",
               (unsigned long)elf_size,
               (unsigned long)load_addr,
               (unsigned long)flags);

        if (elf_size == 0 || elf_size > STAGE_BUF_SIZE) {
            printf("stub: invalid ELF size %lu, ignoring\n", (unsigned long)elf_size);
            clear_doorbell();
            continue;
        }

        /* Reset stdout ring for new session unless KEEP_STDOUT is set */
        if (!(flags & DEV_FLAG_KEEP_STDOUT)) {
            u8 ring_reset[8] = { 0 };
            mc_write_bytes(DEV_CARD_BASE + DEV_OFF_STDOUT_HEAD, ring_reset, 8);
            stdout_ring_init();
        }

        /* DMA ELF from card staging area into _stage_buf */
        printf("stub: reading ELF from card...\n");
        u32 elf_card_addr = DEV_CARD_BASE + DEV_OFF_ELF_DATA;
        u32 bytes_read = 0;
        int read_ok = 1;

        while (bytes_read < elf_size) {
            u32 chunk = elf_size - bytes_read;
            if (chunk > DEV_MC_SECTOR_SIZE) chunk = DEV_MC_SECTOR_SIZE;

            if (mc_read_bytes(elf_card_addr + bytes_read,
                              _stage_buf + bytes_read, chunk) < 0) {
                printf("stub: read error at offset %lu\n", (unsigned long)bytes_read);
                read_ok = 0;
                break;
            }
            bytes_read += chunk;
        }

        if (!read_ok) {
            printf("stub: ELF read failed, clearing doorbell\n");
            clear_doorbell();
            continue;
        }

        printf("stub: ELF read complete (%lu bytes)\n", (unsigned long)bytes_read);

        /* Parse ELF and copy segments to target VAddrs */
        u32 entry = load_elf_from_staging(elf_size);
        if (entry == 0) {
            printf("stub: ELF load failed\n");
            clear_doorbell();
            continue;
        }

        printf("stub: entry point 0x%08lX — sending ACK\n", (unsigned long)entry);

        /*
         * Send ACK before jumping so the PC sees it promptly.
         * The PC's wait_for_ack() will return, and stream_stdout()
         * will start draining the ring as the ELF runs.
         */
        send_ack();
        clear_doorbell();

        /* Small delay to let the ACK propagate through the MC layer */
        for (volatile int d = 0; d < 500000; d++) {}

        stdout_ring_printf("stub: jumping to 0x%08lX\n\n", (unsigned long)entry);

        /*
         * Jump to ELF entry point.
         *
         * The loaded ELF's printf() will route through stdout_ring_write()
         * if stdout_ring_install() succeeded. Otherwise the ELF must call
         * stdout_ring_write() directly.
         *
         * If the ELF returns, we loop back to polling.
         */
        EntryFn elf_entry = (EntryFn)(entry);
        elf_entry();

        stdout_ring_printf("\nstub: ELF returned, re-entering poll loop\n");
        printf("stub: ELF returned, polling for next ELF\n");

        /* Give the PC time to drain any remaining stdout before the
         * next session potentially clears the ring */
        for (volatile int d = 0; d < 30000000; d++) {}
    }

    return 0;
}
