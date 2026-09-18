#!/bin/bash
# Linux counterpart of ./check.sh
set -euo pipefail
GW=192.168.117.1
IF=tun-easytether

say()  { printf '\n\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mno\033[0m   %s\n' "$1"; }
note() { printf '       %s\n' "$1"; }

say "Interface"
if [[ -d /sys/class/net/$IF ]]; then
	ADDR=$(ip -4 -o addr show dev "$IF" | awk '{print $4}' | head -1)
	ok "$IF is up as ${ADDR:-unknown}"
else
	bad "no $IF -- is the daemon running?"
	echo
	echo "start it with:  sudo easytether-bridge -v"
	exit 1
fi

say "Routing"
if ip route | grep -qE "^0\.0\.0\.0/1 .*${IF}"; then
	ok "0.0.0.0/1 goes through $IF"
else
	bad "0.0.0.0/1 does not go through $IF"
fi
VIA=$(ip route get 8.8.8.8 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}' | head -1)
[[ $VIA == "$IF" ]] && ok "internet traffic is routed to $IF" \
                    || bad "internet traffic goes to ${VIA:-unknown}, not $IF"

say "Resolver"
if resolvectl dns "$IF" 2>/dev/null | grep -q "$GW"; then
	ok "systemd-resolved has $GW on $IF"
else
	bad "resolver does not list $GW on $IF"
fi

say "Does traffic actually reach the internet through the phone?"
IP=$(curl -s -m 15 https://api.ipify.org 2>/dev/null || true)
if [[ -n $IP ]]; then
	ok "public address is $IP"
	note "if that is your mobile carrier's address, the tether is carrying your traffic"
else
	bad "could not reach the internet"
fi

if getent hosts example.com >/dev/null 2>&1; then
	ok "DNS lookup for example.com succeeded"
else
	bad "DNS lookup for example.com failed"
fi
