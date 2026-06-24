/**
 * launcher_main.c
 *
 * PS2 Dev Channel Launcher
 * Selected from the FreeMCBoot menu.
 *
 * What it does:
 *   1. Loads IOP modules (SIO2MAN, MCMAN, MCSERV) from ROM
 *   2. Initialises the GS for a live status screen
 *   3. Loads ps2dev_stub.elf from mc0:/PS2DEV/ps2dev_stub.elf
 *   4. Runs the stub as a lower-priority EE thread
 *   5. Main thread stays alive, polls dev channel header every ~500ms,
 *      and refreshes the status display each vsync:
 *        - Stub running state
 *        - MC / bridge ready
 *        - Doorbell (idle / armed / ack'd)
 *        - Last ELF pushed (size + load addr)
 *        - stdout ring fill %
 *        - Total ELFs run this session
 *
 * Build:
 *   make -C launcher
 *
 * Deploy:
 *   mc0:/PS2DEV/ps2dev_launcher.elf
 *
 * FMCB entry:
 *   Name: PS2 Dev
 *   Path: mc0:/PS2DEV/ps2dev_launcher.elf
 */

#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <tamtypes.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <packet.h>
#include <gs_psm.h>

#include "../ps2stub/dev_channel_ps2.h"
#include "../ps2stub/mc_access.h"

/* ── Config ──────────────────────────────────────────────────────────────── */

#define STUB_PATH           "mc0:/PS2DEV/ps2dev_stub.elf"
#define STUB_STACK_ADDR     0x01500000
#define STUB_STACK_SIZE     (512 * 1024)
#define STUB_THREAD_PRIO    32          /* lower than main (16) */
#define POLL_FRAMES         30          /* MC poll every 30 vsyncs (~500ms) */

/* Screen */
#define SCREEN_W    640
#define SCREEN_H    448

/* Colours: 0x00BBGGRR (draw library uses BGR) */
#define COL_BG      0x00101010
#define COL_TITLE   0x0080FF40
#define COL_LABEL   0x00AAAAAA
#define COL_VALUE   0x00FFFFFF
#define COL_ARMED   0x00FFFF00
#define COL_ACK     0x0040FF40
#define COL_IDLE    0x00555555
#define COL_WARN    0x000080FF
#define COL_ERR     0x000000FF

/* Layout */
#define COL_L   48
#define COL_R   240
#define ROW(n)  (60 + (n) * 24)

/* ── Framebuffer ─────────────────────────────────────────────────────────── */

static framebuffer_t _fb[2];
static zbuffer_t     _zb;
static int           _active = 0;

static void gs_init(void) {
    graph_set_mode(GRAPH_MODE_AUTO, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
    graph_set_screen(0, 0, SCREEN_W, SCREEN_H);
    graph_set_bgcolor(0, 0, 0);

    for (int i = 0; i < 2; i++) {
        _fb[i].width   = SCREEN_W;
        _fb[i].height  = SCREEN_H;
        _fb[i].mask    = 0;
        _fb[i].psm     = GS_PSM_CT16;
        _fb[i].address = graph_vram_allocate(SCREEN_W, SCREEN_H,
                                             GS_PSM_CT16, GRAPH_ALIGN_PAGE);
    }

    _zb.enable  = DRAW_DISABLE;
    _zb.address = 0;
    _zb.zsm     = 0;
    _zb.mask    = 0;

    graph_set_framebuffer_filtered(_fb[0].address, _fb[0].width, _fb[0].psm, 0, 0);
    graph_wait_vsync();
}

static void gs_flip(void) {
    graph_set_framebuffer_filtered(_fb[_active].address,
                                   _fb[_active].width,
                                   _fb[_active].psm, 0, 0);
    _active ^= 1;
}

/* ── Draw helpers ────────────────────────────────────────────────────────── */

static qword_t _qbuf[2048] __attribute__((aligned(64)));

static void screen_clear(void) {
    packet_t *p = packet_init(64, PACKET_UCAB);
    qword_t  *q = p->data;
    q = draw_disable_tests(q, 0, &_zb);
    q = draw_clear(q, 0,
                   2048.0f - SCREEN_W / 2.0f, 2048.0f - SCREEN_H / 2.0f,
                   (float)SCREEN_W, (float)SCREEN_H,
                    COL_BG        & 0xFF,
                   (COL_BG >> 8)  & 0xFF,
                   (COL_BG >> 16) & 0xFF);
    q = draw_enable_tests(q, 0, &_zb);
    q = draw_finish(q);
    dma_channel_send_normal_ucab(DMA_CHANNEL_GIF, p->data, q - p->data, 0);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    draw_wait_finish();
    packet_free(p);
}

static void screen_text(int x, int y, u32 col, const char *str) {
    qword_t *q = _qbuf;
    q = draw_setup_environment(q, 0, &_fb[_active], &_zb);
    q = draw_primitive_xyoffset(q, 0,
                                2048 - SCREEN_W / 2,
                                2048 - SCREEN_H / 2);
    color_t c = {
         col        & 0xFF,
        (col >> 8)  & 0xFF,
        (col >> 16) & 0xFF,
        0x80, 0
    };
    q = draw_string(q, 0, (texenv_t *)0, x, y, &c, str);
    q = draw_finish(q);
    dma_channel_send_normal_ucab(DMA_CHANNEL_GIF, _qbuf, q - _qbuf, 0);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    draw_wait_finish();
}

/* ── Dev channel state ───────────────────────────────────────────────────── */

static u8  _hdr_raw[DEV_MC_SECTOR_SIZE] __attribute__((aligned(64)));

static u32 _doorbell    = 0;
static u32 _elf_size    = 0;
static u32 _load_addr   = 0;
static u32 _ack         = 0;
static u32 _stdout_head = 0;
static u32 _stdout_tail = 0;
static u32 _elfs_run    = 0;
static u32 _last_sz     = 0;
static u32 _last_load   = 0;

static int  _mc_ready    = 0;
static int  _stub_loaded = 0;
static char _status[80]  = "Starting...";

static void poll_dev_header(void) {
    if (!_mc_ready) return;
    if (mc_read_sector(DEV_DOORBELL_SECTOR, _hdr_raw) < 0) {
        snprintf(_status, sizeof(_status), "MC read error");
        return;
    }
    _doorbell    = dev_read_u32(_hdr_raw, DEV_OFF_DOORBELL);
    _elf_size    = dev_read_u32(_hdr_raw, DEV_OFF_ELF_SIZE);
    _load_addr   = dev_read_u32(_hdr_raw, DEV_OFF_ELF_LOAD_ADDR);
    _ack         = dev_read_u32(_hdr_raw, DEV_OFF_ACK);
    _stdout_head = dev_read_u32(_hdr_raw, DEV_OFF_STDOUT_HEAD);
    _stdout_tail = dev_read_u32(_hdr_raw, DEV_OFF_STDOUT_TAIL);

    static u32 prev_ack = 0;
    if (_ack == DEV_ACK_MAGIC && prev_ack != DEV_ACK_MAGIC) {
        _elfs_run++;
        _last_sz   = _elf_size;
        _last_load = _load_addr;
    }
    prev_ack = _ack;
}

/* ── Stub loader ─────────────────────────────────────────────────────────── */

static int load_and_start_stub(void) {
    t_ExecData ed;

    snprintf(_status, sizeof(_status), "Loading %s ...", STUB_PATH);
    screen_clear();

    int ret = SifLoadElf(STUB_PATH, &ed);
    if (ret < 0) {
        snprintf(_status, sizeof(_status), "SifLoadElf failed: %d", ret);
        return -1;
    }

    /* Run stub as a secondary EE thread so the launcher keeps its stack */
    ee_thread_t t = {
        .func             = (void *)ed.epc,
        .stack            = (void *)STUB_STACK_ADDR,
        .stack_size       = STUB_STACK_SIZE,
        .gp_reg           = (void *)ed.gp,
        .initial_priority = STUB_THREAD_PRIO,
    };

    int tid = CreateThread(&t);
    if (tid < 0) {
        snprintf(_status, sizeof(_status), "CreateThread failed: %d", tid);
        return -1;
    }

    StartThread(tid, NULL);
    snprintf(_status, sizeof(_status), "Stub thread %d running", tid);
    _stub_loaded = 1;
    return 0;
}

/* ── Status screen render ────────────────────────────────────────────────── */

static char _tmp[128];

static void draw_status_screen(void) {
    screen_clear();

    /* Title */
    screen_text(COL_L, 16, COL_TITLE,  "PS2 Dev Channel");
    screen_text(400,   16, COL_LABEL,  "SD2PSX  v2");

    screen_text(COL_L, 38, COL_LABEL,
                "------------------------------------------------");

    /* Stub */
    screen_text(COL_L, ROW(0), COL_LABEL, "Stub:");
    screen_text(COL_R, ROW(0),
                _stub_loaded ? COL_ACK : COL_WARN,
                _stub_loaded ? "Running" : _status);

    /* MC */
    screen_text(COL_L, ROW(1), COL_LABEL, "MC:");
    screen_text(COL_R, ROW(1),
                _mc_ready ? COL_ACK : COL_ERR,
                _mc_ready ? "Ready" : "Error");

    /* Doorbell */
    screen_text(COL_L, ROW(2), COL_LABEL, "Doorbell:");
    if (_doorbell == DEV_DOORBELL_MAGIC) {
        snprintf(_tmp, sizeof(_tmp), "ARMED  %lu KB",
                 (unsigned long)(_elf_size / 1024));
        screen_text(COL_R, ROW(2), COL_ARMED, _tmp);
    } else if (_ack == DEV_ACK_MAGIC) {
        screen_text(COL_R, ROW(2), COL_ACK, "ACK  running");
    } else {
        screen_text(COL_R, ROW(2), COL_IDLE, "Idle");
    }

    /* Last ELF */
    screen_text(COL_L, ROW(3), COL_LABEL, "Last ELF:");
    if (_last_sz > 0) {
        snprintf(_tmp, sizeof(_tmp), "%lu KB @ 0x%08lX",
                 (unsigned long)(_last_sz / 1024),
                 (unsigned long)_last_load);
        screen_text(COL_R, ROW(3), COL_VALUE, _tmp);
    } else {
        screen_text(COL_R, ROW(3), COL_IDLE, "none yet");
    }

    /* ELFs run */
    screen_text(COL_L, ROW(4), COL_LABEL, "ELFs run:");
    snprintf(_tmp, sizeof(_tmp), "%lu", (unsigned long)_elfs_run);
    screen_text(COL_R, ROW(4), COL_VALUE, _tmp);

    /* stdout ring */
    screen_text(COL_L, ROW(5), COL_LABEL, "stdout ring:");
    {
        u32 used = (_stdout_head >= _stdout_tail)
                 ? (_stdout_head - _stdout_tail)
                 : (DEV_STDOUT_RING_SIZE - _stdout_tail + _stdout_head);
        u32 pct  = (used * 100) / DEV_STDOUT_RING_SIZE;
        snprintf(_tmp, sizeof(_tmp), "%lu B  (%lu%%)",
                 (unsigned long)used, (unsigned long)pct);
        screen_text(COL_R, ROW(5),
                    pct > 75 ? COL_WARN : COL_VALUE, _tmp);
    }

    screen_text(COL_L, ROW(6), COL_LABEL,
                "------------------------------------------------");

    /* Hint lines */
    screen_text(COL_L, ROW(7), COL_LABEL,
                "PC: python sd2psx_cli.py elf <prog.elf>");
    screen_text(COL_L, ROW(8), COL_LABEL,
                "PC: python sd2psx_cli.py stdout");

    gs_flip();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    SifInitRpc(0);
    SifLoadFileInit();
    sbv_patch_enable_lmb();

    /* IOP modules — load from ROM so we need no IRX on the SD card */
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:MCMAN",   0, NULL);
    SifLoadModule("rom0:MCSERV",  0, NULL);

    /* GS + DMA */
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    gs_init();

    /* MC */
    if (mc_access_init() == 0) {
        _mc_ready = 1;
        snprintf(_status, sizeof(_status), "MC ready");
    } else {
        snprintf(_status, sizeof(_status), "MC init failed — is SD2PSX inserted?");
    }

    /* First paint before we block on ELF load */
    draw_status_screen();
    graph_wait_vsync();

    /* Load stub */
    if (_mc_ready)
        load_and_start_stub();

    /* Main loop — refresh display, poll header */
    int frame = 0;
    for (;;) {
        graph_wait_vsync();

        if (++frame >= POLL_FRAMES) {
            frame = 0;
            poll_dev_header();
        }

        draw_status_screen();
    }

    return 0;
}
