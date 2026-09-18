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

install -d /usr/share/applications
cat > /usr/share/applications/easytether-tray.desktop << EOF
[Desktop Entry]
Name=EasyTether
Comment=USB tethering via the EasyTether Android app
Exec=$PREFIX/bin/easytether-tray
Icon=network-wireless
Terminal=false
Type=Application
Categories=Network;
StartupNotify=false
EOF

install -d /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/unmanaged-easytether.conf << 'EOF'
[keyfile]
unmanaged-devices=interface-name:tun-easytether
EOF
if command -v nmcli >/dev/null 2>&1; then
	nmcli connection delete easytether >/dev/null 2>&1 || true
	systemctl reload NetworkManager 2>/dev/null || true
fi

SUDO_USER_NAME="${SUDO_USER:-}"
if [[ -n "$SUDO_USER_NAME" && "$SUDO_USER_NAME" != "root" ]]; then
	echo "$SUDO_USER_NAME ALL=(root) NOPASSWD: $PREFIX/bin/easytether-bridge" \
		> /etc/sudoers.d/easytether-bridge
	chmod 440 /etc/sudoers.d/easytether-bridge
fi

echo "installed $PREFIX/bin/easytether-bridge and easytether-tray"
echo "start the tray from the app menu, or: easytether-tray"
echo "or: sudo easytether-bridge -v"
