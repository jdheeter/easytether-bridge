#!/usr/bin/env python3
"""Simple tray / window control for easytether-bridge on Linux."""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
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
IFNAME = "tun-easytether"
GW = "192.168.117.1"


def iface_up() -> bool:
    return Path(f"/sys/class/net/{IFNAME}").exists()


class App:
    def __init__(self) -> None:
        self.proc: subprocess.Popen[str] | None = None
        self.log = Gtk.TextBuffer()
        self.status_label = Gtk.Label(label="Disconnected")
        self.connect_btn = Gtk.Button(label="Connect")
        self.disconnect_btn = Gtk.Button(label="Disconnect")
        self.disconnect_btn.set_sensitive(False)
        self.connect_btn.connect("clicked", self.on_connect)
        self.disconnect_btn.connect("clicked", self.on_disconnect)

        self.win = Gtk.Window(title="EasyTether")
        self.win.set_default_size(520, 360)
        self.win.connect("delete-event", self.on_close)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        box.set_margin_top(10)
        box.set_margin_bottom(10)
        box.set_margin_start(10)
        box.set_margin_end(10)
        box.pack_start(self.status_label, False, False, 0)

        btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btns.pack_start(self.connect_btn, True, True, 0)
        btns.pack_start(self.disconnect_btn, True, True, 0)
        box.pack_start(btns, False, False, 0)

        view = Gtk.TextView(buffer=self.log)
        view.set_editable(False)
        view.set_monospace(True)
        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        scroll.add(view)
        box.pack_start(scroll, True, True, 0)

        hint = Gtk.Label()
        hint.set_line_wrap(True)
        hint.set_xalign(0)
        hint.set_text(
            "Phone: USB debugging on, EasyTether app open, USB tethering enabled.\n"
            "Test with curl https://example.com — ping to the internet will not work."
        )
        box.pack_start(hint, False, False, 0)
        self.win.add(box)

        self.indicator = None
        if INDICATOR is not None:
            self.indicator = INDICATOR.Indicator.new(
                "easytether-bridge",
                "network-wireless-disconnected-symbolic",
                INDICATOR.IndicatorCategory.SYSTEM_SERVICES,
            )
            self.indicator.set_status(INDICATOR.IndicatorStatus.ACTIVE)
            menu = Gtk.Menu()
            show = Gtk.MenuItem(label="Show")
            show.connect("activate", lambda *_: self.win.present())
            c = Gtk.MenuItem(label="Connect")
            c.connect("activate", self.on_connect)
            d = Gtk.MenuItem(label="Disconnect")
            d.connect("activate", self.on_disconnect)
            q = Gtk.MenuItem(label="Quit")
            q.connect("activate", self.on_quit)
            for item in (show, c, d, q):
                menu.append(item)
                item.show()
            self.indicator.set_menu(menu)
            self.win.hide()
        else:
            self.win.show_all()

        GLib.timeout_add_seconds(1, self.poll_status)

    def append(self, line: str) -> None:
        end = self.log.get_end_iter()
        self.log.insert(end, line if line.endswith("\n") else line + "\n")

    def set_status(self, text: str, connected: bool) -> None:
        self.status_label.set_text(text)
        if self.indicator is not None:
            icon = (
                "network-wireless-connected-symbolic"
                if connected
                else "network-wireless-disconnected-symbolic"
            )
            self.indicator.set_icon_full(icon, text)

    def on_connect(self, *_args) -> None:
        if self.proc and self.proc.poll() is None:
            return
        if not Path(BRIDGE).exists():
            self.append(f"missing {BRIDGE}; run linux/install.sh")
            return
        sudo = shutil.which("sudo") or "sudo"
        cmd = [sudo, "-n", BRIDGE, "-v"]
        self.append("> " + " ".join(cmd))
        try:
            self.proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        except OSError as e:
            self.append(str(e))
            return
        self.connect_btn.set_sensitive(False)
        self.disconnect_btn.set_sensitive(True)
        GLib.io_add_watch(self.proc.stdout, GLib.IO_IN | GLib.IO_HUP, self.on_output)

    def on_output(self, source, condition) -> bool:
        if condition & GLib.IO_IN:
            line = source.readline()
            if line:
                self.append(line.rstrip("\n"))
                return True
        if self.proc and self.proc.poll() is not None:
            self.append(f"bridge exited {self.proc.returncode}")
            self.proc = None
            self.connect_btn.set_sensitive(True)
            self.disconnect_btn.set_sensitive(False)
            return False
        return True

    def on_disconnect(self, *_args) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.proc = None
        self.connect_btn.set_sensitive(True)
        self.disconnect_btn.set_sensitive(False)
        self.set_status("Disconnected", False)

    def on_close(self, *_args):
        if self.indicator is not None:
            self.win.hide()
            return True
        self.on_quit()
        return False

    def on_quit(self, *_args) -> None:
        self.on_disconnect()
        Gtk.main_quit()

    def poll_status(self) -> bool:
        up = iface_up()
        if up:
            self.set_status(f"Connected — {IFNAME} via {GW}", True)
        elif self.proc and self.proc.poll() is None:
            self.set_status("Connecting…", False)
        else:
            self.set_status("Disconnected", False)
        return True


def main() -> int:
    App()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
