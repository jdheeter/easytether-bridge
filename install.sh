#!/bin/bash
#
# Installs easytether-bridge and, optionally, a LaunchDaemon that starts it
# whenever a phone with an ADB interface is plugged in.
#
#   sudo ./install.sh              # binary + LaunchDaemon
#   sudo ./install.sh --no-daemon  # binary only, start it by hand
#   sudo ./install.sh --uninstall
#
set -euo pipefail

BIN=easytether-bridge
LABEL=com.mobile-stream.easytether-bridge
PREFIX=/usr/local
PLIST=com.mobile-stream.easytether-bridge.plist
PLIST_DEST=/Library/LaunchDaemons/$PLIST
NEWSYSLOG=/etc/newsyslog.d/easytether-bridge.conf
VENDOR_PLIST=/Library/LaunchDaemons/com.mobile-stream.easytether-usb.plist

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

if [[ $EUID -ne 0 ]]; then
	echo "run me with sudo" >&2
	exit 1
fi

uninstall() {
	if [[ -f $PLIST_DEST ]]; then
		launchctl bootout system "$PLIST_DEST" 2>/dev/null || true
		rm -f "$PLIST_DEST"
		echo "removed $PLIST_DEST"
	fi
	rm -f "$PREFIX/bin/$BIN"
	echo "removed $PREFIX/bin/$BIN"
	rm -f "$NEWSYSLOG"
	/usr/sbin/scutil <<-'EOF' >/dev/null 2>&1 || true
	open
	remove State:/Network/Service/com.mobile-stream.easytether-bridge/DNS
	remove Setup:/Network/Service/com.mobile-stream.easytether-bridge/DNS
	remove State:/Network/Service/com.mobile-stream.easytether-bridge/IPv4
	quit
	EOF
	echo "done"
}

case "${1:-}" in
--uninstall) uninstall; exit 0 ;;
esac

want_daemon=1
case "${1:-}" in
"")           ;;
--no-daemon)  want_daemon=0 ;;
*)            echo "unknown option: $1" >&2
              echo "usage: $0 [--no-daemon | --uninstall]" >&2
              exit 2 ;;
esac

# A hand-started daemon and the LaunchDaemon would both try to open the phone's
# tunnel, and the phone only serves one client -- the loser sits there failing
# DHCP until it gives up.  Say so rather than leaving a confusing mess.
if pgrep -f "[e]asytether-bridge" >/dev/null 2>&1; then
	echo "==> stopping running instances"
	launchctl bootout "system/$LABEL" 2>/dev/null || true
	pkill -f "[e]asytether-bridge" 2>/dev/null || true
	sleep 1
fi

echo "==> building"
# Build as the invoking user so object files are not left owned by root.
if [[ -n "${SUDO_USER:-}" ]]; then
	su "$SUDO_USER" -c "cd '$here' && make && make test/unit" >/dev/null
else
	make >/dev/null && make test/unit >/dev/null
fi

echo "==> checking"
./test/unit >/dev/null || { echo "unit tests failed; not installing" >&2; exit 1; }

echo "==> installing $PREFIX/bin/$BIN"
install -d "$PREFIX/bin"
install -m 755 "$BIN" "$PREFIX/bin/$BIN"

if [[ $want_daemon -eq 1 ]]; then
	echo "==> installing $PLIST_DEST"
	install -m 644 "$PLIST" "$PLIST_DEST"
	chown root:wheel "$PLIST_DEST"
	launchctl bootout system "$PLIST_DEST" 2>/dev/null || true
	launchctl bootstrap system "$PLIST_DEST"

	# bootstrap only registers the job; it starts on the IOKit match event,
	# which already fired if the phone is plugged in.  Kick it so an update
	# takes effect now instead of at the next replug.
	launchctl kickstart -k "system/$LABEL" 2>/dev/null || true

	# launchd appends forever; hand the log to newsyslog so it cannot grow
	# without bound.
	cat > "$NEWSYSLOG" <<-'ROTATE'
	# logfilename                     [owner:group] mode count size when flags
	/var/log/easytether-bridge.log                  644   5     1000 *    GN
	ROTATE
	chmod 644 "$NEWSYSLOG"
	echo "    logs: /var/log/easytether-bridge.log (rotated at 1 MB, 5 kept)"
fi

if [[ -f $VENDOR_PLIST ]]; then
	cat <<-EOF

	Note: the vendor's daemon is still installed at
	  $VENDOR_PLIST
	It cannot work on this Mac -- it dies at startup with "cannot create
	user_ethernet instance" every time you plug the phone in -- but it is
	harmless, it just fills the log. To stop it:

	  sudo launchctl bootout system $VENDOR_PLIST

	To put it back:

	  sudo launchctl bootstrap system $VENDOR_PLIST
	EOF
fi

cat <<-EOF

	Installed and (re)started. To refresh after changing the code, just run
	this script again -- it rebuilds, reinstalls and restarts the daemon.

	  sudo ./install.sh          # refresh
	  ./check.sh                 # see what the tether is doing
	  sudo launchctl print system/$LABEL   # launchd's view

	Before it can connect:
	  1. USB debugging on in the phone's developer options
	  2. \`adb devices\` shows your phone as "device", not "unauthorized"
	     (tap Allow on the phone's RSA prompt, ticking "always allow")
	  3. the EasyTether app is open with USB tethering switched on

	Then plug the phone in, or run it in the foreground to watch:
	  sudo $PREFIX/bin/$BIN -v
EOF
