"""Receives head tracking from Bridgehead (OSC yaw/pitch/roll in degrees) and sends it as
MIDI pitch bend, so Gig Performer can turn APL Virtuoso's Yaw/Pitch/Roll knobs.

Yaw = pitch bend channel 1, Pitch = channel 2, Roll = channel 3 (14 bit each).
"""
from __future__ import annotations

import socket
import struct
import threading
import time

DEFAULT_RANGES = ((-180.0, 180.0), (-90.0, 90.0), (-90.0, 90.0))  # yaw, pitch, roll in degrees


# ---------- OSC ----------
def _read_string(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "replace"), (end + 4) & ~3


def _osc_string(text: str) -> bytes:
    raw = text.encode("utf-8") + b"\0"
    return raw + b"\0" * ((-len(raw)) % 4)


def split_packet(data: bytes) -> list[bytes]:
    """Split OSC bundles into individual messages."""
    if not data.startswith(b"#bundle\0"):
        return [data]
    messages, pos = [], 16
    while pos + 4 <= len(data):
        (size,) = struct.unpack_from(">i", data, pos)
        if size <= 0:
            break
        messages += split_packet(data[pos + 4 : pos + 4 + size])
        pos += 4 + size
    return messages


def parse_floats(data: bytes) -> tuple[str, list[float]] | None:
    """Address and values if the message consists only of floats."""
    try:
        address, pos = _read_string(data, 0)
        tags, pos = _read_string(data, pos)
        tags = tags[1:]
        if not tags or set(tags) - {"f", "d"}:
            return None
        values = []
        for tag in tags:
            fmt, size = (">f", 4) if tag == "f" else (">d", 8)
            values.append(struct.unpack_from(fmt, data, pos)[0])
            pos += size
        return address, values
    except (ValueError, struct.error):
        return None


def build_message(address: str, values: list[float]) -> bytes:
    return _osc_string(address) + _osc_string("," + "f" * len(values)) + struct.pack(
        ">" + "f" * len(values), *values
    )


# ---------- MIDI ----------
def angle_to_14bit(angle: float, low: float, high: float) -> int:
    value = (angle - low) / (high - low)
    return max(0, min(16383, int(round(value * 16383))))


def pitch_bend(channel: int, value: int) -> list[int]:
    """MIDI pitch bend (14 bit) on channel 0-15."""
    return [0xE0 | (channel & 0x0F), value & 0x7F, (value >> 7) & 0x7F]


class Engine:
    """Receives OSC in its own thread and sends every yaw/pitch/roll update immediately as MIDI."""

    INPUT_TIMEOUT = 1.0  # seconds without input -> "waiting for Bridgehead"

    def __init__(self):
        self.midi_send = None  # callable(list[int]) set by the app
        self.invert = [False, False, False]  # yaw, pitch, roll
        self.ranges = list(DEFAULT_RANGES)
        self.received = 0
        self.last_input = 0.0
        self.last_angles_time = 0.0
        self.addresses: list[str] = []
        self._last_midi: list[int | None] = [None, None, None]
        self._rx: socket.socket | None = None
        self._running = False
        self._thread: threading.Thread | None = None

    def start(self, in_port: int) -> None:
        """Raises OSError if the port is in use."""
        self.stop()
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rx.bind(("127.0.0.1", in_port))
        except OSError:
            rx.close()
            raise
        rx.settimeout(0.25)
        self._rx = rx
        self.addresses = []
        self.reset_midi_cache()
        self._running = True
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None
        if self._rx is not None:
            self._rx.close()
            self._rx = None

    def reset_midi_cache(self) -> None:
        self._last_midi = [None, None, None]

    @property
    def running(self) -> bool:
        return self._running

    @property
    def receiving(self) -> bool:
        return self._running and time.monotonic() - self.last_input < self.INPUT_TIMEOUT

    @property
    def receiving_angles(self) -> bool:
        return self._running and time.monotonic() - self.last_angles_time < self.INPUT_TIMEOUT

    def _receive_loop(self) -> None:
        rx = self._rx
        while self._running and rx is not None:
            try:
                data, _ = rx.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                # Windows sometimes reports "connection reset" on UDP -> continue
                if not self._running:
                    break
                continue
            for msg in split_packet(data):
                parsed = parse_floats(msg)
                if parsed is None:
                    continue
                address, values = parsed
                now = time.monotonic()
                self.last_input = now
                self.received += 1
                if address not in self.addresses:
                    self.addresses.append(address)
                if len(values) == 3:  # yaw, pitch, roll
                    self.last_angles_time = now
                    self.send_angles(values)

    def send_angles(self, angles: list[float]) -> None:
        send = self.midi_send
        if send is None:
            return
        for axis in range(3):
            angle = -angles[axis] if self.invert[axis] else angles[axis]
            low, high = self.ranges[axis]
            value = angle_to_14bit(angle, low, high)
            if value != self._last_midi[axis]:
                self._last_midi[axis] = value
                try:
                    send(pitch_bend(axis, value))
                except Exception:
                    self._last_midi[axis] = None
