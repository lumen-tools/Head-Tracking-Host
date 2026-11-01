"""HeadSmooth - smooths Bridgehead head tracking for APL Virtuoso (Windows & macOS)."""
from __future__ import annotations

import json
import os
import sys
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

from smoother_core import Engine

APP_NAME = "HeadSmooth"
IS_MAC = sys.platform == "darwin"
IS_WIN = sys.platform == "win32"
DEFAULTS = {"in_port": 8000, "out_port": 8001, "smooth_ms": 40, "bypass": False}


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
    d.ellipse((2 * s, 2 * s, 62 * s, 62 * s), fill=(0, 103, 192, 255))
    # stylised smooth wave
    pts = [(12 * s, 36 * s), (22 * s, 24 * s), (32 * s, 36 * s), (42 * s, 48 * s), (52 * s, 30 * s)]
    d.line(pts, fill=(255, 255, 255, 255), width=max(2, int(5 * s)), joint="curve")
    return img


class App(tk.Tk):
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
        self._set_window_icon()
        self._build_ui()
        self._start_engine(show_error=True)
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
            from PIL import ImageTk

            self._icon_photo = ImageTk.PhotoImage(make_icon_image(64))
            self.iconphoto(True, self._icon_photo)
        except Exception:
            pass

    def _build_ui(self):
        pad = {"padx": 16}
        root = ttk.Frame(self, padding=(0, 14, 0, 14))
        root.pack(fill="both", expand=True)

        ttk.Label(root, text=APP_NAME, font=("", 18, "bold")).pack(anchor="w", **pad)
        ttk.Label(root, text="Bridgehead  →  Smoothing  →  APL Virtuoso").pack(anchor="w", **pad)

        # Status
        status = ttk.LabelFrame(root, text="Status", padding=10)
        status.pack(fill="x", padx=16, pady=(12, 0))
        row = ttk.Frame(status)
        row.pack(fill="x")
        self.dot = tk.Canvas(row, width=14, height=14, highlightthickness=0, bd=0)
        self.dot.pack(side="left", padx=(0, 8))
        self.dot_item = self.dot.create_oval(2, 2, 12, 12, fill="#8a8a8a", outline="")
        self.status_text = ttk.Label(row, text="Starting…")
        self.status_text.pack(side="left")
        self.detail_text = ttk.Label(status, text="", foreground="#666666")
        self.detail_text.pack(anchor="w", pady=(6, 0))

        # Ports
        ports = ttk.LabelFrame(root, text="Ports", padding=10)
        ports.pack(fill="x", padx=16, pady=(12, 0))
        ttk.Label(ports, text="Input (Bridgehead sends to)").grid(row=0, column=0, sticky="w")
        self.in_port = ttk.Entry(ports, width=8, justify="center")
        self.in_port.insert(0, str(self.settings["in_port"]))
        self.in_port.grid(row=0, column=1, padx=(12, 0), pady=2)
        ttk.Label(ports, text="Output (port set in Virtuoso)").grid(row=1, column=0, sticky="w")
        self.out_port = ttk.Entry(ports, width=8, justify="center")
        self.out_port.insert(0, str(self.settings["out_port"]))
        self.out_port.grid(row=1, column=1, padx=(12, 0), pady=2)
        ttk.Button(ports, text="Apply", command=lambda: self._start_engine(show_error=True)).grid(
            row=0, column=2, rowspan=2, padx=(12, 0), sticky="ns"
        )
        for entry in (self.in_port, self.out_port):
            entry.bind("<Return>", lambda _e: self._start_engine(show_error=True))
        self.port_hint = ttk.Label(ports, text="", foreground="#666666", wraplength=400, justify="left")
        self.port_hint.grid(row=2, column=0, columnspan=3, sticky="w", pady=(8, 0))
        self._update_port_hint()

        # Smoothing
        smooth = ttk.LabelFrame(root, text="Smoothing", padding=10)
        smooth.pack(fill="x", padx=16, pady=(12, 0))
        top = ttk.Frame(smooth)
        top.pack(fill="x")
        self.smooth_value = ttk.Label(top, text="", width=8)
        self.smooth_value.pack(side="right")
        self.smooth_var = tk.DoubleVar(value=self.settings["smooth_ms"])
        ttk.Scale(
            top, from_=5, to=150, variable=self.smooth_var, command=self._on_smooth, length=260
        ).pack(side="left", fill="x", expand=True)
        ttk.Label(smooth, text="lower = more direct        higher = smoother", foreground="#666666").pack(anchor="w", pady=(4, 0))
        self.bypass_var = tk.BooleanVar(value=self.settings["bypass"])
        ttk.Checkbutton(
            smooth, text="Bypass smoothing (for A/B comparison)", variable=self.bypass_var, command=self._on_bypass
        ).pack(anchor="w", pady=(8, 0))
        self._on_smooth()

        # Buttons
        buttons = ttk.Frame(root)
        buttons.pack(fill="x", padx=16, pady=(14, 0))
        hide_text = "Hide window" if IS_MAC else "Minimize to tray"
        ttk.Button(buttons, text=hide_text, command=self.withdraw if IS_MAC else self._to_tray).pack(
            side="left", expand=True, fill="x", padx=(0, 4)
        )
        ttk.Button(buttons, text="Quit", command=self._quit).pack(side="left", expand=True, fill="x", padx=(4, 0))

    def _update_port_hint(self):
        self.port_hint.config(
            text=f"Bridgehead: leave unchanged (sends to {self.settings['in_port']}).\n"
            f"Virtuoso: Head tracking → gear icon → Port {self.settings['out_port']}."
        )

    # ---------- Actions ----------
    def _read_port(self, entry: ttk.Entry, fallback: int) -> int | None:
        try:
            value = int(entry.get().strip())
            if 1 <= value <= 65535:
                return value
        except ValueError:
            pass
        entry.delete(0, "end")
        entry.insert(0, str(fallback))
        return None

    def _start_engine(self, show_error: bool = False):
        in_port = self._read_port(self.in_port, self.settings["in_port"])
        out_port = self._read_port(self.out_port, self.settings["out_port"])
        if in_port is None or out_port is None:
            messagebox.showerror(APP_NAME, "Ports must be numbers between 1 and 65535.")
            return
        if in_port == out_port:
            messagebox.showerror(APP_NAME, "Input and output must not be the same port.")
            return
        self.settings.update(in_port=in_port, out_port=out_port)
        save_settings(self.settings)
        self._update_port_hint()
        try:
            self.engine.start(in_port, out_port)
        except OSError:
            self._error = (
                f"Port {in_port} is already in use.\n\n"
                f"Virtuoso is probably still listening on port {in_port}. In Virtuoso "
                f"(Head tracking → gear icon) set the port to {out_port}, then click \"Apply\" here.\n\n"
                f"Or {APP_NAME} is already running."
            )
            if show_error:
                messagebox.showwarning(APP_NAME, self._error)
            return
        self._error = None

    def _on_smooth(self, _value=None):
        ms = round(self.smooth_var.get())
        self.engine.smooth_ms = float(ms)
        self.smooth_value.config(text=f"{ms} ms")
        self.settings["smooth_ms"] = ms
        if _value is not None:
            save_settings(self.settings)

    def _on_bypass(self):
        self.engine.bypass = self.bypass_var.get()
        self.settings["bypass"] = self.engine.bypass
        save_settings(self.settings)

    def _refresh(self):
        count = self.engine.received
        rate = (count - self._last_count) * 2  # every 0.5 s
        self._last_count = count
        if not self.engine.running:
            color, text = "#c42b1c", "Stopped – port in use"
            detail = f"Set Virtuoso to port {self.settings['out_port']}, then click \"Apply\"."
        elif self.engine.receiving:
            mode = "unsmoothed (A/B)" if self.engine.bypass else f"smoothed, {int(self.engine.rate_hz)} Hz to Virtuoso"
            color, text = "#1a7f37", f"Receiving ({rate}/s) – {mode}"
            detail = "Detected: " + ", ".join(self.engine.addresses) if self.engine.addresses else ""
        else:
            color, text = "#c69026", "Running – waiting for Bridgehead"
            detail = f"Listening on port {self.settings['in_port']}."
        self.dot.itemconfig(self.dot_item, fill=color)
        self.status_text.config(text=text)
        self.detail_text.config(text=detail)
        if self.tray_icon is not None:
            self.tray_icon.title = f"{APP_NAME}: {text}"
        self.after(500, self._refresh)

    # ---------- Tray (Windows) ----------
    def _to_tray(self):
        try:
            import pystray

            if self.tray_icon is None:
                self.tray_icon = pystray.Icon(
                    APP_NAME,
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
