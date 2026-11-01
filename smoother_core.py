"""Smoothing engine: receives OSC head tracking, smooths it and forwards it.

- messages with 4 numbers      -> smoothed as a quaternion (e.g. /Virtuoso/quat)
- messages with 1 or 3 numbers -> smoothed as angles in degrees
- everything else              -> forwarded unchanged
"""
from __future__ import annotations

import math
import socket
import struct
import threading
import time


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


def parse_numbers(data: bytes) -> tuple[str, list[float]] | None:
    """Address and values if the message consists only of 1, 3 or 4 floats."""
    try:
        address, pos = _read_string(data, 0)
        tags, pos = _read_string(data, pos)
        tags = tags[1:]
        if len(tags) not in (1, 3, 4) or set(tags) - {"f", "d"}:
            return None
        values = []
        for tag in tags:
            fmt, size = (">f", 4) if tag == "f" else (">d", 8)
            values.append(struct.unpack_from(fmt, data, pos)[0])
            pos += size
        if not all(math.isfinite(v) for v in values):
            return None
        return address, values
    except (ValueError, struct.error):
        return None


def describe_message(data: bytes) -> tuple[str, str, list]:
    """Address, type tags and decoded arguments of any OSC message (for recordings)."""
    try:
        address, pos = _read_string(data, 0)
        tags, pos = _read_string(data, pos)
        args: list = []
        for tag in tags[1:]:
            if tag in "fi":
                args.append(struct.unpack_from(">f" if tag == "f" else ">i", data, pos)[0])
                pos += 4
            elif tag in "dh":
                args.append(struct.unpack_from(">d" if tag == "d" else ">q", data, pos)[0])
                pos += 8
            elif tag == "s":
                text, pos = _read_string(data, pos)
                args.append(text)
            elif tag in "TFN":
                args.append({"T": True, "F": False, "N": None}[tag])
            else:
                args.append(f"<{tag}>")
                break
        return address, tags, args
    except (ValueError, struct.error, IndexError):
        return "<unparsed>", "", [data.hex()]


def build_message(address: str, values: list[float]) -> bytes:
    return _osc_string(address) + _osc_string("," + "f" * len(values)) + struct.pack(
        ">" + "f" * len(values), *values
    )


# ---------- Smoothing ----------
def _normalize(q: list[float]) -> list[float]:
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return [v / n for v in q]


def step_quat(current: list[float], target: list[float], t: float) -> list[float]:
    """Component-wise with sign alignment + normalisation (nlerp).
    Works regardless of the order in which w, x, y, z are sent."""
    if sum(a * b for a, b in zip(current, target)) < 0:
        target = [-v for v in target]
    return _normalize([a + (b - a) * t for a, b in zip(current, target)])


def step_angles(current: list[float], target: list[float], t: float) -> list[float]:
    """Shortest path, so 179 -> -179 does not spin all the way round."""
    out = []
    for a, b in zip(current, target):
        diff = (b - a + 180.0) % 360.0 - 180.0
        v = a + diff * t
        out.append((v + 180.0) % 360.0 - 180.0 if abs(b) <= 180.0 else v)
    return out


class Channel:
    def __init__(self, values: list[float]):
        self.quat = len(values) == 4
        self.target = _normalize(values) if self.quat else list(values)
        self.current = self.target
        self.last_input = time.monotonic()


class Engine:
    """Runs in its own threads. All settings can be changed live."""

    INPUT_TIMEOUT = 1.0  # seconds without input -> stop sending

    def __init__(self):
        self.smooth_ms = 40.0
        self.rate_hz = 1000.0  # one update per millisecond -> tiny steps per audio block
        self.bypass = False
        self.dest = ("127.0.0.1", 8001)
        self.received = 0
        self.last_input = 0.0
        self.addresses: list[str] = []
        self._lock = threading.Lock()
        self._channels: dict[str, Channel] = {}
        self._rx: socket.socket | None = None
        self._tx: socket.socket | None = None
        self._running = False
        self._threads: list[threading.Thread] = []
        self._record_until = 0.0
        self._record_start = 0.0
        self._recorded: list[tuple[float, str, str, list]] = []

    # --- Control ---
    def start(self, in_port: int, out_port: int, out_host: str = "127.0.0.1") -> None:
        """Raises OSError if the input port is in use."""
        self.stop()
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rx.bind(("127.0.0.1", in_port))
        except OSError:
            rx.close()
            raise
        rx.settimeout(0.25)
        self._rx = rx
        self._tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.dest = (out_host, out_port)
        with self._lock:
            self._channels.clear()
            self.addresses = []
        self._running = True
        self._threads = [
            threading.Thread(target=self._receive_loop, daemon=True),
            threading.Thread(target=self._output_loop, daemon=True),
        ]
        for th in self._threads:
            th.start()

    def stop(self) -> None:
        self._running = False
        for th in self._threads:
            th.join(timeout=1.0)
        self._threads = []
        for sock in (self._rx, self._tx):
            if sock is not None:
                sock.close()
        self._rx = self._tx = None

    # --- Recording (diagnostics) ---
    def start_recording(self, seconds: float) -> None:
        with self._lock:
            self._recorded = []
            self._record_start = time.monotonic()
            self._record_until = self._record_start + seconds

    @property
    def recording(self) -> bool:
        return time.monotonic() < self._record_until

    def recording_remaining(self) -> float:
        return max(0.0, self._record_until - time.monotonic())

    def take_recording(self) -> list[tuple[float, str, str, list]]:
        with self._lock:
            rows, self._recorded = self._recorded, []
        return rows

    @property
    def running(self) -> bool:
        return self._running

    @property
    def receiving(self) -> bool:
        return self._running and time.monotonic() - self.last_input < self.INPUT_TIMEOUT

    # --- Threads ---
    def _send(self, data: bytes) -> None:
        tx = self._tx
        if tx is None:
            return
        try:
            tx.sendto(data, self.dest)
        except OSError:
            pass  # destination unreachable (e.g. Virtuoso not open yet) -> ignore

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
            messages = split_packet(data)
            now = time.monotonic()
            if now < self._record_until:
                with self._lock:
                    for msg in messages:
                        self._recorded.append((now - self._record_start, *describe_message(msg)))
            for msg in messages:
                parsed = parse_numbers(msg)
                if parsed is None or self.bypass:
                    self._send(msg)
                    if parsed is not None:
                        self._note_input(*parsed, passthrough=True)
                    continue
                self._note_input(*parsed)

    def _note_input(self, address: str, values: list[float], passthrough: bool = False) -> None:
        with self._lock:
            ch = self._channels.get(address)
            if ch is None or len(values) != len(ch.target):
                ch = self._channels[address] = Channel(values)
                if address not in self.addresses:
                    self.addresses.append(address)
            ch.target = _normalize(values) if ch.quat else list(values)
            if passthrough:
                ch.current = ch.target
            ch.last_input = self.last_input = time.monotonic()
            self.received += 1

    def _output_loop(self) -> None:
        last = next_tick = time.monotonic()
        while self._running:
            period = 1.0 / max(self.rate_hz, 10.0)
            next_tick += period
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.monotonic()
            now = time.monotonic()
            dt, last = now - last, now
            if self.bypass:
                continue
            tau = max(self.smooth_ms, 0.0) / 1000.0
            t = 1.0 if tau == 0 else 1.0 - math.exp(-dt / tau)
            out = []
            with self._lock:
                for address, ch in self._channels.items():
                    if now - ch.last_input > self.INPUT_TIMEOUT:
                        continue
                    step = step_quat if ch.quat else step_angles
                    ch.current = step(ch.current, ch.target, t)
                    out.append(build_message(address, ch.current))
            for msg in out:
                self._send(msg)
