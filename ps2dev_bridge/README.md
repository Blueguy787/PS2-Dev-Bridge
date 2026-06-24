# PS2 Dev Channel — SD2PSX Bridge

Single USB-C wire development environment for PS2 homebrew.

```
PC ──USB-C──► SD2PSX (RP2040) ──SIO2──► PS2 EE
               │                          │
               └── PSRAM dev channel ─────┘
                   (top 512KB of 8MB)
```

## How it works

The SD2PSX emulates an 8MB PS2 memory card. The top 512KB of its PSRAM is
reserved as a **dev channel** — a shared memory region that the RP2040 exposes
to both the PC (over USB-C CDC serial) and the PS2 (as readable/writable card
sectors). Normal card data lives below this region and is unaffected.

**Boot sequence:**
1. SD2PSX boots PS2 via FMCB card image (normal operation)
2. FMCB launches `ps2dev_stub.elf` from the SD card
3. Stub initialises the MC driver and enters the doorbell poll loop
4. PC pushes an ELF with `sd2psx_cli.py elf myprogram.elf`
5. Stub detects the doorbell, DMAs the ELF into EE RAM, jumps to it
6. stdout from the ELF comes back to the PC terminal automatically
7. When the ELF returns, stub loops back to step 4

## Dev channel memory map

```
Card offset       Size     Contents
─────────────── ──────── ──────────────────────────────────────
0x000000        7.5 MB   Normal PS2 memory card data (FMCB etc.)
0x780000        512 KB   Dev channel (never flushed to SD)
  +0x0000       4 B      Doorbell   (PC writes 'PS2D' when ELF ready)
  +0x0004       4 B      ELF size
  +0x0008       4 B      ELF load address in EE RAM
  +0x000C       4 B      Flags
  +0x0010       4 B      ACK        (stub writes 'ACK!' after claiming ELF)
  +0x0014       4 B      stdout HEAD (stub write pointer)
  +0x0018       4 B      stdout TAIL (PC drain pointer)
  +0x001C       4 B      reserved
  +0x0020       ~447KB   ELF staging area
  +0x73020      32 KB    stdout ring buffer
```

## Files

```
firmware/
  dev_channel.h       Shared memory map — RP2040 side
  usb_bridge.h        Updated bridge header (v2, dev channel commands)
  usb_bridge.c        Updated bridge implementation

ps2stub/
  dev_channel_ps2.h   Shared memory map — EE side (mirrors dev_channel.h)
  mc_access.h/.c      Sector-aligned MC read/write wrappers
  stdout_ring.h/.c    stdout ring buffer writer
  stub_main.c         Main poll/load/jump loop
  Makefile            Build with ps2sdk ee-gcc toolchain

sd2psx_bridge.py      PC library (push_elf, pull_stdout, dev_status, …)
sd2psx_cli.py         CLI (elf, stdout, dev-status, dev-reset commands)
```

## Setup

### 1. Build the firmware

Integrate `firmware/` into the SD2PSX source tree per `INTEGRATION.c`:

```cmake
cmake -DSD2PSX_WITH_USB_BRIDGE=ON ..
```

Flash to your SD2PSX.

### 2. Build the PS2 stub

```bash
export PS2SDK=/usr/local/ps2dev/ps2sdk
cd ps2stub
make
make pack   # optional — smaller file, faster FMCB launch
```

Copy `ps2dev_stub.elf` (or `_packed.elf`) to the root of your SD2PSX SD card.

### 3. Configure FMCB

Add the stub as a launch item in FMCB configuration:
- Name: `PS2 Dev Stub`
- Path: `mass:ps2dev_stub.elf`  (or `mc0:ps2dev_stub.elf` if using mc storage)

### 4. Install PC tools

```bash
pip install pyserial
```

## Usage

```bash
# Check bridge is alive
python sd2psx_cli.py status

# Push and run an ELF — stdout streams to terminal automatically
python sd2psx_cli.py elf hello.elf

# Push to a specific EE RAM address
python sd2psx_cli.py elf hello.elf --addr 0x00200000

# Push without waiting for stdout (fire and forget)
python sd2psx_cli.py elf hello.elf --no-stdout

# Stream stdout from a running ELF (if you didn't use elf command)
python sd2psx_cli.py stdout

# Query dev channel state
python sd2psx_cli.py dev-status

# Clear the channel (useful if stub crashed mid-transfer)
python sd2psx_cli.py dev-reset
```

## Using the library in your own tools

```python
from sd2psx_bridge import SD2PSXBridge

with SD2PSXBridge() as bridge:
    # Push an ELF and stream stdout to a file
    import sys, threading
    stop = threading.Event()

    def save_stdout(chunk):
        sys.stdout.buffer.write(chunk)

    bridge.push_elf("hello.elf", load_addr=0x00100000)
    if bridge.wait_for_ack(timeout=10.0):
        bridge.stream_stdout(save_stdout, stop_event=stop)
```

## Writing ELFs for the stub

Any standard ps2sdk ELF works. A minimal example:

```c
#include <stdio.h>
#include <kernel.h>

int main(void) {
    printf("Hello from PS2!\n");
    for (int i = 0; i < 10; i++)
        printf("  count: %d\n", i);
    return 0;
}
```

Build normally with ps2sdk. printf goes through the ring automatically
if the stub successfully installed the stdout hook. If not (older sdk),
link `stdout_ring.o` into your ELF and call `stdout_ring_write()` directly.

## Protocol additions (v2)

```
New commands:
  0x10  ELF_PUSH    [addr=chunk_offset][len=chunk_size][data]
  0x11  ELF_COMMIT  [addr=load_addr][len=elf_size]
  0x12  STDOUT_PULL [addr=0][len=max_bytes]  → [data]
  0x13  DEV_STATUS  → [1B doorbell][1B ack][1B busy][1B rsv][2B stdout_pending]
  0x14  DEV_RESET

New response:
  0xA3  NODATA  (STDOUT_PULL when ring is empty)
```

Normal READ/WRITE are clamped at the dev channel boundary — they cannot
reach or corrupt the dev region regardless of address.

## Known limitations

- ELF max size ~447KB (dev channel size minus header and stdout ring)
- Stub uses busy-poll on the MC — there is no interrupt mechanism from
  the RP2040 to the EE, so latency between PC commit and stub wakeup
  is 0–200ms depending on where in the poll cycle the doorbell lands
- stdout hook depends on ps2sdk version; older sdks may require direct
  calls to stdout_ring_write()
- ELFs that never return will hold the stub; dev-reset + power cycle
  the PS2 to recover
