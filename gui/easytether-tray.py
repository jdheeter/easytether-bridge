#!/usr/bin/env python3
"""Tray control for easytether-bridge — menu only, no extra window."""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

os.environ.setdefault("GDK_BACKEND", "x11,wayland")

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

INDICATOR = None
try:
    gi.require_version("AyatanaAppIndicator3", "0.1")
    from gi.repository import AyatanaAppIndicator3 as AppIndicator3

    INDICATOR = AppIndicator3
except (ValueError, ImportError):
    try:
        gi.require_version("AppIndicator3", "0.1")
        from gi.repository import AppIndicator3

        INDICATOR = AppIndicator3
    except (ValueError, ImportError):
        INDICATOR = None

BRIDGE = os.environ.get("EASYTETHER_BRIDGE", "/usr/local/bin/easytether-bridge")
UNIT = "easytether-bridge.service"
SYSTEMCTL = shutil.which("systemctl") or "/usr/bin/systemctl"
IFNAME = "tun-easytether"
STATS = Path(f"/sys/class/net/{IFNAME}/statistics")
ICON_UP = "network-transmit-receive-symbolic"
ICON_DOWN = "network-offline-symbolic"


def iface_up() -> bool:
    return Path(f"/sys/class/net/{IFNAME}").exists()


def has_unit() -> bool:
    return Path("/etc/systemd/system/" + UNIT).exists() or Path(
        "/lib/systemd/system/" + UNIT
    ).exists()


def unit_active() -> bool:
    if not has_unit():
        return False
    r = subprocess.run(
        [SYSTEMCTL, "is-active", "--quiet", UNIT],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return r.returncode == 0


def sudo_cmd(*args: str) -> list[str]:
    sudo = shutil.which("sudo") or "sudo"
    return [sudo, "-n", *args]


def read_bytes() -> tuple[int, int] | None:
    try:
        rx = int((STATS / "rx_bytes").read_text())
        tx = int((STATS / "tx_bytes").read_text())
        return rx, tx
    except OSError:
        return None


def read_addr() -> tuple[str, str] | None:
    r = subprocess.run(
        ["ip", "-4", "-o", "addr", "show", "dev", IFNAME],
        capture_output=True,
        text=True,
        check=False,
    )
    if r.returncode != 0 or not r.stdout.strip():
        return None
    # 7: tun-easytether    inet 192.168.117.2 peer 192.168.117.1/32 ...
    parts = r.stdout.split()
    ip = peer = ""
    if "inet" in parts:
        i = parts.index("inet")
        if i + 1 < len(parts):
            ip = parts[i + 1].split("/")[0]
        if i + 3 < len(parts) and parts[i + 2] == "peer":
            peer = parts[i + 3].split("/")[0]
    return (ip, peer) if ip else None


def fmt_bytes(n: float) -> str:
    n = float(n)
    for unit, div in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if n >= div:
            return f"{n / div:.1f} {unit}"
    return f"{int(n)} B"


def fmt_rate(bps: float) -> str:
    if bps < 0:
        bps = 0
    return f"{fmt_bytes(bps)}/s"


def fmt_dur(seconds: float) -> str:
    s = int(max(0, seconds))
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if h:
        return f"{h}h {m:02d}m"
    if m:
        return f"{m}m {s:02d}s"
    return f"{s}s"


def set_label(item: Gtk.MenuItem, text: str) -> None:
    item.set_label(text)
    child = item.get_child()
    if isinstance(child, Gtk.AccelLabel) or isinstance(child, Gtk.Label):
        child.set_text(text)


class App:
    def __init__(self) -> None:
        self.proc: subprocess.Popen[str] | None = None
        self.last_error = ""
        self.prev_bytes: tuple[int, int] | None = None
        self.prev_t = time.monotonic()
        self.rx_bps = 0.0
        self.tx_bps = 0.0
        self.up_since: float | None = None
        self.addr: tuple[str, str] | None = None
        self._was_up = False

        self.status_item = Gtk.MenuItem(label="EasyTether — …")
        self.addr_item = Gtk.MenuItem(label="No tunnel")
        self.rate_item = Gtk.MenuItem(label="↓ —    ↑ —")
        self.vol_item = Gtk.MenuItem(label="In —    Out —")
        self.up_item = Gtk.MenuItem(label="Up —")
        for info in (
            self.status_item,
            self.addr_item,
            self.rate_item,
            self.vol_item,
            self.up_item,
        ):
            # Stay activatable so AppIndicator does not hide “insensitive” rows.
            info.connect("activate", lambda *_: None)

        self.connect_item = Gtk.MenuItem(label="Connect")
        self.connect_item.connect("activate", self.on_connect)
        self.disconnect_item = Gtk.MenuItem(label="Disconnect")
        self.disconnect_item.connect("activate", self.on_disconnect)
        quit_item = Gtk.MenuItem(label="Quit tray")
        quit_item.connect("activate", self.on_quit)

        menu = Gtk.Menu()
        for w in (
            self.status_item,
            self.addr_item,
            self.rate_item,
            self.vol_item,
            self.up_item,
            Gtk.SeparatorMenuItem(),
            self.connect_item,
            self.disconnect_item,
            Gtk.SeparatorMenuItem(),
            quit_item,
        ):
            menu.append(w)
            w.show()
        menu.show_all()

        if INDICATOR is None:
            sys.stderr.write(
                "easytether-tray needs AyatanaAppIndicator3 or AppIndicator3\n"
            )
            sys.exit(1)

        self.indicator = INDICATOR.Indicator.new(
            "easytether-bridge",
            ICON_DOWN,
            INDICATOR.IndicatorCategory.SYSTEM_SERVICES,
        )
        self.indicator.set_status(INDICATOR.IndicatorStatus.ACTIVE)
        self.indicator.set_title("EasyTether")
        self.indicator.set_menu(menu)

        self.poll_status()
        GLib.timeout_add(1000, self.poll_status)

    def on_connect(self, *_args) -> None:
        if self.proc and self.proc.poll() is None:
            return
        if has_unit():
            r = subprocess.run(
                sudo_cmd(SYSTEMCTL, "start", UNIT),
                capture_output=True,
                text=True,
                check=False,
            )
            if r.returncode != 0:
                self.last_error = (r.stderr or r.stdout or "start failed").strip().split("\n")[-1]
            else:
                self.last_error = ""
            return
        if not Path(BRIDGE).exists():
            self.last_error = f"missing {BRIDGE}"
            return
        try:
            self.proc = subprocess.Popen(
                sudo_cmd(BRIDGE, "-v"),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            self.last_error = ""
        except OSError as e:
            self.last_error = str(e)

    def on_disconnect(self, *_args) -> None:
        if has_unit():
            subprocess.run(
                sudo_cmd(SYSTEMCTL, "stop", UNIT),
                capture_output=True,
                text=True,
                check=False,
            )
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.proc = None
        self.last_error = ""

    def on_quit(self, *_args) -> None:
        Gtk.main_quit()

    def poll_status(self) -> bool:
        up = iface_up()
        active = unit_active() or (
            self.proc is not None and self.proc.poll() is None
        )
        now = time.monotonic()

        if up:
            if not self._was_up:
                self.up_since = now
                self.prev_bytes = None
                self.addr = read_addr()
            self._was_up = True
            b = read_bytes()
            dt = max(0.001, now - self.prev_t)
            if b and self.prev_bytes:
                self.rx_bps = (b[0] - self.prev_bytes[0]) / dt
                self.tx_bps = (b[1] - self.prev_bytes[1]) / dt
            if b:
                self.prev_bytes = b
            self.prev_t = now
            if self.addr is None:
                self.addr = read_addr()

            ip, peer = self.addr or ("?", "?")
            set_label(self.status_item, "EasyTether — Connected")
            set_label(self.addr_item, f"{IFNAME}  {ip} → {peer}")
            set_label(
                self.rate_item,
                f"↓ {fmt_rate(self.rx_bps):<10}  ↑ {fmt_rate(self.tx_bps)}",
            )
            if b:
                set_label(
                    self.vol_item,
                    f"In {fmt_bytes(b[0]):<8}  Out {fmt_bytes(b[1])}",
                )
            set_label(
                self.up_item,
                f"Up {fmt_dur(now - (self.up_since or now))}",
            )
            self.indicator.set_icon_full(
                ICON_UP, f"EasyTether · ↓ {fmt_rate(self.rx_bps)} ↑ {fmt_rate(self.tx_bps)}"
            )
            self.connect_item.set_sensitive(False)
            self.disconnect_item.set_sensitive(True)
        elif active:
            self._was_up = False
            self.addr = None
            self.prev_bytes = None
            self.up_since = None
            set_label(self.status_item, "EasyTether — Connecting…")
            set_label(self.addr_item, "Waiting for DHCP from the phone")
            set_label(self.rate_item, "↓ —    ↑ —")
            set_label(self.vol_item, "In —    Out —")
            set_label(
                self.up_item,
                self.last_error or "USB debugging on, EasyTether USB enabled",
            )
            self.indicator.set_icon_full(ICON_DOWN, "EasyTether · connecting")
            self.connect_item.set_sensitive(False)
            self.disconnect_item.set_sensitive(True)
        else:
            self._was_up = False
            self.addr = None
            self.prev_bytes = None
            self.up_since = None
            set_label(self.status_item, "EasyTether — Disconnected")
            set_label(self.addr_item, "No tunnel")
            set_label(self.rate_item, "↓ —    ↑ —")
            set_label(self.vol_item, "In —    Out —")
            set_label(
                self.up_item,
                self.last_error or "Plug in the phone, or Connect",
            )
            self.indicator.set_icon_full(ICON_DOWN, "EasyTether · disconnected")
            self.connect_item.set_sensitive(True)
            self.disconnect_item.set_sensitive(False)
        return True


def main() -> int:
    App()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
