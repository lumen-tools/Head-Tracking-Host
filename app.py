"""Head Tracking Smoother - smooths Bridgehead head tracking for APL Virtuoso (Windows & macOS)."""
from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from tkinter import messagebox

import customtkinter as ctk

from smoother_core import DEFAULT_RANGES, Engine

APP_NAME = "Head Tracking Smoother"
IS_MAC = sys.platform == "darwin"
IS_WIN = sys.platform == "win32"
DEFAULTS = {
    "in_port": 8000, "out_port": 8001, "smooth_ms": 40, "bypass": False,
    "output_mode": "osc", "midi_port": "", "invert_yaw": False, "invert_pitch": False, "invert_roll": False,
}
MODE_LABELS = {"osc": "Virtuoso (OSC)", "midi": "Gig Performer (MIDI)"}
MAC_VIRTUAL_PORT = "Head Tracking Smoother (virtual port)"
SLIDER_MIN, SLIDER_MAX = 5, 150
RECOMMENDED_MIN, RECOMMENDED_MAX = 20, 60
PORT_APPLY_DELAY_MS = 700
BUSY_RETRY_SECONDS = 2.0
RECORD_SECONDS = 20

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


def recordings_dir() -> Path:
    documents = Path.home() / "Documents"
    return (documents if documents.is_dir() else Path.home()) / f"{APP_NAME} recordings"


def reveal_file(path: Path) -> None:
    try:
        if IS_WIN:
            subprocess.Popen(["explorer", "/select,", str(path)])
        elif IS_MAC:
            subprocess.Popen(["open", "-R", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path.parent)])
    except OSError:
        pass


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
        names = list(self._out.get_ports())
        return ([MAC_VIRTUAL_PORT] if IS_MAC else []) + names

    def open(self, name: str) -> bool:
        if self._out is None:
            return False
        self.close()
        try:
            if name == MAC_VIRTUAL_PORT:
                self._out.open_virtual_port("Head Tracking Smoother")
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


def parse_port(text: str) -> int | None:
    try:
        value = int(text.strip())
    except ValueError:
        return None
    return value if 1 <= value <= 65535 else None


class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        self.resizable(False, False)
        self.settings = load_settings()
        self.engine = Engine()
        self.engine.smooth_ms = float(self.settings["smooth_ms"])
        self.engine.bypass = bool(self.settings["bypass"])
        self.midi = MidiOut()
        self.engine.midi_send = self.midi.send
        self._apply_engine_midi_settings()
        self.tray_icon = None
        self._last_count = 0
        self._port_job = None
        self._port_busy = False
        self._busy_retry_at = 0.0
        self._set_window_icon()
        self._build_ui()
        self._apply_ports(force=True)
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
                ico = Path(tempfile.gettempdir()) / "head_tracking_smoother.ico"
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

    def _build_ui(self):
        # Header
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.pack(fill="x", padx=22, pady=(20, 14))
        ctk.CTkLabel(header, text=APP_NAME, font=ctk.CTkFont(size=24, weight="bold")).pack(anchor="w")
        self.subtitle = ctk.CTkLabel(header, text="", text_color=MUTED, font=ctk.CTkFont(size=13))
        self.subtitle.pack(anchor="w")

        # Status
        status = self._card("Status")
        row = ctk.CTkFrame(status, fg_color="transparent")
        row.pack(fill="x", padx=18)
        self.dot = ctk.CTkLabel(row, text="●", text_color=GREY, font=ctk.CTkFont(size=18), width=18)
        self.dot.pack(side="left", padx=(0, 8))
        self.status_text = ctk.CTkLabel(row, text="Starting…", font=ctk.CTkFont(size=13), anchor="w")
        self.status_text.pack(side="left", fill="x")
        self.detail_text = ctk.CTkLabel(
            status, text="", text_color=MUTED, font=ctk.CTkFont(size=12), anchor="w", justify="left", wraplength=380
        )
        self.detail_text.pack(fill="x", padx=18, pady=(2, 8))
        record_row = ctk.CTkFrame(status, fg_color="transparent")
        record_row.pack(fill="x", padx=18, pady=(0, 14))
        self.record_button = ctk.CTkButton(
            record_row, text=f"Record {RECORD_SECONDS} s", width=110, command=self._start_recording,
            fg_color="transparent", border_width=1, text_color=("gray10", "gray90"),
            border_color=("gray60", "gray40"), hover_color=("gray85", "gray25"),
        )
        self.record_button.pack(side="left")
        self.record_text = ctk.CTkLabel(
            record_row, text="Saves the incoming data for analysis", text_color=MUTED, font=ctk.CTkFont(size=12)
        )
        self.record_text.pack(side="left", padx=(10, 0))

        # Output
        output = self._card("Output")
        self.mode_button = ctk.CTkSegmentedButton(
            output, values=list(MODE_LABELS.values()), command=self._on_mode
        )
        self.mode_button.set(MODE_LABELS[self.settings["output_mode"]])
        self.mode_button.pack(fill="x", padx=18, pady=(0, 10))
        self.osc_info = ctk.CTkLabel(
            output, text="Sends to Virtuoso's head tracking input (Virtuoso: OSC, profile APL).",
            text_color=MUTED, font=ctk.CTkFont(size=12), anchor="w", justify="left", wraplength=380,
        )
        self.midi_frame = ctk.CTkFrame(output, fg_color="transparent")
        port_row = ctk.CTkFrame(self.midi_frame, fg_color="transparent")
        port_row.pack(fill="x")
        ctk.CTkLabel(port_row, text="MIDI port", anchor="w").pack(side="left")
        self.midi_menu = ctk.CTkOptionMenu(port_row, values=["—"], command=self._on_midi_port, width=230)
        self.midi_menu.pack(side="left", padx=(10, 6), fill="x", expand=True)
        ctk.CTkButton(port_row, text="↻", width=32, command=self._refresh_midi_ports).pack(side="right")
        invert_row = ctk.CTkFrame(self.midi_frame, fg_color="transparent")
        invert_row.pack(fill="x", pady=(10, 0))
        ctk.CTkLabel(invert_row, text="Invert", anchor="w").pack(side="left", padx=(0, 10))
        self.invert_boxes = {}
        for axis in ("yaw", "pitch", "roll"):
            box = ctk.CTkCheckBox(invert_row, text=axis.capitalize(), width=70, command=self._on_invert)
            if self.settings[f"invert_{axis}"]:
                box.select()
            box.pack(side="left")
            self.invert_boxes[axis] = box
        (ylo, yhi), (plo, phi), (rlo, rhi) = DEFAULT_RANGES
        self.midi_info = ctk.CTkLabel(
            self.midi_frame,
            text=f"Bridgehead profile: DaVinci Resolve (/ypr).\n"
            f"Gig Performer: MIDI-learn the widgets for Virtuoso's knobs –\n"
            f"Yaw = pitch bend ch 1, Pitch = ch 2, Roll = ch 3. Virtuoso: Manual.\n"
            f"Ranges: yaw {ylo:+.0f}…{yhi:+.0f}°, pitch {plo:+.0f}…{phi:+.0f}°, roll {rlo:+.0f}…{rhi:+.0f}°.",
            text_color=MUTED, font=ctk.CTkFont(size=12), anchor="w", justify="left", wraplength=380,
        )
        self.midi_info.pack(fill="x", pady=(10, 0))
        self.midi_error = ctk.CTkLabel(
            self.midi_frame, text="", text_color=RED, font=ctk.CTkFont(size=12), anchor="w", justify="left",
            wraplength=380,
        )  # only shown while there is an error

        # Ports
        ports = self._card("Ports")
        grid = ctk.CTkFrame(ports, fg_color="transparent")
        grid.pack(fill="x", padx=18)
        grid.columnconfigure(0, weight=1)
        ctk.CTkLabel(grid, text="Input  (Bridgehead sends to)", anchor="w").grid(row=0, column=0, sticky="w", pady=4)
        self.out_label = ctk.CTkLabel(grid, text="Output  (port set in Virtuoso)", anchor="w")
        self.out_label.grid(row=1, column=0, sticky="w", pady=4)
        self.in_port = ctk.CTkEntry(grid, width=96, justify="center")
        self.out_port = ctk.CTkEntry(grid, width=96, justify="center")
        self.in_port.grid(row=0, column=1, pady=4)
        self.out_port.grid(row=1, column=1, pady=4)
        for entry, key in ((self.in_port, "in_port"), (self.out_port, "out_port")):
            entry.insert(0, str(self.settings[key]))
            entry.bind("<KeyRelease>", self._schedule_ports)
            entry.bind("<Return>", lambda _e: self._apply_ports())
            entry.bind("<FocusOut>", lambda _e: self._apply_ports())
        self.port_hint = ctk.CTkLabel(
            ports, text="", text_color=MUTED, font=ctk.CTkFont(size=12), anchor="w", justify="left", wraplength=380
        )
        self.port_hint.pack(fill="x", padx=18, pady=(6, 14))
        self.port_error = ctk.CTkLabel(
            ports, text="", text_color=RED, font=ctk.CTkFont(size=12), anchor="w", justify="left", wraplength=380
        )  # only shown while there is an error
        self._update_port_hint()

        # Smoothing
        smooth = self._card("Smoothing")
        top = ctk.CTkFrame(smooth, fg_color="transparent")
        top.pack(fill="x", padx=18)
        self.smooth_slider = ctk.CTkSlider(
            top, from_=SLIDER_MIN, to=SLIDER_MAX, number_of_steps=SLIDER_MAX - SLIDER_MIN, command=self._on_smooth
        )
        self.smooth_slider.set(self.settings["smooth_ms"])
        self.smooth_slider.pack(side="left", fill="x", expand=True)
        self.smooth_value = ctk.CTkLabel(top, text="", width=64, anchor="e", font=ctk.CTkFont(size=13, weight="bold"))
        self.smooth_value.pack(side="right")
        self.smooth_note = ctk.CTkLabel(smooth, text="", font=ctk.CTkFont(size=12), anchor="w")
        self.smooth_note.pack(fill="x", padx=18, pady=(6, 0))
        ctk.CTkLabel(
            smooth,
            text="Lower = more direct  ·  Higher = smoother, but adds delay",
            text_color=MUTED,
            font=ctk.CTkFont(size=12),
            anchor="w",
        ).pack(fill="x", padx=18)
        self.bypass_switch = ctk.CTkSwitch(smooth, text="Bypass smoothing (A/B comparison)", command=self._on_bypass)
        if self.settings["bypass"]:
            self.bypass_switch.select()
        self.bypass_switch.pack(anchor="w", padx=18, pady=(10, 16))
        self._on_smooth(self.settings["smooth_ms"], save=False)
        self._refresh_midi_ports()
        self._show_mode()

        # Buttons
        buttons = ctk.CTkFrame(self, fg_color="transparent")
        buttons.pack(fill="x", padx=20, pady=(2, 20))
        secondary = {"fg_color": "transparent", "border_width": 1, "text_color": ("gray10", "gray90"),
                     "border_color": ("gray60", "gray40"), "hover_color": ("gray85", "gray25")}
        ctk.CTkButton(buttons, text="Reset all settings", command=self._reset, **secondary).pack(
            side="left", padx=(0, 6)
        )
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

    def _update_port_hint(self):
        if self.settings["output_mode"] == "midi":
            text = f"Bridgehead sends to port {self.settings['in_port']}."
        else:
            text = (f"Bridgehead: leave unchanged (sends to {self.settings['in_port']}).\n"
                    f"Virtuoso: Head tracking → gear icon → Port {self.settings['out_port']}.")
        self.port_hint.configure(text=text)

    # ---------- Output mode ----------
    def _apply_engine_midi_settings(self):
        self.engine.output_mode = self.settings["output_mode"]
        self.engine.invert = [self.settings[f"invert_{a}"] for a in ("yaw", "pitch", "roll")]
        self.engine.reset_midi_cache()

    def _show_mode(self):
        midi = self.settings["output_mode"] == "midi"
        target = "Gig Performer → Virtuoso knobs" if midi else "APL Virtuoso"
        self.subtitle.configure(text=f"Bridgehead  →  Smoothing  →  {target}")
        if midi:
            self.osc_info.pack_forget()
            self.midi_frame.pack(fill="x", padx=18, pady=(0, 14))
            self.out_label.grid_remove()
            self.out_port.grid_remove()
        else:
            self.midi_frame.pack_forget()
            self.osc_info.pack(fill="x", padx=18, pady=(0, 14))
            self.out_label.grid()
            self.out_port.grid()
        self._update_port_hint()

    def _on_mode(self, label: str):
        mode = next(k for k, v in MODE_LABELS.items() if v == label)
        self.settings["output_mode"] = mode
        save_settings(self.settings)
        self._apply_engine_midi_settings()
        self._show_mode()

    def _refresh_midi_ports(self):
        ports = self.midi.ports()
        if not ports:
            self.midi_menu.configure(values=["No MIDI port found"])
            self.midi_menu.set("No MIDI port found")
            self.midi.close()
            hint = "Windows: install and start loopMIDI, create a port, then press ↻." if IS_WIN else ""
            self._set_midi_error(self.midi.error or hint)
            return
        self.midi_menu.configure(values=ports)
        wanted = self.settings["midi_port"]
        if wanted not in ports:
            wanted = next((p for p in ports if "loopmidi" in p.lower()), ports[0])
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

    def _set_midi_error(self, text: str):
        self.midi_error.configure(text=text)
        if text:
            self.midi_error.pack(fill="x", pady=(6, 0))
        else:
            self.midi_error.pack_forget()

    def _on_invert(self):
        for axis, box in self.invert_boxes.items():
            self.settings[f"invert_{axis}"] = bool(box.get())
        save_settings(self.settings)
        self._apply_engine_midi_settings()

    # ---------- Ports (applied automatically) ----------
    def _schedule_ports(self, _event=None):
        if self._port_job is not None:
            self.after_cancel(self._port_job)
        self._port_job = self.after(PORT_APPLY_DELAY_MS, self._apply_ports)

    def _apply_ports(self, force: bool = False):
        if self._port_job is not None:
            self.after_cancel(self._port_job)
            self._port_job = None
        in_port = parse_port(self.in_port.get())
        out_port = parse_port(self.out_port.get())
        if in_port is None or out_port is None:
            self._set_port_error("Ports must be numbers between 1 and 65535.")
            return
        if in_port == out_port:
            self._set_port_error("Input and output must be different ports.")
            return
        changed = (in_port, out_port) != (self.settings["in_port"], self.settings["out_port"])
        if not (changed or force or not self.engine.running):
            self._set_port_error("")
            return
        self.settings.update(in_port=in_port, out_port=out_port)
        save_settings(self.settings)
        self._update_port_hint()
        self._start_engine()

    def _start_engine(self) -> bool:
        try:
            self.engine.start(self.settings["in_port"], self.settings["out_port"])
        except OSError:
            self._port_busy = True
            if self.settings["output_mode"] == "midi":
                msg = (f"Port {self.settings['in_port']} is in use by another program (e.g. Virtuoso standalone). "
                       f"Close it; this app connects automatically.")
            else:
                msg = (f"Port {self.settings['in_port']} is in use – probably Virtuoso is still listening on it. "
                       f"Set Virtuoso to port {self.settings['out_port']}; this app connects automatically.")
            self._set_port_error(msg)
            return False
        self._port_busy = False
        self._set_port_error("")
        return True

    # ---------- Smoothing ----------
    def _on_smooth(self, value, save: bool = True):
        ms = int(round(float(value)))
        self.engine.smooth_ms = float(ms)
        self.settings["smooth_ms"] = ms
        self.smooth_value.configure(text=f"{ms} ms")
        if RECOMMENDED_MIN <= ms <= RECOMMENDED_MAX:
            self.smooth_note.configure(
                text=f"✓ Recommended: {RECOMMENDED_MIN}–{RECOMMENDED_MAX} ms  ·  Default: {DEFAULTS['smooth_ms']} ms",
                text_color=GREEN,
            )
        else:
            self.smooth_note.configure(
                text=f"Outside the recommended {RECOMMENDED_MIN}–{RECOMMENDED_MAX} ms  ·  Default: {DEFAULTS['smooth_ms']} ms",
                text_color=AMBER,
            )
        if save:
            save_settings(self.settings)

    def _on_bypass(self):
        self.engine.bypass = bool(self.bypass_switch.get())
        self.settings["bypass"] = self.engine.bypass
        save_settings(self.settings)

    # ---------- Recording ----------
    def _start_recording(self):
        if self.engine.recording:
            return
        self.engine.start_recording(RECORD_SECONDS)
        self.record_button.configure(state="disabled")
        self._recording_tick()

    def _recording_tick(self):
        if self.engine.recording:
            remaining = int(self.engine.recording_remaining()) + 1
            self.record_text.configure(text=f"Recording… {remaining} s – move your head", text_color=RED)
            self.after(200, self._recording_tick)
            return
        rows = self.engine.take_recording()
        self.record_button.configure(state="normal")
        self.record_text.configure(text="Saves the incoming data for analysis", text_color=MUTED)
        if not rows:
            messagebox.showwarning(
                APP_NAME, f"Nothing was received on port {self.settings['in_port']} during the recording.", parent=self
            )
            return
        path = recordings_dir() / time.strftime(f"recording-port{self.settings['in_port']}-%Y%m%d-%H%M%S.csv")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("w", newline="", encoding="utf-8") as f:
                f.write(f"# {APP_NAME} recording, input port {self.settings['in_port']}, platform {sys.platform}\n")
                writer = csv.writer(f)
                writer.writerow(["time_s", "address", "types", "values"])
                for t, address, tags, args in rows:
                    writer.writerow([f"{t:.6f}", address, tags, " ".join(repr(a) for a in args)])
        except OSError as exc:
            messagebox.showerror(APP_NAME, f"Could not save the recording:\n{exc}", parent=self)
            return
        messagebox.showinfo(APP_NAME, f"{len(rows)} messages saved to:\n{path}", parent=self)
        reveal_file(path)

    # ---------- Reset ----------
    def _reset(self):
        if not messagebox.askyesno(APP_NAME, "Reset all settings to their defaults?", parent=self):
            return
        self.settings = dict(DEFAULTS)
        save_settings(self.settings)
        for entry, key in ((self.in_port, "in_port"), (self.out_port, "out_port")):
            entry.delete(0, "end")
            entry.insert(0, str(DEFAULTS[key]))
        self.smooth_slider.set(DEFAULTS["smooth_ms"])
        self._on_smooth(DEFAULTS["smooth_ms"])
        self.bypass_switch.deselect()
        self._on_bypass()
        for box in self.invert_boxes.values():
            box.deselect()
        self.mode_button.set(MODE_LABELS["osc"])
        self._apply_engine_midi_settings()
        self._refresh_midi_ports()
        self._show_mode()
        self._start_engine()

    # ---------- Status ----------
    def _refresh(self):
        import time

        count = self.engine.received
        rate = (count - self._last_count) * 2  # every 0.5 s
        self._last_count = count
        if self._port_busy and time.monotonic() >= self._busy_retry_at:
            self._busy_retry_at = time.monotonic() + BUSY_RETRY_SECONDS
            self._start_engine()
        if not self.engine.running:
            color, text = RED, "Stopped – input port in use"
            detail = f"Set Virtuoso to port {self.settings['out_port']}. Retrying automatically…"
        elif self.engine.receiving and self.settings["output_mode"] == "midi":
            detail = "Detected: " + ", ".join(self.engine.addresses) if self.engine.addresses else ""
            if not self.engine.midi_has_angles:
                color, text = AMBER, "Receiving – but no yaw/pitch/roll"
                detail = "Set the Bridgehead profile to DaVinci Resolve (/ypr). " + detail
            elif not self.midi.is_open:
                color, text = AMBER, "Receiving – no MIDI port open"
            else:
                mode = "unsmoothed (A/B)" if self.engine.bypass else "smoothed"
                color, text = GREEN, f"Receiving ({rate}/s) – {mode}, MIDI to Gig Performer"
        elif self.engine.receiving:
            mode = "unsmoothed (A/B)" if self.engine.bypass else f"smoothed, {int(self.engine.rate_hz)} Hz to Virtuoso"
            color, text = GREEN, f"Receiving ({rate}/s) – {mode}"
            detail = "Detected: " + ", ".join(self.engine.addresses) if self.engine.addresses else ""
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
                    "head_tracking_smoother",
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
