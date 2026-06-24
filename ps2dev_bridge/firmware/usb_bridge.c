/**
 * usb_bridge.c
 *
 * Live USB-C data bridge for SD2PSX + PS2 dev channel.
 *
 * Core 0 only. No SIO bus access. PSRAM protected by critical_section.
 *
 * Dev channel additions over the original bridge:
 *
 *   ELF_PUSH   — PC streams ELF chunks into PSRAM staging area.
 *                Address range: DEV_ADDR_ELF_DATA .. DEV_ADDR_ELF_DATA+len
 *                Never touches dirty tracking — dev region never flushes.
 *
 *   ELF_COMMIT — PC signals ELF transfer is complete. Firmware writes the
 *                header (size, load addr, flags) then rings the doorbell
 *                by writing DEV_DOORBELL_MAGIC to DEV_ADDR_DOORBELL.
 *                PS2 stub polls this address via normal card reads.
 *
 *   STDOUT_PULL — Firmware reads STDOUT_HEAD from PSRAM (written by PS2
 *                 stub), compares to local tail, returns available bytes
 *                 from the ring, advances tail.
 *
 *   DEV_STATUS  — Returns usb_bridge_dev_status_t: doorbell state, ack,
 *                 card busy, stdout bytes pending.
 *
 *   DEV_RESET   — Clears doorbell, ACK, and stdout ring pointers.
 *
 * Normal READ/WRITE are clamped at DEV_CHANNEL_BASE — they cannot reach
 * the dev region regardless of what the host sends.
 *
 * Dirty marking is skipped entirely for any write that touches the dev
 * channel region, so it is never flushed to the SD card.
 */

#include "usb_bridge.h"
#include "dev_channel.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "pico/stdlib.h"
#include "settings.h"
#include "debug.h"

#ifdef WITH_PSRAM
extern void psram_read (uint32_t addr, void *buf, size_t sz);
extern void psram_write(uint32_t addr, void *buf, size_t sz);
#endif

#include "ps1/ps1_dirty.h"
#include "ps2/ps2_dirty.h"
#include "bigmem.h"

/* ── Internal state ─────────────────────────────────────────────────────── */

static bool    bridge_card_busy     = false;
static bool    bridge_pending_write = false;

/* Stdout ring tail — PC side drain pointer, mirrored in PSRAM for the stub */
static uint32_t stdout_tail = 0;

static uint8_t rx_buf[USB_BRIDGE_MAX_PACKET];
static size_t  rx_len = 0;

static uint8_t data_buf[USB_BRIDGE_MAX_PAYLOAD];

/* ── USB descriptors ────────────────────────────────────────────────────── */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,
    .idProduct          = 0x000A,
    .bcdDevice          = 0x0200,   /* device version bumped to match protocol */
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},
    "SD2PSX",
    "SD2PSX USB Bridge",
    "000002",
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

const uint8_t *tud_descriptor_device_cb(void)              { return (const uint8_t *)&desc_device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t i)  { (void)i; return desc_configuration; }

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[32];
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&desc_str[1], string_desc[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
        const char *str = string_desc[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = str[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}

/* ── Checksum ───────────────────────────────────────────────────────────── */

static uint8_t xor_checksum(const uint8_t *buf, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs ^= buf[i];
    return cs;
}

/* ── Response helpers ───────────────────────────────────────────────────── */

static void send_response(uint8_t rsp, const uint8_t *data, size_t data_len) {
    uint8_t out[USB_BRIDGE_MAX_PAYLOAD + 2];
    out[0] = rsp;
    if (data && data_len > 0)
        memcpy(out + 1, data, data_len);
    out[1 + data_len] = xor_checksum(out, 1 + data_len);
    tud_cdc_write(out, 2 + data_len);
    tud_cdc_write_flush();
}

static void send_ok(void)     { send_response(USB_BRIDGE_RSP_OK,     NULL, 0); }
static void send_err(void)    { send_response(USB_BRIDGE_RSP_ERR,    NULL, 0); }
static void send_busy(void)   { send_response(USB_BRIDGE_RSP_BUSY,   NULL, 0); }
static void send_nodata(void) { send_response(USB_BRIDGE_RSP_NODATA, NULL, 0); }

/* ── PSRAM helpers for dev channel header fields ────────────────────────── */

static void psram_write_u32(uint32_t addr, uint32_t val) {
#ifdef WITH_PSRAM
    uint8_t buf[4] = {
        (val >> 24) & 0xFF,
        (val >> 16) & 0xFF,
        (val >>  8) & 0xFF,
         val        & 0xFF,
    };
    psram_write(addr, buf, 4);
#else
    (void)addr; (void)val;
#endif
}

static uint32_t psram_read_u32(uint32_t addr) {
#ifdef WITH_PSRAM
    uint8_t buf[4];
    psram_read(addr, buf, 4);
    return ((uint32_t)buf[0] << 24)
         | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] <<  8)
         |  (uint32_t)buf[3];
#else
    (void)addr;
    return 0;
#endif
}

/* ── Dirty marking (card region only — dev channel excluded) ────────────── */

static void bridge_mark_dirty(uint32_t addr, size_t len) {
    /*
     * Never mark dirty anything in the dev channel region.
     * Clamp len so normal writes can't accidentally bleed into it.
     */
    uint32_t safe_len = dev_channel_clamp_len(addr, (uint32_t)len);
    if (safe_len == 0) return;

    if (settings_get_mode(true) == MODE_PS1) {
        uint32_t card_size = sizeof(bigmem.ps1.card_image);
        if (addr >= card_size) return;
        if (addr + safe_len > card_size)
            safe_len = card_size - addr;

#ifdef WITH_PSRAM
        ps1_dirty_lock();
        psram_read(addr, &bigmem.ps1.card_image[addr], safe_len);
        ps1_dirty_unlock();
#endif
        uint32_t s0 = addr / 128;
        uint32_t s1 = (addr + safe_len - 1) / 128;
        ps1_dirty_lock();
        for (uint32_t s = s0; s <= s1; s++)
            ps1_dirty_mark(s);
        ps1_dirty_lockout_renew();
        ps1_dirty_unlock();

    } else {
        uint32_t s0 = addr / 512;
        uint32_t s1 = (addr + safe_len - 1) / 512;
        ps2_dirty_lock();
        for (uint32_t s = s0; s <= s1; s++)
            ps2_dirty_mark(s);
        ps2_dirty_lockout_renew();
        ps2_dirty_unlock();
    }
}

/* ── Dev channel — ELF commit (ring the doorbell) ───────────────────────── */

static void dev_ring_doorbell(uint32_t load_addr, uint32_t elf_size, uint32_t flags) {
#ifdef WITH_PSRAM
    /* Write header fields before the doorbell so the stub reads consistent data */
    psram_write_u32(DEV_ADDR_ELF_SIZE,      elf_size);
    psram_write_u32(DEV_ADDR_ELF_LOAD_ADDR, load_addr ? load_addr : DEV_DEFAULT_LOAD_ADDR);
    psram_write_u32(DEV_ADDR_FLAGS,         flags);
    psram_write_u32(DEV_ADDR_ACK,           DEV_DOORBELL_CLEAR);

    /* Doorbell last — stub spins on this */
    psram_write_u32(DEV_ADDR_DOORBELL, DEV_DOORBELL_MAGIC);

    DPRINTF("dev_channel: doorbell armed — ELF %lu bytes → EE 0x%08lX\n",
            (unsigned long)elf_size, (unsigned long)load_addr);
#else
    (void)load_addr; (void)elf_size; (void)flags;
#endif
}

/* ── Dev channel — stdout pull ──────────────────────────────────────────── */

/*
 * The PS2 stub advances STDOUT_HEAD as it writes bytes into the ring.
 * We read HEAD from PSRAM, compute available bytes, copy them out,
 * then advance our local tail and write it back to STDOUT_TAIL in PSRAM
 * so the stub knows the space is free.
 */
static uint16_t dev_stdout_pull(uint8_t *out, uint16_t max_len) {
#ifdef WITH_PSRAM
    uint32_t head = psram_read_u32(DEV_ADDR_STDOUT_HEAD);
    uint32_t tail = stdout_tail;

    /* Ring arithmetic */
    uint32_t available;
    if (head >= tail)
        available = head - tail;
    else
        available = DEV_STDOUT_RING_SIZE - tail + head;

    if (available == 0) return 0;

    uint16_t to_read = (available > max_len) ? max_len : (uint16_t)available;

    /* May wrap — read in up to two segments */
    uint32_t seg1 = DEV_STDOUT_RING_SIZE - tail;
    if (seg1 >= to_read) {
        psram_read(DEV_ADDR_STDOUT_RING + tail, out, to_read);
    } else {
        psram_read(DEV_ADDR_STDOUT_RING + tail, out, seg1);
        psram_read(DEV_ADDR_STDOUT_RING,        out + seg1, to_read - seg1);
    }

    stdout_tail = (tail + to_read) % DEV_STDOUT_RING_SIZE;
    psram_write_u32(DEV_ADDR_STDOUT_TAIL, stdout_tail);

    return to_read;
#else
    (void)out; (void)max_len;
    return 0;
#endif
}

/* ── Dev channel — reset ────────────────────────────────────────────────── */

static void dev_reset(void) {
#ifdef WITH_PSRAM
    psram_write_u32(DEV_ADDR_DOORBELL,    DEV_DOORBELL_CLEAR);
    psram_write_u32(DEV_ADDR_ACK,         DEV_DOORBELL_CLEAR);
    psram_write_u32(DEV_ADDR_ELF_SIZE,    0);
    psram_write_u32(DEV_ADDR_STDOUT_HEAD, 0);
    psram_write_u32(DEV_ADDR_STDOUT_TAIL, 0);
    stdout_tail = 0;
    DPRINTF("dev_channel: reset\n");
#endif
}

/* ── Packet processor ───────────────────────────────────────────────────── */

static void process_packet(void) {
    if (rx_len < USB_BRIDGE_HEADER_SIZE + 1) { send_err(); return; }

    uint8_t  magic = rx_buf[0];
    uint8_t  cmd   = rx_buf[1];
    uint32_t addr  = ((uint32_t)rx_buf[2] << 16)
                   | ((uint32_t)rx_buf[3] <<  8)
                   |  (uint32_t)rx_buf[4];
    uint16_t len   = ((uint16_t)rx_buf[5] << 8) | rx_buf[6];

    if (magic != USB_BRIDGE_MAGIC)    { send_err(); return; }
    if (len > USB_BRIDGE_MAX_PAYLOAD) { send_err(); return; }

    /* Verify checksum */
    size_t body_len = USB_BRIDGE_HEADER_SIZE
                    + ((cmd == USB_BRIDGE_CMD_WRITE || cmd == USB_BRIDGE_CMD_ELF_PUSH)
                       ? len : 0);
    if (xor_checksum(rx_buf, body_len) != rx_buf[body_len]) {
        DPRINTF("usb_bridge: checksum mismatch\n");
        send_err();
        return;
    }

    switch (cmd) {

        /* ── Original commands ─────────────────────────────────────────── */

        case USB_BRIDGE_CMD_STATUS: {
            uint8_t status[2] = { USB_BRIDGE_VERSION, bridge_card_busy ? 0x01 : 0x00 };
            send_response(USB_BRIDGE_RSP_OK, status, sizeof(status));
            break;
        }

        case USB_BRIDGE_CMD_READ: {
            if (len == 0) { send_err(); break; }
            /* Clamp read to card region — cannot read dev channel this way */
            uint32_t safe_len = dev_channel_clamp_len(addr, len);
            if (safe_len == 0) { send_err(); break; }
#ifdef WITH_PSRAM
            psram_read(addr, data_buf, safe_len);
            send_response(USB_BRIDGE_RSP_OK, data_buf, safe_len);
#else
            send_err();
#endif
            break;
        }

        case USB_BRIDGE_CMD_WRITE: {
            if (bridge_card_busy) { send_busy(); break; }
            if (len == 0) { send_err(); break; }
            uint32_t safe_len = dev_channel_clamp_len(addr, len);
            if (safe_len == 0) { send_err(); break; }
#ifdef WITH_PSRAM
            psram_write(addr, rx_buf + USB_BRIDGE_HEADER_SIZE, safe_len);
            bridge_mark_dirty(addr, safe_len);
            bridge_pending_write = true;
            send_ok();
#else
            send_err();
#endif
            break;
        }

        case USB_BRIDGE_CMD_FLUSH: {
            bridge_pending_write = false;
            send_ok();
            break;
        }

        /* ── Dev channel commands ──────────────────────────────────────── */

        case USB_BRIDGE_CMD_ELF_PUSH: {
            /*
             * addr  = byte offset into ELF staging area (0-based)
             * len   = chunk size
             * data  = ELF bytes
             *
             * PC streams chunks sequentially. No ordering enforcement here —
             * host is responsible for in-order delivery, which serial CDC
             * guarantees anyway.
             */
            if (len == 0) { send_err(); break; }

            uint32_t staging_addr = DEV_ADDR_ELF_DATA + addr;

            /* Bounds check — don't overflow the staging area */
            if (addr + len > DEV_ELF_MAX_SIZE) {
                DPRINTF("dev_channel: ELF_PUSH overflow addr=%lu len=%u\n",
                        (unsigned long)addr, len);
                send_err();
                break;
            }

#ifdef WITH_PSRAM
            psram_write(staging_addr, rx_buf + USB_BRIDGE_HEADER_SIZE, len);
            /* No dirty marking — dev region never goes to SD */
            send_ok();
#else
            send_err();
#endif
            break;
        }

        case USB_BRIDGE_CMD_ELF_COMMIT: {
            /*
             * addr  = EE RAM load address (0 = use DEV_DEFAULT_LOAD_ADDR)
             * len   = total ELF size in bytes
             * data  = none
             *
             * Writes header + rings doorbell. PS2 stub wakes up.
             */
            if (len == 0) { send_err(); break; }
            dev_ring_doorbell(addr, (uint32_t)len, 0);
            send_ok();
            break;
        }

        case USB_BRIDGE_CMD_STDOUT_PULL: {
            /*
             * addr  = 0 (unused)
             * len   = max bytes to return (capped at MAX_PAYLOAD)
             */
            uint16_t want = (len == 0 || len > USB_BRIDGE_MAX_PAYLOAD)
                            ? USB_BRIDGE_MAX_PAYLOAD : len;
            uint16_t got = dev_stdout_pull(data_buf, want);
            if (got == 0) {
                send_nodata();
            } else {
                send_response(USB_BRIDGE_RSP_OK, data_buf, got);
            }
            break;
        }

        case USB_BRIDGE_CMD_DEV_STATUS: {
#ifdef WITH_PSRAM
            uint32_t doorbell = psram_read_u32(DEV_ADDR_DOORBELL);
            uint32_t ack      = psram_read_u32(DEV_ADDR_ACK);
            uint32_t head     = psram_read_u32(DEV_ADDR_STDOUT_HEAD);
            uint32_t tail     = stdout_tail;

            uint32_t available;
            if (head >= tail)
                available = head - tail;
            else
                available = DEV_STDOUT_RING_SIZE - tail + head;

            usb_bridge_dev_status_t st = {
                .doorbell_armed  = (doorbell == DEV_DOORBELL_MAGIC) ? 1 : 0,
                .ack_received    = (ack      == DEV_ACK_MAGIC)      ? 1 : 0,
                .card_busy       = bridge_card_busy ? 1 : 0,
                .reserved        = 0,
                .stdout_pending  = (available > 0xFFFF) ? 0xFFFF : (uint16_t)available,
            };
            send_response(USB_BRIDGE_RSP_OK, (uint8_t *)&st, sizeof(st));
#else
            send_err();
#endif
            break;
        }

        case USB_BRIDGE_CMD_DEV_RESET: {
            dev_reset();
            send_ok();
            break;
        }

        default:
            send_err();
            break;
    }
}

/* ── Packet accumulator ─────────────────────────────────────────────────── */

static void accumulate_and_dispatch(void) {
    uint32_t available = tud_cdc_available();
    if (available == 0) return;

    size_t space = sizeof(rx_buf) - rx_len;
    if (available > space) available = (uint32_t)space;

    rx_len += tud_cdc_read(rx_buf + rx_len, available);

    if (rx_len < USB_BRIDGE_HEADER_SIZE) return;

    if (rx_buf[0] != USB_BRIDGE_MAGIC) {
        DPRINTF("usb_bridge: bad magic %02X, resyncing\n", rx_buf[0]);
        rx_len = 0;
        return;
    }

    uint8_t  cmd      = rx_buf[1];
    uint16_t len      = ((uint16_t)rx_buf[5] << 8) | rx_buf[6];
    size_t   expected = USB_BRIDGE_HEADER_SIZE + 1;

    if (cmd == USB_BRIDGE_CMD_WRITE || cmd == USB_BRIDGE_CMD_ELF_PUSH)
        expected += len;

    if (rx_len < expected) return;

    process_packet();
    rx_len = 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void usb_bridge_init(void) {
    DPRINTF("usb_bridge: init (v%d, dev channel at 0x%06X)\n",
            USB_BRIDGE_VERSION, DEV_CHANNEL_BASE);
    tusb_init();

    /*
     * Clear the dev channel header on boot so a stale doorbell from a
     * previous session doesn't immediately trigger the PS2 stub.
     */
    dev_reset();
}

void usb_bridge_task(void) {
    tud_task();
    if (tud_cdc_connected())
        accumulate_and_dispatch();
}

void usb_bridge_notify_card_busy(bool busy) {
    bridge_card_busy = busy;
}

bool usb_bridge_has_pending_write(void) {
    return bridge_pending_write;
}
