#!/usr/bin/env python3
"""
ramdump.py — PC-side controller for ramdump_interactive.elf

Usage:
    python ramdump.py 0x00100000 512          # dump 512 bytes from addr
    python ramdump.py 0x00080000 0x1000       # hex lengths work too
    python ramdump.py 0x00100000 256 --raw    # save raw bytes instead of hex
    python ramdump.py --port COM3 0x00100000 512

Assumes ramdump_interactive.elf is already running on the PS2.
Sends the DUMP command via the bridge write, then drains stdout.

Output is printed to terminal. Redirect to capture:
    python ramdump.py 0x00100000 4096 > dump.txt
"""

import sys
import struct
import time
import argparse

sys.path.insert(0, "../..")   # find sd2psx_bridge.py
from sd2psx_bridge import SD2PSXBridge, BridgeError, find_device

# Must match ramdump_interactive.c
CMD_CARD_ADDR = 0x780080    # DEV_CHANNEL_BASE (0x780000) + 0x80
CMD_MAGIC     = 0x44554D50  # 'DUMP'


def send_dump_command(bridge: SD2PSXBridge, addr: int, length: int) -> None:
    payload = struct.pack(">III", addr, length, CMD_MAGIC)
    # Write in chunks if needed (payload is 12 bytes, well within MAX_PAYLOAD)
    bridge.write_block(CMD_CARD_ADDR, payload)


def drain_stdout(bridge: SD2PSXBridge, timeout: float = 30.0) -> str:
    """
    Drain stdout ring until we see 'dump complete' or timeout.
    Returns accumulated output as a string.
    """
    buf = b""
    deadline = time.monotonic() + timeout
    last_data = time.monotonic()

    while time.monotonic() < deadline:
        chunk = bridge.pull_stdout()
        if chunk:
            buf += chunk
            last_data = time.monotonic()
            # Print live
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()
            if b"dump complete" in buf:
                # Drain any tail
                time.sleep(0.2)
                tail = bridge.pull_stdout()
                if tail:
                    sys.stdout.write(tail.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                break
        else:
            # Stop if quiet for 3s after receiving some data
            if buf and (time.monotonic() - last_data) > 3.0:
                break
            time.sleep(0.05)

    return buf.decode("utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser(
        description="EE RAM dumper — controller for ramdump_interactive.elf"
    )
    parser.add_argument("addr",   help="Start address (hex or decimal)")
    parser.add_argument("length", help="Length in bytes (hex or decimal)")
    parser.add_argument("--port", default=None)
    parser.add_argument("--raw",  action="store_true",
                        help="Save raw bytes (strips [HEX] framing) — NYI")
    args = parser.parse_args()

    addr   = int(args.addr,   0)
    length = int(args.length, 0)

    if length == 0 or length > 0x200000:
        print(f"error: length {length} out of range", file=sys.stderr)
        sys.exit(1)

    port = args.port or find_device()
    if not port:
        print("error: no SD2PSX device found", file=sys.stderr)
        sys.exit(1)

    print(f"# EE RAM dump  addr=0x{addr:08X}  len={length}  ({length//1024 or length} {'KB' if length >= 1024 else 'B'})",
          file=sys.stderr)
    print(f"# Port: {port}", file=sys.stderr)
    print(f"# Sending DUMP command...", file=sys.stderr)

    with SD2PSXBridge(port=port) as bridge:
        # Check dev status first
        st = bridge.dev_status()
        if not st["ack_received"]:
            print("warning: no ACK from stub — is ramdump_interactive.elf running?",
                  file=sys.stderr)

        send_dump_command(bridge, addr, length)
        print("# Command sent — draining stdout...\n", file=sys.stderr)
        drain_stdout(bridge, timeout=60.0)


if __name__ == "__main__":
    main()
