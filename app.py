"""Head Tracking Bridge - turns APL Virtuoso's Yaw/Pitch/Roll knobs with Bridgehead head tracking,
via MIDI and Gig Performer (Windows & macOS)."""
from __future__ import annotations

import json
import math
import os
import sys
import tempfile
import time
import webbrowser
from pathlib import Path
from tkinter import messagebox

import customtkinter as ctk

from tracker_core import DEFAULT_RANGES, Engine, angle_to_14bit, pitch_bend

APP_NAME = "Head Tracking Bridge"
IS_MAC = sys.platform == "darwin"
IS_WIN = sys.platform == "win32"
DEFAULTS = {"in_port": 8000, "midi_port": "", "invert_yaw": False, "invert_pitch": False, "invert_roll": False}
AXES = ("yaw", "pitch", "roll")
MAC_VIRTUAL_PORT = "Head Tracking Bridge (virtual port)"
SETUP_GUIDE_URL = "https://github.com/lumen-tools/Head-Tracking-Bridge#setup"
# Never offer these: the Supperware tracker's own port and the built-in Windows synth.
HIDDEN_PORT_WORDS = ("head tracker", "wavetable")
VIRTUAL_PORT_WORDS = ("loopmidi", "iac", "virtual", "head tracking bridge")
LEARN_SECONDS = 6
LEARN_WIGGLE_DEGREES = 20.0
PORT_APPLY_DELAY_MS = 700
BUSY_RETRY_SECONDS = 2.0

MUTED = ("gray38", "gray62")
GREEN, AMBER, RED, GREY = "#1f9d55", "#d4a017", "#d64545", "#8a8a8a"

ctk.set_appearance_mode("system")
ctk.set_default_color_theme("blue")


# ---------- Settings ----------
def settings_path() -> Path:
    if IS_MAC:
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("APPDATA", Path.home()))
    return base / APP_NAME / "settings.json"


def load_settings() -> dict:
    settings = dict(DEFAULTS)
    try:
        stored = json.loads(settings_path().read_text(encoding="utf-8"))
        for key, default in DEFAULTS.items():
            if isinstance(stored.get(key), type(default)):
                settings[key] = stored[key]
    except (OSError, ValueError, TypeError):
        pass
    return settings


def save_settings(settings: dict) -> None:
    try:
        path = settings_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(settings, indent=2), encoding="utf-8")
    except OSError:
        pass


def make_icon_image(size: int = 64):
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / 64
    d.ellipse((2 * s, 2 * s, 62 * s, 62 * s), fill=(31, 106, 165, 255))
    # stylised smooth wave
    pts = [(12 * s, 36 * s), (22 * s, 24 * s), (32 * s, 36 * s), (42 * s, 48 * s), (52 * s, 30 * s)]
    d.line(pts, fill=(255, 255, 255, 255), width=max(2, int(5 * s)), joint="curve")
    return img


def parse_port(text: str) -> int | None:
    try:
        value = int(text.strip())
    except ValueError:
        return None
    return value if 1 <= value <= 65535 else None


class MidiOut:
    """Thin wrapper around python-rtmidi; every failure is reported, never raised."""

    def __init__(self):
        self.error = ""
        self.port_name = ""
        try:
            import rtmidi

            self._out = rtmidi.MidiOut()
        except Exception as exc:  # no MIDI system available
            self._out = None
            self.error = f"MIDI is not available: {exc}"

    def ports(self) -> list[str]:
        if self._out is None:
            return []
        names = [n for n in self._out.get_ports() if not any(w in n.lower() for w in HIDDEN_PORT_WORDS)]
        return ([MAC_VIRTUAL_PORT] if IS_MAC else []) + names

    def open(self, name: str) -> bool:
        if self._out is None:
            return False
        self.close()
        try:
            if name == MAC_VIRTUAL_PORT:
                self._out.open_virtual_port("Head Tracking Bridge")
            else:
                self._out.open_port(list(self._out.get_ports()).index(name))
        except Exception as exc:
            self.error = f"Could not open MIDI port: {exc}"
            return False
        self.port_name, self.error = name, ""
        return True

    def close(self):
        if self._out is not None and self._out.is_port_open():
            self._out.close_port()
        self.port_name = ""

    @property
    def is_open(self) -> bool:
        return self._out is not None and self._out.is_port_open()

    def send(self, message: list[int]):
        if self.is_open:
            self._out.send_message(message)


class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        self.resizable(False, False)
        self.settings = load_settings()
        self.engine = Engine()
        self.midi = MidiOut()
        self.engine.midi_send = self.midi.send
        self._apply_invert()
        self.tray_icon = None
        self._last_count = 0
        self._port_job = None
        self._port_busy = False
        self._busy_retry_at = 0.0
        self._set_window_icon()
        self._build_ui()
        self._refresh_midi_ports()
        self._apply_port(force=True)
        self.after(500, self._refresh)

        if IS_MAC:
            # Closing the window hides it; clicking the Dock icon brings it back.
            self.protocol("WM_DELETE_WINDOW", self.withdraw)
            self.createcommand("tk::mac::ReopenApplication", self._restore)
            self.createcommand("tk::mac::Quit", self._quit)
        else:
            self.protocol("WM_DELETE_WINDOW", self._to_tray)

    # ---------- UI ----------
    def _set_window_icon(self):
        try:
            if IS_WIN:
                # CustomTkinter sets its own icon shortly after start; override it afterwards.
                ico = Path(tempfile.gettempdir()) / "head_tracking_bridge.ico"
                make_icon_image(256).save(ico, sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
                self.after(300, lambda: self.iconbitmap(str(ico)))
            else:
                from PIL import ImageTk

                self._icon_photo = ImageTk.PhotoImage(make_icon_image(128))
                self.iconphoto(True, self._icon_photo)
        except Exception:
            pass

    def _card(self, title: str) -> ctk.CTkFrame:
        frame = ctk.CTkFrame(self, corner_radius=14)
        frame.pack(fill="x", padx=20, pady=(0, 12))
        ctk.CTkLabel(frame, text=title, font=ctk.CTkFont(size=14, weight="bold")).pack(
            anchor="w", padx=18, pady=(14, 6)
        )
        return frame

    def _small(self, parent, text="", color=MUTED) -> ctk.CTkLabel:
        return ctk.CTkLabel(
            parent, text=text, text_color=color, font=ctk.CTkFont(size=12), anchor="w", justify="left",
            wraplength=380,
        )

    def _build_ui(self):
        # Header
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.pack(fill="x", padx=22, pady=(20, 14))
        ctk.CTkLabel(header, text=APP_NAME, font=ctk.CTkFont(size=24, weight="bold")).pack(anchor="w")
        ctk.CTkLabel(
            header, text="Bridgehead  →  Gig Performer  →  Virtuoso knobs", text_color=MUTED,
            font=ctk.CTkFont(size=13),
        ).pack(anchor="w")

        # Status
        status = self._card("Status")
        row = ctk.CTkFrame(status, fg_color="transparent")
        row.pack(fill="x", padx=18)
        self.dot = ctk.CTkLabel(row, text="●", text_color=GREY, font=ctk.CTkFont(size=18), width=18)
        self.dot.pack(side="left", padx=(0, 8))
        self.status_text = ctk.CTkLabel(row, text="Starting…", font=ctk.CTkFont(size=13), anchor="w")
        self.status_text.pack(side="left", fill="x")
        self.detail_text = self._small(status)
        self.detail_text.pack(fill="x", padx=18, pady=(2, 14))

        # Input
        inp = self._card("Input – Bridgehead")
        grid = ctk.CTkFrame(inp, fg_color="transparent")
        grid.pack(fill="x", padx=18)
        grid.columnconfigure(0, weight=1)
        ctk.CTkLabel(grid, text="Port  (Bridgehead sends to)", anchor="w").grid(row=0, column=0, sticky="w")
        self.in_port = ctk.CTkEntry(grid, width=96, justify="center")
        self.in_port.insert(0, str(self.settings["in_port"]))
        self.in_port.grid(row=0, column=1)
        self.in_port.bind("<KeyRelease>", self._schedule_port)
        self.in_port.bind("<Return>", lambda _e: self._apply_port())
        self.in_port.bind("<FocusOut>", lambda _e: self._apply_port())
        self.port_hint = self._small(inp, "Bridgehead profile: DaVinci Resolve (sends /ypr to port 8000).")
        self.port_hint.pack(fill="x", padx=18, pady=(6, 14))
        self.port_error = self._small(inp, color=RED)  # only shown while there is an error

        # Output
        out = self._card("Output – MIDI to Gig Performer")
        port_row = ctk.CTkFrame(out, fg_color="transparent")
        port_row.pack(fill="x", padx=18)
        ctk.CTkLabel(port_row, text="MIDI port", anchor="w").pack(side="left")
        self.midi_menu = ctk.CTkOptionMenu(port_row, values=["—"], command=self._on_midi_port, width=230)
        self.midi_menu.pack(side="left", padx=(10, 6), fill="x", expand=True)
        ctk.CTkButton(port_row, text="↻", width=32, command=self._refresh_midi_ports).pack(side="right")
        self.midi_error = self._small(out, color=RED)  # only shown while there is an error
        invert_row = ctk.CTkFrame(out, fg_color="transparent")
        invert_row.pack(fill="x", padx=18, pady=(10, 0))
        ctk.CTkLabel(invert_row, text="Invert", anchor="w").pack(side="left", padx=(0, 10))
        self.invert_boxes = {}
        for axis in AXES:
            box = ctk.CTkCheckBox(invert_row, text=axis.capitalize(), width=70, command=self._on_invert)
            if self.settings[f"invert_{axis}"]:
                box.select()
            box.pack(side="left")
            self.invert_boxes[axis] = box
        learn_row = ctk.CTkFrame(out, fg_color="transparent")
        learn_row.pack(fill="x", padx=18, pady=(12, 0))
        ctk.CTkLabel(learn_row, text="MIDI Learn", anchor="w").pack(side="left", padx=(0, 10))
        self.learn_buttons = {}
        for index, axis in enumerate(AXES):
            button = ctk.CTkButton(learn_row, text=axis.capitalize(), width=74,
                                   command=lambda i=index: self._start_learn(i))
            button.pack(side="left", padx=(0, 6))
            self.learn_buttons[index] = button
        self.learn_text = self._small(
            out, "Click while Gig Performer is learning: only that knob moves for a few seconds."
        )
        self.learn_text.pack(fill="x", padx=18, pady=(6, 14))

        # Buttons
        buttons = ctk.CTkFrame(self, fg_color="transparent")
        buttons.pack(fill="x", padx=20, pady=(2, 20))
        secondary = {"fg_color": "transparent", "border_width": 1, "text_color": ("gray10", "gray90"),
                     "border_color": ("gray60", "gray40"), "hover_color": ("gray85", "gray25")}
        ctk.CTkButton(buttons, text="Reset", width=70, command=self._reset, **secondary).pack(
            side="left", padx=(0, 6)
        )
        ctk.CTkButton(buttons, text="Setup guide", width=100, command=lambda: webbrowser.open(SETUP_GUIDE_URL),
                      **secondary).pack(side="left", padx=(0, 6))
        ctk.CTkButton(buttons, text="Quit", width=80, command=self._quit, **secondary).pack(side="right")
        hide_text = "Hide window" if IS_MAC else "Minimize to tray"
        ctk.CTkButton(buttons, text=hide_text, command=self.withdraw if IS_MAC else self._to_tray).pack(
            side="right", padx=(0, 6)
        )

    def _set_port_error(self, text: str):
        if text == self.port_error.cget("text"):
            return
        self.port_error.configure(text=text)
        if text:
            self.port_hint.pack_configure(pady=(6, 4))
            self.port_error.pack(fill="x", padx=18, pady=(0, 14), after=self.port_hint)
        else:
            self.port_error.pack_forget()
            self.port_hint.pack_configure(pady=(6, 14))

    def _set_midi_error(self, text: str):
        self.midi_error.configure(text=text)
        if text:
            self.midi_error.pack(fill="x", padx=18, pady=(6, 0), after=self.midi_menu.master)
        else:
            self.midi_error.pack_forget()

    # ---------- Input port (applied automatically) ----------
    def _schedule_port(self, _event=None):
        if self._port_job is not None:
            self.after_cancel(self._port_job)
        self._port_job = self.after(PORT_APPLY_DELAY_MS, self._apply_port)

    def _apply_port(self, force: bool = False):
        if self._port_job is not None:
            self.after_cancel(self._port_job)
            self._port_job = None
        port = parse_port(self.in_port.get())
        if port is None:
            self._set_port_error("The port must be a number between 1 and 65535.")
            return
        if not (port != self.settings["in_port"] or force or not self.engine.running):
            self._set_port_error("")
            return
        self.settings["in_port"] = port
        save_settings(self.settings)
        self._start_engine()

    def _start_engine(self) -> bool:
        try:
            self.engine.start(self.settings["in_port"])
        except OSError:
            self._port_busy = True
            self._set_port_error(
                f"Port {self.settings['in_port']} is in use by another program (e.g. Virtuoso standalone "
                f"with OSC head tracking). Close it; this app connects automatically."
            )
            return False
        self._port_busy = False
        self._set_port_error("")
        return True

    # ---------- MIDI ----------
    def _refresh_midi_ports(self):
        ports = self.midi.ports()
        if not ports:
            self.midi_menu.configure(values=["No MIDI port found"])
            self.midi_menu.set("No MIDI port found")
            self.midi.close()
            self.midi.close()
            hint = "Install and start loopMIDI, create a port, then press ↻." if IS_WIN else ""
            self._set_midi_error(self.midi.error or hint)
            return
        self.midi_menu.configure(values=ports)
        wanted = self.settings["midi_port"]
        if wanted not in ports:
            # Only pick a port automatically if it is clearly a virtual MIDI port.
            wanted = next((p for p in ports if any(w in p.lower() for w in VIRTUAL_PORT_WORDS)), None)
        if wanted is None:
            self.midi.close()
            self.midi_menu.set("Choose a MIDI port")
            hint = "No loopMIDI port found. Start loopMIDI, create a port, then press ↻." if IS_WIN else ""
            self._set_midi_error(hint)
            return
        self.midi_menu.set(wanted)
        self._on_midi_port(wanted)

    def _on_midi_port(self, name: str):
        if name == self.midi.port_name and self.midi.is_open:
            return
        if self.midi.open(name):
            self.settings["midi_port"] = name
            save_settings(self.settings)
            self.engine.reset_midi_cache()
            self._set_midi_error("")
        else:
            self._set_midi_error(self.midi.error)

    # ---------- MIDI Learn helper ----------
    def _start_learn(self, axis: int):
        if not self.midi.is_open:
            self._set_midi_error("Choose a MIDI port first.")
            return
        self.engine.paused = True
        self._learn_axis = axis
        self._learn_until = time.monotonic() + LEARN_SECONDS
        for button in self.learn_buttons.values():
            button.configure(state="disabled")
        self._learn_tick()

    def _learn_tick(self):
        remaining = self._learn_until - time.monotonic()
        axis = self._learn_axis
        if remaining <= 0:
            low, high = DEFAULT_RANGES[axis]
            self.midi.send(pitch_bend(axis, angle_to_14bit(0.0, low, high)))  # back to centre
            self.engine.paused = False
            self.engine.reset_midi_cache()
            for button in self.learn_buttons.values():
                button.configure(state="normal")
            self.learn_text.configure(
                text="Click while Gig Performer is learning: only that knob moves for a few seconds.",
                text_color=MUTED,
            )
            return
        angle = LEARN_WIGGLE_DEGREES * math.sin(2 * math.pi * 0.5 * time.monotonic())
        low, high = DEFAULT_RANGES[axis]
        self.midi.send(pitch_bend(axis, angle_to_14bit(angle, low, high)))
        self.learn_text.configure(
            text=f"Sending {AXES[axis].capitalize()} only (pitch bend ch {axis + 1})… {int(remaining) + 1} s",
            text_color=AMBER,
        )
        self.after(20, self._learn_tick)

    def _apply_invert(self):
        self.engine.invert = [self.settings[f"invert_{a}"] for a in AXES]
        self.engine.reset_midi_cache()

    def _on_invert(self):
        for axis, box in self.invert_boxes.items():
            self.settings[f"invert_{axis}"] = bool(box.get())
        save_settings(self.settings)
        self._apply_invert()

    # ---------- Reset ----------
    def _reset(self):
        if not messagebox.askyesno(APP_NAME, "Reset all settings to their defaults?", parent=self):
            return
        midi_port = self.settings["midi_port"]
        self.settings = dict(DEFAULTS, midi_port=midi_port)
        save_settings(self.settings)
        self.in_port.delete(0, "end")
        self.in_port.insert(0, str(DEFAULTS["in_port"]))
        for box in self.invert_boxes.values():
            box.deselect()
        self._apply_invert()
        self._start_engine()

    # ---------- Status ----------
    def _refresh(self):
        count = self.engine.received
        rate = (count - self._last_count) * 2  # every 0.5 s
        self._last_count = count
        if self._port_busy and time.monotonic() >= self._busy_retry_at:
            self._busy_retry_at = time.monotonic() + BUSY_RETRY_SECONDS
            self._start_engine()
        detail = "Detected: " + ", ".join(self.engine.addresses) if self.engine.addresses else ""
        if not self.engine.running:
            color, text = RED, "Stopped – input port in use"
            detail = "Retrying automatically…"
        elif self.engine.receiving_angles and not self.midi.is_open:
            color, text = AMBER, "Receiving – no MIDI port open"
        elif self.engine.receiving_angles:
            color, text = GREEN, f"Receiving ({rate}/s) – sending MIDI to Gig Performer"
        elif self.engine.receiving:
            color, text = AMBER, "Receiving – but no yaw/pitch/roll"
            detail = "Set the Bridgehead profile to DaVinci Resolve (/ypr). " + detail
        else:
            color, text = AMBER, "Running – waiting for Bridgehead"
            detail = f"Listening on port {self.settings['in_port']}."
        self.dot.configure(text_color=color)
        self.status_text.configure(text=text)
        self.detail_text.configure(text=detail)
        if self.tray_icon is not None:
            self.tray_icon.title = f"{APP_NAME}: {text}"
        self.after(500, self._refresh)

    # ---------- Tray (Windows) ----------
    def _to_tray(self):
        try:
            import pystray

            if self.tray_icon is None:
                self.tray_icon = pystray.Icon(
                    "head_tracking_bridge",
                    make_icon_image(64),
                    APP_NAME,
                    pystray.Menu(
                        pystray.MenuItem("Open", lambda *_: self.after(0, self._restore), default=True),
                        pystray.MenuItem("Quit", lambda *_: self.after(0, self._quit)),
                    ),
                )
                self.tray_icon.run_detached()
            self.withdraw()
        except Exception:
            self.iconify()

    def _restore(self):
        self.deiconify()
        self.lift()
        self.focus_force()

    def _quit(self):
        if self.tray_icon is not None:
            try:
                self.tray_icon.stop()
            except Exception:
                pass
            self.tray_icon = None
        self.engine.stop()
        self.midi.close()
        self.destroy()


def main():
    try:
        App().mainloop()
    except Exception:
        import traceback

        log = settings_path().with_name("crash.log")
        try:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(traceback.format_exc(), encoding="utf-8")
        except OSError:
            pass
        raise


if __name__ == "__main__":
    main()
