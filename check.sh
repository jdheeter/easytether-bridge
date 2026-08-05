#!/bin/bash
#
# Reports the state of the tether: interface, routes, resolver, and whether
# traffic is actually leaving through the phone.  Needs no root.
#
#   ./check.sh
#
SERVICE=com.mobile-stream.easytether-bridge
GW=192.168.117.1

say()  { printf '\n\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mno\033[0m   %s\n' "$1"; }
note() { printf '       %s\n' "$1"; }

say "Interface"
IF=$(ifconfig 2>/dev/null | awk '/^utun/{name=substr($1,1,length($1)-1)} /192\.168\.117\./{print name; exit}')
if [[ -n $IF ]]; then
	ok "$IF is up as $(ipconfig getifaddr "$IF" 2>/dev/null || echo 192.168.117.2)"
else
	bad "no utun holding a 192.168.117.x address -- is the daemon running?"
	echo
	echo "start it with:  sudo ./easytether-bridge -v"
	exit 1
fi

say "Routing"
if netstat -rn -f inet | grep -qE "^(0/1|default).*$IF"; then
	ok "0.0.0.0/1 goes through $IF"
else
	bad "0.0.0.0/1 does not go through $IF"
fi
VIA=$(route -n get 8.8.8.8 2>/dev/null | awk '/interface:/{print $2}')
[[ $VIA == "$IF" ]] && ok "internet traffic is routed to $IF" \
                    || bad "internet traffic goes to $VIA, not $IF"

say "Resolver"
# scutil's `list` takes a regular expression, not a shell glob.
KEYS=$(echo "list State:/Network/Service/.*" | scutil 2>/dev/null)
if grep -q "$SERVICE/DNS" <<<"$KEYS"; then
	ok "the daemon published its resolver entry"
else
	bad "the daemon published no resolver entry"
	note "is the daemon running? see: sudo ./easytether-bridge -v"
fi

if echo "show State:/Network/Service/$SERVICE/DNS" | scutil 2>/dev/null \
   | grep -q SupplementalMatchDomains; then
	ok "it is registered as a catch-all supplemental resolver"
else
	bad "no supplemental match domain"
	note "without it configd ignores this resolver, because the service is not"
	note "in Setup:/Network/Global/IPv4:ServiceOrder. Rebuild and restart."
fi

if scutil --dns 2>/dev/null | grep -q "$GW"; then
	ok "the system resolver list includes the phone ($GW)"
else
	bad "the system resolver list does not mention the phone"
	note "DNS is currently coming from another interface, so it will break"
	note "the moment you turn Wi-Fi off"
fi

PRIMARY=$(echo 'show State:/Network/Global/IPv4' | scutil 2>/dev/null | awk '/PrimaryInterface/{print $3}')
note "macOS considers ${PRIMARY:-nothing} the primary interface"

say "Does traffic actually reach the internet through the phone?"
IP=$(curl -s -m 15 https://api.ipify.org 2>/dev/null)
if [[ -n $IP ]]; then
	ok "public address is $IP"
	note "if that is your mobile carrier's address, the tether is carrying your traffic"
else
	bad "could not reach the internet"
fi

if dig +short +time=5 +tries=1 @"$GW" example.com >/dev/null 2>&1; then
	ok "the phone answers DNS queries directly"
else
	bad "the phone did not answer a DNS query"
fi

say "Reminders"
note "ping to the internet will never work -- EasyTether carries only TCP and UDP."
note "test with curl. ping $GW does work."
note "utun interfaces never appear in System Settings > Network. That is normal."
