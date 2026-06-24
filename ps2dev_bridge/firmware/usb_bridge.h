#pragma once

/**
 * usb_bridge.h
 *
 * Live USB-C data bridge for SD2PSX + PS2 dev channel.
 *
 * Runs on core 0 alongside debug_task(), ps1_task(), ps2_task().
 * Never touches core 1 or the SIO bus.
 * All PSRAM access goes through the existing critical_section — thread safe.
 *
 * Dev channel commands (in addition to original READ/WRITE/FLUSH/STATUS):
 *
 *   ELF_PUSH   — stream an ELF into the dev channel staging area and ring
 *                the doorbell. PS2 stub picks it up and jumps to it.
 *
 *   STDOUT_PULL — drain bytes from the PS2 stdout ring buffer back to host.
 *
 *   DEV_STATUS  — query dev channel state (doorbell, ack, stdout pending).
 *
 * Normal READ/WRITE are still available for raw card image manipulation,
 * but they are clamped at DEV_CHANNEL_BASE — they cannot touch the dev
 * region, keeping card data and dev traffic strictly separated.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Protocol constants ─────────────────────────────────────────────────── */

#define USB_BRIDGE_MAGIC        0x5D
#define USB_BRIDGE_VERSION      0x02   /* bumped — dev channel support    */

/* Commands (host → device) */
#define USB_BRIDGE_CMD_READ         0x01
#define USB_BRIDGE_CMD_WRITE        0x02
#define USB_BRIDGE_CMD_FLUSH        0x03
#define USB_BRIDGE_CMD_STATUS       0x04
#define USB_BRIDGE_CMD_ELF_PUSH     0x10   /* write ELF chunk to staging  */
#define USB_BRIDGE_CMD_ELF_COMMIT   0x11   /* ring doorbell, ELF is ready */
#define USB_BRIDGE_CMD_STDOUT_PULL  0x12   /* pull N bytes from stdout    */
#define USB_BRIDGE_CMD_DEV_STATUS   0x13   /* query dev channel state     */
#define USB_BRIDGE_CMD_DEV_RESET    0x14   /* clear doorbell + stdout ring */

/* Responses (device → host) */
#define USB_BRIDGE_RSP_OK       0xA0
#define USB_BRIDGE_RSP_ERR      0xA1
#define USB_BRIDGE_RSP_BUSY     0xA2
#define USB_BRIDGE_RSP_NODATA   0xA3   /* STDOUT_PULL with empty ring    */

/*
 * Packet layout (unchanged from v1):
 *   [1B magic][1B cmd][3B addr BE][2B len BE][NB data][1B checksum]
 *
 * For ELF_PUSH:  addr = byte offset into ELF staging area, len = chunk size
 * For ELF_COMMIT: addr = EE load address, len = total ELF size (no data)
 * For STDOUT_PULL: addr = 0, len = max bytes to return
 * For DEV_STATUS: addr = 0, len = 0
 * For DEV_RESET:  addr = 0, len = 0
 */

#define USB_BRIDGE_HEADER_SIZE  7
#define USB_BRIDGE_MAX_PAYLOAD  512
#define USB_BRIDGE_MAX_PACKET   (USB_BRIDGE_HEADER_SIZE + USB_BRIDGE_MAX_PAYLOAD + 1)

/* ── Card address limits ────────────────────────────────────────────────── */

#define USB_BRIDGE_PS1_CARD_SIZE  (128 * 1024)
#define USB_BRIDGE_PS2_CARD_SIZE  (8 * 1024 * 1024)

/* ── DEV_STATUS response payload (6 bytes) ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  doorbell_armed;   /* 1 = ELF waiting, 0 = idle              */
    uint8_t  ack_received;     /* 1 = PS2 stub has claimed the ELF        */
    uint8_t  card_busy;        /* 1 = mid memory-card transaction         */
    uint8_t  reserved;
    uint16_t stdout_pending;   /* bytes available in stdout ring          */
} usb_bridge_dev_status_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

void usb_bridge_init(void);
void usb_bridge_task(void);
void usb_bridge_notify_card_busy(bool busy);
bool usb_bridge_has_pending_write(void);
