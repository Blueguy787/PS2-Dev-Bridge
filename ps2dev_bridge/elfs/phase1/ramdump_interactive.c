/**
 * ramdump_interactive.c — Phase 1b extended
 *
 * Like ramdump.c but the dump target is controlled by the PC at runtime.
 * No recompile needed to dump a different address range.
 *
 * Protocol:
 *   PC writes a 12-byte command record into the dev channel header's
 *   reserved field (DEV_OFF_ACK + 8, safe after ACK is consumed):
 *
 *     +0  u32 BE  target address
 *     +4  u32 BE  length in bytes
 *     +8  u32 BE  command magic (0x44554D50 = 'DUMP')
 *
 *   ELF polls that location, executes the dump, writes 0 to the magic
 *   field when done, then loops waiting for the next command.
 *
 *   PC sends a command with:
 *     bridge.write_block(DEV_CARD_BASE + DEV_OFF_CMD, struct.pack('>III', addr, length, 0x44554D50))
 *
 *   Then streams stdout for the result.
 *
 * Push once, dump as many ranges as you want:
 *   python sd2psx_cli.py elf ramdump_interactive.elf --no-stdout
 *   python sd2psx_cli.py stdout &
 *   # then from Python:
 *   #   bridge.write_block(cmd_addr, struct.pack('>III', 0x00080000, 0x1000, 0x44554D50))
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include "dev_io.h"
#include "../../ps2stub/dev_channel_ps2.h"
#include "../../ps2stub/mc_access.h"

/* Command sits in the dev channel header after ACK (offset +0x18 = STDOUT_TAIL
 * area won't work — use the reserved dword at +0x001C, which is 4 bytes.
 * We need 12 bytes so we use +0x001C (reserved) and the first 8 bytes of ELF_DATA.
 * Cleaner: use a fixed card byte address just past the header. */
#define CMD_CARD_ADDR   (DEV_CARD_BASE + 0x0080u)   /* 128 bytes past base, safe */
#define CMD_MAGIC       0x44554D50u                   /* 'DUMP' */
#define CHUNK_SIZE      256u

static u8  _cmd_buf[16] __attribute__((aligned(64)));

static int read_command(u32 *out_addr, u32 *out_len) {
    if (mc_read_bytes(CMD_CARD_ADDR, _cmd_buf, 12) < 0)
        return -1;
    u32 magic = dev_read_u32(_cmd_buf, 8);
    if (magic != CMD_MAGIC)
        return 0;   /* no command yet */
    *out_addr = dev_read_u32(_cmd_buf, 0);
    *out_len  = dev_read_u32(_cmd_buf, 4);
    return 1;
}

static void clear_command(void) {
    u8 clr[12] = { 0 };
    mc_write_bytes(CMD_CARD_ADDR, clr, 12);
}

static void do_dump(u32 addr, u32 len) {
    DEV_INF("dump  addr=0x%08lX  len=%lu",
            (unsigned long)addr, (unsigned long)len);

    /* Basic sanity — don't read outside mapped EE RAM (32MB) */
    if (addr > 0x02000000u || len == 0 || len > 0x200000u) {
        DEV_ERR("invalid range");
        return;
    }

    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
        DEV_HEX(addr + done, (void *)(addr + done), chunk);
        done += chunk;
        for (volatile int d = 0; d < 4500000; d++) {}
    }

    DEV_INF("dump complete  %lu bytes", (unsigned long)len);
}

int main(void) {
    dev_io_init();

    if (mc_access_init() < 0) {
        DEV_ERR("mc_access_init failed");
        return -1;
    }

    DEV_INF("ramdump interactive — waiting for DUMP commands");
    DEV_INF("cmd addr: card 0x%08lX", (unsigned long)CMD_CARD_ADDR);
    DEV_INF("PC: bridge.write_block(0x%08lX, pack('>III', addr, len, 0x44554D50))",
            (unsigned long)CMD_CARD_ADDR);

    /* Clear any stale command */
    clear_command();

    for (;;) {
        u32 tgt_addr = 0, tgt_len = 0;
        int r = read_command(&tgt_addr, &tgt_len);

        if (r < 0) {
            DEV_ERR("mc read error");
            for (volatile int d = 0; d < 9000000; d++) {}
            continue;
        }

        if (r == 1) {
            clear_command();
            do_dump(tgt_addr, tgt_len);
        }

        /* Poll ~200ms */
        for (volatile int d = 0; d < 18000000; d++) {}
    }

    return 0;
}
