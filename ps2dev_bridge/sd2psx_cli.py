#!/usr/bin/env python3
"""
sd2psx_cli.py — SD2PSX USB bridge + PS2 dev channel CLI

Card commands:
    status
    push <file.mcr|.ps2>
    pull <out.mcr> [--size ps1|ps2]
    watch <dir>

Dev channel commands:
    elf <file.elf> [--addr 0x00100000]   push ELF and run it on PS2
    stdout                                stream PS2 stdout to terminal
    dev-status                            query dev channel state
    dev-reset                             clear channel
"""

import argparse
import sys
import os
import time
import threading

from sd2psx_bridge import (
    SD2PSXBridge, BridgeError, BridgeBusy, ELFTooLarge,
    PS1_CARD_SIZE, PS2_CARD_SIZE, find_device
)


# ── Progress bar ──────────────────────────────────────────────────────────────

def progress_bar(done: int, total: int, width: int = 40):
    pct  = done / total
    fill = int(width * pct)
    bar  = "█" * fill + "░" * (width - fill)
    kb_done  = done  // 1024
    kb_total = total // 1024
    print(f"\r  [{bar}] {kb_done}/{kb_total} KB", end="", flush=True)
    if done >= total:
        print()


# ── Card commands ─────────────────────────────────────────────────────────────

def cmd_status(bridge, args):
    st = bridge.status()
    print(f"  Bridge version : {st['version']}")
    print(f"  Card busy      : {'YES' if st['busy'] else 'no'}")
    print(f"  Port           : {bridge.port}")
    print(f"  ✓ SD2PSX USB bridge is live.")


def cmd_push(bridge, args):
    if not os.path.exists(args.file):
        print(f"  ✗ File not found: {args.file}")
        sys.exit(1)
    size = os.path.getsize(args.file)
    print(f"  Pushing {args.file} ({size // 1024} KB)...")
    try:
        bridge.write_file(args.file, progress=progress_bar)
        print("  ✓ Card updated and flushed to SD.")
    except BridgeBusy:
        print("  ✗ Card busy. Wait for idle.")
        sys.exit(1)


def cmd_pull(bridge, args):
    size = PS2_CARD_SIZE if args.size == "ps2" else PS1_CARD_SIZE
    print(f"  Pulling {size // 1024} KB → {args.file} ...")
    bridge.read_to_file(args.file, size=size, progress=progress_bar)
    print(f"  ✓ Saved.")


def cmd_watch(bridge, args):
    if not os.path.isdir(args.directory):
        print(f"  ✗ Not a directory: {args.directory}")
        sys.exit(1)
    print(f"  Watching {args.directory} for .mcr/.ps2 changes. Ctrl+C to stop.\n")
    seen = {}
    try:
        while True:
            for fname in os.listdir(args.directory):
                if not (fname.endswith(".mcr") or fname.endswith(".ps2")):
                    continue
                fpath = os.path.join(args.directory, fname)
                mtime = os.path.getmtime(fpath)
                if seen.get(fpath) != mtime:
                    seen[fpath] = mtime
                    if seen.get(fpath) is not None:
                        print(f"  ↑ {fname}")
                        try:
                            bridge.write_file(fpath, progress=progress_bar)
                            print(f"  ✓ Pushed.")
                        except BridgeBusy:
                            print(f"  ✗ Busy, skipping.")
                        except BridgeError as e:
                            print(f"  ✗ {e}")
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n  Stopped.")


# ── Dev channel commands ──────────────────────────────────────────────────────

def cmd_elf(bridge, args):
    """Push an ELF to the PS2 dev channel and optionally stream stdout."""
    path = args.file
    if not os.path.exists(path):
        print(f"  ✗ File not found: {path}")
        sys.exit(1)

    load_addr = int(args.addr, 0) if args.addr else 0x00100000
    size = os.path.getsize(path)

    print(f"  Pushing ELF: {path} ({size // 1024} KB) → EE RAM 0x{load_addr:08X}")
    try:
        bridge.push_elf(path, load_addr=load_addr, progress=progress_bar)
    except ELFTooLarge as e:
        print(f"  ✗ {e}")
        sys.exit(1)

    print(f"  ✓ ELF staged. Doorbell armed — waiting for PS2 stub to ACK...")
    if bridge.wait_for_ack(timeout=15.0):
        print(f"  ✓ PS2 stub acknowledged — ELF is running.")
    else:
        print(f"  ⚠ No ACK within 15s. Is the stub running?")

    if not args.no_stdout:
        print(f"  ── stdout ─────────────────────────────")
        stop = threading.Event()
        try:
            bridge.stream_stdout(
                callback=lambda b: sys.stdout.buffer.write(b) or sys.stdout.buffer.flush(),
                stop_event=stop,
            )
        except KeyboardInterrupt:
            stop.set()
            print(f"\n  ── stdout stream ended ────────────────")


def cmd_stdout(bridge, args):
    """Stream PS2 stdout to terminal."""
    print("  ── PS2 stdout (Ctrl+C to stop) ────────")
    stop = threading.Event()
    try:
        bridge.stream_stdout(
            callback=lambda b: sys.stdout.buffer.write(b) or sys.stdout.buffer.flush(),
            stop_event=stop,
        )
    except KeyboardInterrupt:
        stop.set()
        print("\n  ── ended ──────────────────────────────")


def cmd_dev_status(bridge, args):
    st = bridge.dev_status()
    print(f"  Doorbell armed  : {'YES — ELF waiting' if st['doorbell_armed'] else 'no'}")
    print(f"  PS2 ACK         : {'YES — stub running' if st['ack_received'] else 'no'}")
    print(f"  Card busy       : {'YES' if st['card_busy'] else 'no'}")
    print(f"  Stdout pending  : {st['stdout_pending']} bytes")


def cmd_dev_reset(bridge, args):
    bridge.dev_reset()
    print("  ✓ Dev channel cleared.")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        prog="sd2psx_cli",
        description="SD2PSX USB bridge — card + PS2 dev channel",
    )
    parser.add_argument("--port", "-p", default=None)

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("status")

    p = sub.add_parser("push")
    p.add_argument("file")

    p = sub.add_parser("pull")
    p.add_argument("file")
    p.add_argument("--size", choices=["ps1", "ps2"], default="ps1")

    p = sub.add_parser("watch")
    p.add_argument("directory")

    p = sub.add_parser("elf", help="Push ELF to PS2 and run it")
    p.add_argument("file")
    p.add_argument("--addr", default=None, help="EE RAM load address (default 0x00100000)")
    p.add_argument("--no-stdout", action="store_true", help="Don't stream stdout after launch")

    sub.add_parser("stdout", help="Stream PS2 stdout")
    sub.add_parser("dev-status", help="Query dev channel state")
    sub.add_parser("dev-reset",  help="Clear dev channel")

    args = parser.parse_args()

    print()
    print("  SD2PSX USB Bridge")
    print("  ─────────────────")

    port = args.port or find_device()
    if not port:
        print("  ✗ No SD2PSX device found.")
        sys.exit(1)
    print(f"  Port: {port}")

    try:
        with SD2PSXBridge(port=port) as bridge:
            dispatch = {
                "status":     cmd_status,
                "push":       cmd_push,
                "pull":       cmd_pull,
                "watch":      cmd_watch,
                "elf":        cmd_elf,
                "stdout":     cmd_stdout,
                "dev-status": cmd_dev_status,
                "dev-reset":  cmd_dev_reset,
            }
            dispatch[args.command](bridge, args)
    except BridgeError as e:
        print(f"  ✗ {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
