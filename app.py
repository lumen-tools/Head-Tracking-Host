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

from smoother_core import Engine

APP_NAME = "Head Tracking Smoother"
IS_MAC = sys.platform == "darwin"
IS_WIN = sys.platform == "win32"
DEFAULTS = {"in_port": 8000, "out_port": 8001, "smooth_ms": 40, "bypass": False}
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
        ctk.CTkLabel(
            header, text="Bridgehead  →  Smoothing  →  APL Virtuoso", text_color=MUTED, font=ctk.CTkFont(size=13)
        ).pack(anchor="w")

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

        # Ports
        ports = self._card("Ports")
        grid = ctk.CTkFrame(ports, fg_color="transparent")
        grid.pack(fill="x", padx=18)
        grid.columnconfigure(0, weight=1)
        ctk.CTkLabel(grid, text="Input  (Bridgehead sends to)", anchor="w").grid(row=0, column=0, sticky="w", pady=4)
        ctk.CTkLabel(grid, text="Output  (port set in Virtuoso)", anchor="w").grid(row=1, column=0, sticky="w", pady=4)
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
        self.port_hint.configure(
            text=f"Bridgehead: leave unchanged (sends to {self.settings['in_port']}).\n"
            f"Virtuoso: Head tracking → gear icon → Port {self.settings['out_port']}."
        )

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
            self._set_port_error(
                f"Port {self.settings['in_port']} is in use – probably Virtuoso is still listening on it. "
                f"Set Virtuoso to port {self.settings['out_port']}; this app connects automatically."
            )
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
        self._update_port_hint()
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
