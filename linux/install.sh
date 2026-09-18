#!/bin/bash
# Install easytether-bridge + tray on Linux. Run from the repo root as root.
set -euo pipefail
PREFIX="${PREFIX:-/usr/local}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ "$(uname -s)" != "Linux" ]]; then
	echo "linux/install.sh is for Linux; use ./install.sh on macOS" >&2
	exit 1
fi
if [[ "$(id -u)" -ne 0 ]]; then
	echo "re-run as root: sudo $0" >&2
	exit 1
fi

make
install -d "$PREFIX/bin"
install -m 755 easytether-bridge "$PREFIX/bin/easytether-bridge"
install -m 755 gui/easytether-tray.py "$PREFIX/bin/easytether-tray"

DESKTOP_BODY="[Desktop Entry]
Name=EasyTether
Comment=USB tethering via the EasyTether Android app
Exec=$PREFIX/bin/easytether-tray
Icon=network-wireless
Terminal=false
Type=Application
Categories=Network;
StartupNotify=false
X-GNOME-Autostart-enabled=true
"

install -d /usr/share/applications /etc/xdg/autostart
printf '%s' "$DESKTOP_BODY" > /usr/share/applications/easytether-tray.desktop
printf '%s' "$DESKTOP_BODY" > /etc/xdg/autostart/easytether-tray.desktop

# The 2018 vendor package also claims tun-easytether via udev. Mask it so
# easytether-bridge is the only host driver.
if [[ -f /lib/systemd/system/easytether-usb@.service ]]; then
	systemctl mask easytether-usb@.service >/dev/null 2>&1 || true
fi

OWNER="${SUDO_USER:-}"
if [[ -z "$OWNER" || "$OWNER" == "root" ]]; then
	OWNER="$(loginctl list-users --no-legend 2>/dev/null | awk '$2!="root"{print $2; exit}')"
fi
if [[ -z "$OWNER" ]]; then
	OWNER="osis"
fi

install -d /etc/systemd/system
sed "s/__EASYTETHER_USER__/${OWNER}/g" \
	"$ROOT/linux/easytether-bridge.service" \
	> /etc/systemd/system/easytether-bridge.service
install -m 644 "$ROOT/linux/99-easytether-bridge.rules" \
	/etc/udev/rules.d/99-easytether-bridge.rules
# Same filename as the vendor package: a /dev/null link in /etc disables it.
ln -sfn /dev/null /etc/udev/rules.d/99-easytether-usb.rules
udevadm control --reload-rules 2>/dev/null || true
systemctl daemon-reload
systemctl enable easytether-bridge.service >/dev/null
# Hand-started copies fight the phone (one tunnel client). Prefer the unit.
systemctl stop easytether-bridge.service >/dev/null 2>&1 || true
if command -v killall >/dev/null 2>&1; then
	killall -TERM easytether-bridge >/dev/null 2>&1 || true
	sleep 1
	killall -KILL easytether-bridge >/dev/null 2>&1 || true
fi
systemctl start easytether-bridge.service

install -d /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/unmanaged-easytether.conf << 'EOF'
[keyfile]
unmanaged-devices=interface-name:tun-easytether
EOF
if command -v nmcli >/dev/null 2>&1; then
	nmcli connection delete easytether >/dev/null 2>&1 || true
	systemctl reload NetworkManager 2>/dev/null || true
fi

SUDO_USER_NAME="${SUDO_USER:-$OWNER}"
if [[ -n "$SUDO_USER_NAME" && "$SUDO_USER_NAME" != "root" ]]; then
	cat > /etc/sudoers.d/easytether-bridge << EOF
$SUDO_USER_NAME ALL=(root) NOPASSWD: $PREFIX/bin/easytether-bridge
$SUDO_USER_NAME ALL=(root) NOPASSWD: /usr/bin/systemctl start easytether-bridge.service, /usr/bin/systemctl stop easytether-bridge.service, /usr/bin/systemctl restart easytether-bridge.service
EOF
	chmod 440 /etc/sudoers.d/easytether-bridge
fi

echo "installed $PREFIX/bin/easytether-bridge and easytether-tray"
echo "udev starts easytether-bridge when an ADB phone is plugged in"
echo "tray: easytether-tray   logs: journalctl -u easytether-bridge -f"
