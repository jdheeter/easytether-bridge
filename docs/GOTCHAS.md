# Gotchas and debugging playbook

Things that cost real time to find. Most of them share a shape: **something
reports success and silently does nothing**, and the failure only surfaces
later under a condition you were not testing.

---

## Protocol

### `ping` to the internet never works, and that is not a bug

The Android app NATs through ordinary sockets because an unprivileged app
cannot open raw ones, so only TCP and UDP traverse. `ping 8.8.8.8` times out
forever even when everything is perfect. `ping 192.168.117.1` works because the
phone answers that locally and synthetically.

**Never use ping as a health check here.** Half a day can go into "debugging"
a tunnel that was working the whole time. Use `curl`, or the DNS test in
`test/protocol.c`.

### A desynchronised record stream cannot be recovered

If you read a length ≥ 1515 you have lost framing, and there is no resync
marker to hunt for — the payload is arbitrary binary. Drop the connection, as
the vendor does. Any attempt to skip forward and re-lock will silently corrupt
traffic.

### Only one client at a time

The phone serves a single tunnel. A second daemon does not get an error — it
connects, sits there getting no DHCP response, and gives up after 60 seconds
with a message about USB tethering being off, which sends you looking in
entirely the wrong place. This bit us when a hand-started daemon and the
LaunchDaemon were both loaded; `install.sh` now stops everything before it
starts anything.

### Ethernet padding must be trimmed before handing packets up

Frames arrive padded to 60 bytes. Forwarding `frame_len - 14` bytes to the
tunnel interface appends garbage after short datagrams. Trim to the IP header's
total-length field. `handle_ipv4()` does this.

---

## macOS specifics

### The scoped-resolver trap

Publishing `ServerAddresses` under `State:/Network/Service/<id>/DNS` is
accepted, appears in `scutil --dns`, and **is never used** for ordinary
lookups. configd builds the global resolver from services listed in
`Setup:/Network/Global/IPv4:ServiceOrder`; a utun published by an ordinary
daemon is not in that list, so the entry ends up scoped to the interface:

```
resolver #2
  nameserver[0] : 192.168.117.1
  if_index : 22 (utun4)
  flags    : Scoped          ← only for queries explicitly bound to utun4
```

Everything looks fine, because DNS is quietly still coming from whatever real
interface is primary. Turn that interface off and nothing resolves.

The fix is `SupplementalMatchDomains = [""]` plus `SupplementalMatchOrders` —
an empty match domain matches every name, and supplemental resolvers are merged
regardless of service order. This is the mechanism VPNs use for split DNS.
Confirmed working when the global configuration shows:

```
ServerAddresses : 192.168.117.1
__CONFIGURATION_ID__ : Supplemental: com.mobile-stream.easytether-bridge 0
```

### `scutil list` takes a regex, not a glob

`list State:/Network/Service/com.example/*` matches nothing, because `*` is a
quantifier applied to `/`. This produced a false "no keys" reading that sent
the DNS investigation down a completely wrong path for a while. Use
`list State:/Network/Service/.*` and grep, or `show <exact key>`.

### A utun cannot appear in System Settings ▸ Network

`SCNetworkInterfaceCopyAll()` returns only interfaces backed by a real IOKit
network device — on a representative machine, eight entries typed `Ethernet`,
`IEEE80211` and `Bridge`. No utun. So `SCNetworkServiceCreate()` has nothing to
bind to, and adding the service ID to `ServiceOrder` does nothing because the
entry names a service with no interface. Editing the persistent preferences to
try is risk with no payoff. `feth` is no better.

### Point-to-point interfaces want a /32

`ifconfig utunN inet <addr> <peer> netmask 255.255.255.0` is asking for a
confusing `SIOCAIFADDR` failure. Give a utun a /32 and name the gateway as the
peer — that installs the host route that makes the gateway reachable, and the
split-default halves cover everything else. Nothing else exists on the link, so
no subnet route is needed.

### `launchctl bootstrap` does not start an event-triggered job

Bootstrap only *registers* it. A job triggered by `LaunchEvents` IOKit matching
starts on the event — which already fired if the device is plugged in. After
updating the binary you must `launchctl kickstart -k system/<label>`, or the
update does not take effect until the next replug. `install.sh` does this.

### The split-default trick and other tunnels

`0.0.0.0/1` + `128.0.0.0/1` beat an existing default route on specificity
without deleting it, so the previous connection returns by itself when the
interface goes away. Safe here in a way it is not for a VPN, because our
transport is USB rather than IP — there is no possibility of routing the tunnel
through itself.

Do **not** fall back to `route change` when the `add` fails. Our routes die
with the utun, so a surviving `0.0.0.0/1` belongs to somebody else — usually a
VPN using the same trick — and taking it breaks their tunnel with no way to
give it back.

### IOUserEthernet is gone for good

`IOEthernetControllerCreate` opens `IOUserEthernetResourceUserClient`, which the
kernel authorises with an AMFI check for `com.apple.networking.ethernet.user-access`
— Apple-private, not obtainable through any provisioning profile. On Intel the
check historically fell through to a plain root check, which is why the
vendor's driver worked there. Rebuilding for arm64 or re-signing changes
nothing.

---

## Bugs that were introduced by "fixes"

Worth reading before making the same class of change.

### The RFC-correctness regression

`send_dhcp()` originally took one `server` argument and passed it to
`dhcp_build()` **twice** — once as the DHCP option-54 value and once as the
datagram's destination IP. Making the renewal RFC-2131-correct (RENEWING must
not carry a server-identifier option) meant passing `server = 0`, which also
zeroed the destination. Renewals went to `0.0.0.0`, were undeliverable, and
silently failed forever until the lease expired.

The lesson is the API shape, not the RFC: one parameter meaning two things is a
bug waiting for its second caller. They are separate arguments now, and both
`test/unit.c:test_dhcp_renew` and a live renewal exchange in the protocol test
pin it. The mock also complains about any DHCP packet addressed to `0.0.0.0`.

### The unclamped poll timeout

`timeout = (int)(deadline - now)` on a 64-bit millisecond delta. An infinite
lease put `renew_at` decades out, the cast went negative, and `poll()` blocked
forever with the timers dead. The poll timeout is capped at one second now,
which also bounds `SIGTERM` latency.

### State surviving a reconnect

`configured` was not reset at the start of a session, so after a reconnect the
daemon announced "tethering is up" against a utun it had not configured yet.
Everything per-session is now reset at the *top* of `run_session()`.

---

## Debugging playbook

Start here, in this order.

```bash
./check.sh                       # the whole picture in one screen
```

Then, narrowing down:

| Question | Command |
| --- | --- |
| Is the phone visible and authorized? | `adb devices -l` — wants `device`, not `unauthorized` |
| Is the app actually listening? | `adb shell cat /proc/net/unix \| grep easytether` |
| Does the protocol work at all? | `./test/protocol 5037 --dns` — no root needed |
| Is the interface configured? | `ifconfig utunN` |
| Where does traffic go? | `route -n get 8.8.8.8` |
| Is DNS actually ours? | `scutil --dns \| head -20`, and look for `flags: Scoped` |
| What does the global config say? | `echo 'show State:/Network/Global/DNS' \| scutil` |
| Is traffic really leaving via the phone? | `curl -s https://api.ipify.org` — expect the carrier's address |
| What is on the tunnel? | `sudo tcpdump -i utunN -n` |
| What did the daemon say? | `tail -f /var/log/easytether-bridge.log`, or run it with `-v` |

`./test/protocol 5037 --dns` is the highest-value single command: it exercises
the entire protocol against the real phone without root and without touching
the system's network configuration, so it cleanly separates "the protocol is
broken" from "the OS integration is broken".
