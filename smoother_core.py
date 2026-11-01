"""Glaettungs-Engine: empfaengt OSC-Head-Tracking, glaettet es und sendet es weiter.

- Nachrichten mit 4 Zahlen        -> als Quaternion geglaettet (z. B. /Virtuoso/quat)
- Nachrichten mit 1 oder 3 Zahlen -> als Winkel in Grad geglaettet
- alles andere                    -> unveraendert weitergereicht
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
    """Zerlegt OSC-Bundles in einzelne Nachrichten."""
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
    """Adresse und Werte, wenn die Nachricht nur aus 1, 3 oder 4 Fliesskommazahlen besteht."""
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


def build_message(address: str, values: list[float]) -> bytes:
    return _osc_string(address) + _osc_string("," + "f" * len(values)) + struct.pack(
        ">" + "f" * len(values), *values
    )


# ---------- Glaettung ----------
def _normalize(q: list[float]) -> list[float]:
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return [v / n for v in q]


def step_quat(current: list[float], target: list[float], t: float) -> list[float]:
    """Komponentenweise mit Vorzeichenabgleich + Normierung (nlerp).
    Unabhaengig davon, in welcher Reihenfolge w, x, y, z gesendet werden."""
    if sum(a * b for a, b in zip(current, target)) < 0:
        target = [-v for v in target]
    return _normalize([a + (b - a) * t for a, b in zip(current, target)])


def step_angles(current: list[float], target: list[float], t: float) -> list[float]:
    """Kuerzester Weg, damit 179 -> -179 nicht einmal herum dreht."""
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
    """Laeuft in eigenen Threads. Alle Einstellungen koennen live geaendert werden."""

    INPUT_TIMEOUT = 1.0  # s ohne Eingang -> nichts mehr senden

    def __init__(self):
        self.smooth_ms = 40.0
        self.rate_hz = 200.0
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

    # --- Steuerung ---
    def start(self, in_port: int, out_port: int, out_host: str = "127.0.0.1") -> None:
        """Wirft OSError, wenn der Eingangsport belegt ist."""
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
            pass  # Ziel nicht erreichbar (z. B. Virtuoso noch nicht offen) -> ignorieren

    def _receive_loop(self) -> None:
        rx = self._rx
        while self._running and rx is not None:
            try:
                data, _ = rx.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                # Windows meldet bei UDP manchmal "Verbindung zurueckgesetzt" -> weiter
                if not self._running:
                    break
                continue
            for msg in split_packet(data):
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
