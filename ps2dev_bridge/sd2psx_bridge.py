"""
sd2psx_bridge.py
Core protocol library for the SD2PSX USB bridge + PS2 dev channel.

New in v2:
  push_elf(path_or_bytes, load_addr)  — stream ELF to PS2 dev channel
  pull_stdout(max_bytes)              — drain PS2 stdout ring
  dev_status()                        — query dev channel state
  dev_reset()                         — clear doorbell + stdout ring
  stream_stdout(callback)             — blocking stdout tail loop
"""

import serial
import serial.tools.list_ports
import struct
import time
from enum import IntEnum
from typing import Optional, Callable, Union


# ── Protocol constants ────────────────────────────────────────────────────────

MAGIC   = 0x5D
VERSION = 0x02

class Cmd(IntEnum):
    READ         = 0x01
    WRITE        = 0x02
    FLUSH        = 0x03
    STATUS       = 0x04
    ELF_PUSH     = 0x10
    ELF_COMMIT   = 0x11
    STDOUT_PULL  = 0x12
    DEV_STATUS   = 0x13
    DEV_RESET    = 0x14

class Rsp(IntEnum):
    OK     = 0xA0
    ERR    = 0xA1
    BUSY   = 0xA2
    NODATA = 0xA3

MAX_PAYLOAD      = 512
PS1_CARD_SIZE    = 128 * 1024
PS2_CARD_SIZE    = 8 * 1024 * 1024
DEV_CHANNEL_SIZE = 512 * 1024
DEV_CHANNEL_BASE = PS2_CARD_SIZE - DEV_CHANNEL_SIZE
DEV_ELF_MAX_SIZE = DEV_CHANNEL_SIZE - 0x0020 - (32 * 1024)  # matches firmware


# ── Exceptions ────────────────────────────────────────────────────────────────

class BridgeError(Exception):
    pass

class BridgeBusy(BridgeError):
    pass

class BridgeChecksumError(BridgeError):
    pass

class BridgeTimeout(BridgeError):
    pass

class ELFTooLarge(BridgeError):
    pass


# ── Low-level framing ─────────────────────────────────────────────────────────

def _xor_checksum(data: bytes) -> int:
    cs = 0
    for b in data:
        cs ^= b
    return cs


def _build_packet(cmd: Cmd, addr: int, data: bytes = b"") -> bytes:
    length = len(data)
    header = bytes([
        MAGIC,
        int(cmd),
        (addr >> 16) & 0xFF,
        (addr >>  8) & 0xFF,
         addr        & 0xFF,
        (length >> 8) & 0xFF,
         length       & 0xFF,
    ])
    body = header + data
    return body + bytes([_xor_checksum(body)])


def _parse_response(raw: bytes, expected_data_len: int = 0) -> bytes:
    min_len = 2
    if expected_data_len:
        min_len += expected_data_len

    if len(raw) < min_len:
        raise BridgeError(f"Response too short: {len(raw)} bytes")

    rsp = raw[0]
    if rsp == Rsp.BUSY:
        raise BridgeBusy("Card is busy with console transaction")
    if rsp == Rsp.ERR:
        raise BridgeError("Device returned ERR")
    if rsp == Rsp.NODATA:
        return b""   # stdout ring empty — not an error
    if rsp != Rsp.OK:
        raise BridgeError(f"Unknown response byte: {rsp:#04x}")

    payload = raw[1:-1]
    received_cs = raw[-1]
    expected_cs = _xor_checksum(raw[:-1])
    if received_cs != expected_cs:
        raise BridgeChecksumError(
            f"Checksum mismatch: expected {expected_cs:#04x}, got {received_cs:#04x}"
        )
    return payload


# ── Device discovery ──────────────────────────────────────────────────────────

def find_device() -> Optional[str]:
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x2E8A:
            return port.device
    return None


# ── Bridge class ──────────────────────────────────────────────────────────────

class SD2PSXBridge:
    """
    High-level interface to the SD2PSX USB bridge with PS2 dev channel.

    Card image operations (original):
        write_card(data)        — push full card image
        read_card(size)         — pull full card image
        flush()                 — commit PSRAM to SD

    Dev channel operations (new):
        push_elf(data, addr)    — stream ELF and ring doorbell
        pull_stdout(n)          — drain stdout ring
        dev_status()            — query dev channel state
        dev_reset()             — clear channel
        stream_stdout(cb)       — blocking stdout loop
    """

    def __init__(self, port: Optional[str] = None, timeout: float = 3.0):
        self.port    = port
        self.timeout = timeout
        self._serial: Optional[serial.Serial] = None

    # ── Connection ────────────────────────────────────────────────────────────

    def connect(self, port: Optional[str] = None) -> str:
        if port:
            self.port = port
        if not self.port:
            self.port = find_device()
        if not self.port:
            raise BridgeError("No SD2PSX bridge device found. Is it plugged in?")
        self._serial = serial.Serial(self.port, baudrate=115200, timeout=self.timeout)
        time.sleep(0.1)
        self.status()   # verify comms
        return self.port

    def disconnect(self):
        if self._serial and self._serial.is_open:
            self._serial.close()
        self._serial = None

    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.disconnect()

    # ── Raw transaction ───────────────────────────────────────────────────────

    def _transact(self, cmd: Cmd, addr: int = 0,
                  data: bytes = b"", expected_data_len: int = 0) -> bytes:
        if not self.is_connected():
            raise BridgeError("Not connected")
        packet = _build_packet(cmd, addr, data)
        self._serial.write(packet)
        self._serial.flush()
        rsp_len = 2 + expected_data_len
        raw = self._serial.read(rsp_len)
        if len(raw) < 2:
            raise BridgeTimeout("No response from device")
        return _parse_response(raw, expected_data_len)

    def _transact_variable(self, cmd: Cmd, addr: int = 0,
                           data: bytes = b"", max_response: int = MAX_PAYLOAD) -> bytes:
        """
        Like _transact but reads a variable-length response.
        Used for STDOUT_PULL and DEV_STATUS where response length isn't fixed.
        """
        if not self.is_connected():
            raise BridgeError("Not connected")
        packet = _build_packet(cmd, addr, data)
        self._serial.write(packet)
        self._serial.flush()
        # Read rsp byte first
        rsp_byte = self._serial.read(1)
        if not rsp_byte:
            raise BridgeTimeout("No response from device")
        rsp = rsp_byte[0]
        if rsp == Rsp.BUSY:
            raise BridgeBusy("Card busy")
        if rsp == Rsp.ERR:
            raise BridgeError("Device returned ERR")
        if rsp == Rsp.NODATA:
            return b""
        if rsp != Rsp.OK:
            raise BridgeError(f"Unknown response: {rsp:#04x}")
        # Read payload + checksum
        payload_and_cs = self._serial.read(max_response + 1)
        if not payload_and_cs:
            return b""
        payload  = payload_and_cs[:-1]
        recv_cs  = payload_and_cs[-1]
        frame    = bytes([rsp]) + payload
        expect_cs = _xor_checksum(frame)
        if recv_cs != expect_cs:
            raise BridgeChecksumError("Checksum mismatch on variable response")
        return payload

    # ── Original protocol commands ────────────────────────────────────────────

    def status(self) -> dict:
        payload = self._transact(Cmd.STATUS, expected_data_len=2)
        return {"version": payload[0], "busy": bool(payload[1])}

    def read_block(self, addr: int, length: int) -> bytes:
        if length > MAX_PAYLOAD:
            raise ValueError(f"length {length} exceeds MAX_PAYLOAD")
        return self._transact(Cmd.READ, addr=addr, expected_data_len=length)

    def write_block(self, addr: int, data: bytes) -> None:
        if len(data) > MAX_PAYLOAD:
            raise ValueError(f"data too large for one block")
        self._transact(Cmd.WRITE, addr=addr, data=data)

    def flush(self) -> None:
        self._transact(Cmd.FLUSH)

    def read_card(self, size: int = PS1_CARD_SIZE,
                  progress: Optional[Callable[[int, int], None]] = None) -> bytes:
        result = bytearray()
        addr = 0
        while addr < size:
            chunk = min(MAX_PAYLOAD, size - addr)
            result.extend(self.read_block(addr, chunk))
            addr += chunk
            if progress:
                progress(addr, size)
        return bytes(result)

    def write_card(self, data: bytes,
                   progress: Optional[Callable[[int, int], None]] = None,
                   flush_after: bool = True) -> None:
        size = len(data)
        addr = 0
        while addr < size:
            chunk = min(MAX_PAYLOAD, size - addr)
            self.write_block(addr, data[addr:addr + chunk])
            addr += chunk
            if progress:
                progress(addr, size)
        if flush_after:
            self.flush()

    def write_file(self, path: str,
                   progress: Optional[Callable[[int, int], None]] = None) -> None:
        with open(path, "rb") as f:
            data = f.read()
        self.write_card(data, progress=progress)

    def read_to_file(self, path: str, size: int = PS1_CARD_SIZE,
                     progress: Optional[Callable[[int, int], None]] = None) -> None:
        data = self.read_card(size=size, progress=progress)
        with open(path, "wb") as f:
            f.write(data)

    # ── Dev channel — ELF upload ──────────────────────────────────────────────

    def push_elf(self,
                 elf: Union[str, bytes],
                 load_addr: int = 0x00100000,
                 progress: Optional[Callable[[int, int], None]] = None) -> None:
        """
        Stream an ELF into the PS2 dev channel and ring the doorbell.

        elf       — path string or raw bytes
        load_addr — EE RAM destination address
        progress  — optional callback(bytes_done, total)

        The PS2 stub will DMA the ELF into EE RAM at load_addr and jump to it.
        Call stream_stdout() or poll pull_stdout() after to get stdout back.
        """
        if isinstance(elf, str):
            with open(elf, "rb") as f:
                data = f.read()
        else:
            data = elf

        size = len(data)
        if size > DEV_ELF_MAX_SIZE:
            raise ELFTooLarge(
                f"ELF is {size} bytes, max is {DEV_ELF_MAX_SIZE} bytes"
            )

        # Stream in MAX_PAYLOAD chunks using ELF_PUSH
        offset = 0
        while offset < size:
            chunk = min(MAX_PAYLOAD, size - offset)
            self._transact(Cmd.ELF_PUSH, addr=offset, data=data[offset:offset + chunk])
            offset += chunk
            if progress:
                progress(offset, size)

        # Commit — addr field carries load_addr, len field carries elf size
        # Protocol packs addr as 3 bytes and len as 2 bytes.
        # ELF size may exceed uint16 for large ELFs — use addr field for size,
        # len=0 signals "use addr as size" to the firmware.
        # Simpler: firmware ELF_COMMIT uses addr=load_addr, len=size (capped to 512KB)
        # which fits in uint16 since DEV_ELF_MAX_SIZE < 512KB.
        self._transact(Cmd.ELF_COMMIT, addr=load_addr, data=b"",
                       expected_data_len=0)
        # Note: ELF_COMMIT has no data payload; len in packet header = 0.
        # We communicate elf_size via a separate prior WRITE to DEV_ADDR_ELF_SIZE,
        # but firmware ELF_COMMIT handler already has the size from our chunks.
        # Actually firmware uses the `len` field of ELF_COMMIT for elf_size —
        # re-send with len encoded in the data workaround below.
        # See _commit_elf() for the correct framing.

    def _commit_elf(self, load_addr: int, elf_size: int) -> None:
        """
        Internal: ring the doorbell with correct size framing.
        ELF_COMMIT: addr=load_addr, len=elf_size (firmware reads len field).
        We abuse the `data` parameter to force len into the packet header.
        Instead, build the packet manually with elf_size in the len field.
        """
        if not self.is_connected():
            raise BridgeError("Not connected")
        # Build packet with elf_size in the len field, no data payload
        header = bytes([
            MAGIC,
            int(Cmd.ELF_COMMIT),
            (load_addr >> 16) & 0xFF,
            (load_addr >>  8) & 0xFF,
             load_addr        & 0xFF,
            (elf_size  >>  8) & 0xFF,
             elf_size         & 0xFF,
        ])
        packet = header + bytes([_xor_checksum(header)])
        self._serial.write(packet)
        self._serial.flush()
        raw = self._serial.read(2)
        if len(raw) < 2:
            raise BridgeTimeout("No response to ELF_COMMIT")
        _parse_response(raw, 0)

    def push_elf(self,
                 elf: Union[str, bytes],
                 load_addr: int = 0x00100000,
                 progress: Optional[Callable[[int, int], None]] = None) -> None:
        """
        Stream an ELF into the PS2 dev channel and ring the doorbell.
        """
        if isinstance(elf, str):
            with open(elf, "rb") as f:
                data = f.read()
        else:
            data = elf

        size = len(data)
        if size > DEV_ELF_MAX_SIZE:
            raise ELFTooLarge(f"ELF {size}B > max {DEV_ELF_MAX_SIZE}B")

        offset = 0
        while offset < size:
            chunk = min(MAX_PAYLOAD, size - offset)
            self._transact(Cmd.ELF_PUSH, addr=offset, data=data[offset:offset + chunk])
            offset += chunk
            if progress:
                progress(offset, size)

        self._commit_elf(load_addr, size)

    # ── Dev channel — stdout ──────────────────────────────────────────────────

    def pull_stdout(self, max_bytes: int = MAX_PAYLOAD) -> bytes:
        """
        Pull available bytes from the PS2 stdout ring.
        Returns b"" if nothing is available.
        """
        return self._transact_variable(
            Cmd.STDOUT_PULL, addr=0,
            data=b"", max_response=min(max_bytes, MAX_PAYLOAD)
        )

    def stream_stdout(self,
                      callback: Callable[[bytes], None],
                      poll_interval: float = 0.05,
                      stop_event=None) -> None:
        """
        Blocking stdout drain loop. Calls callback(chunk) as bytes arrive.

        stop_event — optional threading.Event; loop exits when set.

        Example:
            import sys, threading
            stop = threading.Event()
            bridge.stream_stdout(lambda b: sys.stdout.buffer.write(b), stop_event=stop)
        """
        while True:
            if stop_event and stop_event.is_set():
                break
            chunk = self.pull_stdout()
            if chunk:
                callback(chunk)
            else:
                time.sleep(poll_interval)

    # ── Dev channel — status / reset ──────────────────────────────────────────

    def dev_status(self) -> dict:
        """
        Query dev channel state.
        Returns dict: doorbell_armed, ack_received, card_busy, stdout_pending
        """
        raw = self._transact_variable(Cmd.DEV_STATUS, max_response=6)
        if len(raw) < 6:
            raise BridgeError(f"DEV_STATUS response too short: {len(raw)}")
        return {
            "doorbell_armed":  bool(raw[0]),
            "ack_received":    bool(raw[1]),
            "card_busy":       bool(raw[2]),
            "stdout_pending":  (raw[4] << 8) | raw[5],
        }

    def dev_reset(self) -> None:
        """Clear dev channel: doorbell, ACK, stdout ring."""
        self._transact(Cmd.DEV_RESET)

    def wait_for_ack(self, timeout: float = 10.0,
                     poll_interval: float = 0.1) -> bool:
        """
        Poll DEV_STATUS until PS2 stub acknowledges the ELF (ack_received=True).
        Returns True if ack received within timeout, False otherwise.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            st = self.dev_status()
            if st["ack_received"]:
                return True
            time.sleep(poll_interval)
        return False
