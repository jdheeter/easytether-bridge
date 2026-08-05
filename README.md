# easytether-bridge

A native macOS host driver for [EasyTether](http://www.mobile-stream.com/easytether/),
written because the vendor's driver cannot work on Apple Silicon.

Nothing here is a patch to the vendor's software. It speaks the same protocol
to the same Android app and presents the result to macOS a different way.

> **Independent project.** Not affiliated with, endorsed by, or supported by
> Mobile Stream. "EasyTether" is their trademark and is used here only to say
> what this interoperates with. This is a replacement for the *host-side
> driver* only: it still requires their Android app, licensed and running, and
> it neither replaces nor circumvents any part of it. The protocol was
> recovered by disassembling the vendor's own macOS host binaries purely to
> achieve interoperability with hardware they no longer support; no vendor code
> is included or redistributed. If you find this useful, buy the app.

---

## Why the vendor's driver doesn't work

The installed package (`easytether-yosemite-b16.pkg`, the newest one they
publish, dated December 2023) puts two binaries in `/usr/local/bin`. Both are
**thin x86_64** — no arm64 slice — and both fail identically on this Mac:

```
easytether-usb[43820]: cannot create user_ethernet instance (lack of permissions?)
easytether-usb-monitor[33782]: /usr/local/bin/easytether-usb: 69
```

That message is emitted the moment the daemon starts, every time the phone is
plugged in. It never reaches the USB or tethering code at all.

The cause is not Rosetta and not the missing arm64 slice. The daemon builds its
virtual network interface with `IOEthernetControllerCreate`, a private IOKit
SPI that opens the `IOUserEthernetResourceUserClient` kernel user client. That
open is authorised in the kernel by an AMFI check for the entitlement
**`com.apple.networking.ethernet.user-access`**, which is Apple-private and not
obtainable through any provisioning profile. On Intel the check historically
fell through to a plain root check, so an unsigned root daemon worked. On Apple
Silicon all code is signed and entitlements are enforced non-forgeably: an
unsigned binary gets an ad-hoc signature with no entitlements, the check fails,
and `IOServiceOpen` returns `kIOReturnNotPermitted`.

So it is not fixable by rebuilding for arm64, re-signing, or changing
permissions. A native arm64 build would fail in exactly the same place. The
interface has to be created some other way.

## What this does instead

`utun`, the interface WireGuard and every modern macOS VPN uses. It needs root
and nothing else — no kext, no entitlement, no SIP changes, no system
extension.

The catch is that `utun` is layer 3 and the EasyTether tunnel carries raw
Ethernet frames. The vendor's driver got ARP and DHCP for free by handing those
frames to a real virtual NIC. This daemon terminates layer 2 itself: it has a
small ARP responder and a DHCP client, strips and synthesises Ethernet headers,
and hands bare IP packets to `utun`.

```
   phone (EasyTether app, USB tethering on)
     |  USB
   adbd  <->  adb server on 127.0.0.1:5037
     |  localabstract:easytetherx
   [ easytether-bridge: framing, ARP, DHCP client ]
     |
   utunN  <->  macOS network stack
```

Going through the ADB server rather than driving USB directly means device
enumeration and the RSA authorization are already solved, and the daemon
coexists with a running `adb` instead of fighting it for exclusive access to
the interface.

## The protocol

Undocumented, and not published anywhere — the socket name `easytetherx`
returns zero hits on the open web. This was recovered by disassembling the
vendor's own `easytether-usb` and `easytether-local`, which agree byte for
byte.

1. Open an ADB stream to `localabstract:easytetherx`.
2. Send a four byte hello: `51 b7 04 00` (u32 little-endian `0x0004b751`).
3. From then on, every packet in both directions is a record:

   ```
   +--------+--------+------------------------+---------+
   | len lo | len hi |  raw Ethernet frame    | padding |
   +--------+--------+------------------------+---------+
   ```

   `len` is little-endian and counts only the frame. The record occupies
   `(len + 5) & ~3` bytes, i.e. `2 + len` rounded up to a multiple of four, so
   there are 0–3 zero padding bytes. `len` must be under 1515; the vendor
   treats anything larger as a protocol error and drops the tunnel. Records
   with `len <= 13` are too short to hold an Ethernet header and are skipped.

The phone is an ordinary Ethernet peer on the far side: it serves DHCP on a
fixed `192.168.117.0/24`, takes `.1` as the gateway, and answers ARP.

## Building and installing

```bash
make && make check
sudo ./install.sh
```

That installs `/usr/local/bin/easytether-bridge` plus a LaunchDaemon that
starts it when a phone with an ADB interface is plugged in. `sudo ./install.sh
--no-daemon` skips the LaunchDaemon; `sudo ./install.sh --uninstall` removes
everything.

**To refresh after changing the code, run the same command again.** It stops
whatever is running (including a hand-started foreground daemon), rebuilds,
reinstalls the binary, and restarts the LaunchDaemon. The last step matters:
`launchctl bootstrap` only *registers* the job, and this one starts on an IOKit
match event that has already fired if the phone is plugged in — so the script
also `kickstart`s it, otherwise an update would not take effect until the next
replug.

Never run one by hand while the LaunchDaemon is loaded. The phone serves a
single client, so the second one sits there failing DHCP until it gives up.

Before it can connect, all three of these must be true:

1. USB debugging is on in the phone's developer options.
2. `adb devices` shows the phone as `device` — not `unauthorized`. If it says
   unauthorized, unlock the phone and tap **Allow** on the RSA prompt, ticking
   "always allow from this computer".
3. The EasyTether app is open on the phone with USB tethering switched on.

Then watch it work:

```bash
sudo easytether-bridge -v
```

## Testing without a phone

`test/mockphone` impersonates the ADB server and the EasyTether service, hands
out a lease, answers ARP, and replies to pings aimed at the gateway. It checks
the daemon's output strictly and complains about anything off-spec.

```bash
make mock
./test/mockphone 15037                        # terminal 1
sudo ./easytether-bridge -p 15037 -D -R -v    # terminal 2
ping 192.168.117.1                            # terminal 3
```

`-D` leaves DNS alone and `-R` skips the default route, so the mock cannot
black-hole your real networking.

Three checks need no root and no phone:

```bash
make check           # unit tests: record arithmetic, checksums, ARP, DHCP
make protocol-test   # a full conversation against the mock phone
make windows-check   # the protocol core still cross-compiles for Windows
```

The last one needs `brew install mingw-w64` and guards the property a port
depends on: `src/proto.c` has no platform dependency at all and builds
unmodified for 32- and 64-bit Windows. See [docs/PORTING.md](docs/PORTING.md).

The same client also runs against a **real** phone without root, which is how
the protocol above was confirmed. With the phone plugged in, authorized, and
EasyTether running:

```bash
./test/protocol 5037 --dns
```

That opens the real tunnel, completes DHCP and ARP, pings the gateway, and
then resolves `example.com` through the phone's own resolver — so a passing
run proves the framing is right *and* that traffic is really reaching the
internet. Against the moto g power used to develop this, the phone leases
`192.168.117.2/24`, is the gateway at `192.168.117.1` with MAC
`02:00:6d:73:62:62`, and hands out an infinite lease.

`make protocol-test` runs the real `adb.c` and `proto.c` through an ADB
handshake, the hello, a DHCP discover/offer/request/ack exchange, a lease
renewal, ARP resolution and an ICMP round trip, with the mock independently
validating every record it receives. What that leaves untested without hardware is
`utun.c`, `netcfg.c` and the event loop — which is what the mock plus
`sudo easytether-bridge -p 15037 -D -R -v` covers.

## Things to know

**Ping to the internet will not work, and that is normal.** The EasyTether
Android app NATs through ordinary sockets, because an unprivileged app cannot
open raw ones. Only TCP and UDP cross the tunnel. `ping 8.8.8.8` times out
forever; `ping 192.168.117.1` works because the phone answers that locally.
Test with `curl https://example.com`, not `ping`.

**IPv4 only.** The tether link is IPv4 in practice; IPv6 frames are dropped
rather than half-supported.

**If large transfers stall, try a smaller MTU** with `-m 1400`. The phone
terminates TCP itself rather than forwarding packets, so 1500 should be safe,
but at least one Linux deployment reported needing 1448.

**The interface will not appear in System Settings ▸ Network, and cannot be
made to.** That list is built from `SCNetworkInterfaceCopyAll()`, which returns
only interfaces backed by a real IOKit network device. On this machine that is
eight entries — `en0`–`en7` and `bridge0`, typed `Ethernet`, `IEEE80211` and
`Bridge`. No utun appears, so there is no `SCNetworkInterfaceRef` for
`SCNetworkServiceCreate` to bind a service to. Editing
`Setup:/Network/Global/IPv4:ServiceOrder` does not help either: an entry there
naming a service that has no interface is simply ignored, so it is risk to your
saved network preferences for no gain. `feth` is no better — it is not an
IOKit network device either.

The only two routes into that UI are a NetworkExtension system extension
(which appears under **VPN**, not Other Services, and needs a paid Developer
ID plus notarization) and a NetworkingDriverKit DEXT (which would give a real
`en` entry but needs an Apple-approved managed entitlement). Neither has any
bearing on whether the tether works. `ifconfig`, `netstat -rn` and
`./check.sh` all see it.

**The resolver has to be registered as a catch-all *supplemental* resolver.**
This is the subtlety that made "works on Wi-Fi, dead without it" happen, and it
is worth understanding before touching that code.

Publishing `ServerAddresses` under `State:/Network/Service/<id>/DNS` is not
enough. configd builds the global resolver from the services listed in
`Setup:/Network/Global/IPv4:ServiceOrder`, and a utun published by an ordinary
daemon is not in that list — so the entry is accepted but ends up **scoped to
the interface**:

```
resolver #2
  nameserver[0] : 192.168.117.1
  if_index : 22 (utun4)
  flags    : Scoped          <- only for queries explicitly bound to utun4
```

Ordinary lookups never touch a scoped resolver. DNS keeps silently coming from
whichever real interface is primary, and everything looks fine right up until
that interface goes away, at which point nothing resolves.

Adding `SupplementalMatchDomains = [""]` (an empty match domain matches every
name) plus `SupplementalMatchOrders` fixes it: supplemental resolvers are
merged regardless of service order. This is the same mechanism VPNs use for
split DNS. It then shows up in the global configuration properly:

```
ServerAddresses : 192.168.117.1
__CONFIGURATION_ID__ : Supplemental: com.mobile-stream.easytether-bridge 0
```

The daemon also holds one `SCDynamicStoreRef` open for its whole life rather
than shelling out to `scutil`, so errors are actually checked and the resolver
reverts by itself if the process is killed.

**The vendor's LaunchDaemon is left alone by the installer.** It still fails on
every device attach, which is noise in the log but harmless. To silence it:

```bash
sudo launchctl bootout system /Library/LaunchDaemons/com.mobile-stream.easytether-usb.plist
```

## Troubleshooting

Run it in the foreground with `-v`; it says which of these it hit.

| Symptom | Cause and fix |
| --- | --- |
| `ADB: device unauthorized` | Unlock the phone and tap **Allow** on the RSA prompt. Tick "always allow from this computer" so it sticks. |
| `ADB: no devices/emulators found` | The cable is charge-only, or USB debugging is off in developer options. |
| `... is EasyTether running on the phone with USB tethering enabled?` | The ADB stream opened but nothing is listening on `easytetherx`. Open the app and switch USB tethering on. Confirm with `adb shell cat /proc/net/unix \| grep easytether`. |
| `no DHCP response after 60s` | The socket is there but the app is not serving the link — usually the app is open but tethering is toggled off, or the trial/licence state is blocking it. |
| `cannot reach ADB server on 127.0.0.1:5037` | Run `adb start-server` as your own user first. |
| Works with Wi-Fi on, dead with Wi-Fi off | The resolver never got published, so DNS was quietly coming from Wi-Fi. Run `./check.sh`; if it reports no resolver entries, you are on a build from before that was fixed. |
| Connects, but nothing loads | Check DNS separately: `dig @192.168.117.1 example.com`. If that works and browsing does not, the resolver did not take — see `./check.sh`. |
| `ping 8.8.8.8` fails | Expected. See above — EasyTether carries only TCP and UDP. |

`./check.sh` reports all of this at once — interface, routes, resolver,
primary service, and whether traffic is really leaving through the phone. Run
it first when something looks wrong.

To see the tunnel itself, `tcpdump -i utun6 -n` (substitute the name the daemon
logs at startup).

## Alternatives considered

- **`feth` / `if_fake` + BPF.** A pair of fake Ethernet interfaces created with
  `SIOCIFCREATE`, driven through `/dev/bpf`. Root-only, no entitlement, and it
  gives a real layer 2 interface, so macOS's own DHCP client and ARP would run
  and this daemon would not need its own. This is what ReRNDIS (the successor
  to HoRNDIS) does. It is the better long-term shape and a plausible second
  backend; `utun` was chosen first because it is simpler, has one fewer moving
  part, and is the better-trodden path.
- **`NEPacketTunnelProvider`.** Apple's blessed route, but it needs the
  NetworkExtension entitlement, a Developer ID, notarization and a packaged
  system extension. Heavy for a USB tether.
- **A DriverKit `IOUserNetworkEthernet` DEXT.** The true modern replacement for
  a networking kext, and the only option that yields a real `en` interface in
  the Network pane — but the entitlements are Apple-approval-gated.
- **Reimplementing ADB over USB directly**, as the vendor does through libusb.
  Self-contained, but it means claiming the USB interface exclusively (fighting
  `adb`) and reimplementing RSA host authentication for no practical gain.

## Documentation

Deeper material lives in [`docs/`](docs/):

| Document | For |
| --- | --- |
| [PROTOCOL.md](docs/PROTOCOL.md) | The complete wire specification, the disassembly evidence behind each rule, the values a real phone hands out, and an honest list of what is still unknown. Read this first if you are porting. |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Module map, data flow, the DHCP/ARP state machine and its non-obvious rules, buffering and back-pressure, extension points, testing layers. |
| [PORTING.md](docs/PORTING.md) | What is platform-independent and what is not, and a concrete Windows port guide. |
| [GOTCHAS.md](docs/GOTCHAS.md) | Traps that cost real time, bugs that were introduced by "fixes", and a debugging playbook. |

## Licence

MIT — see [LICENSE](LICENSE). The protocol documentation in
[docs/PROTOCOL.md](docs/PROTOCOL.md) is offered under the same terms.

## Layout

| Path | What it is |
| --- | --- |
| `src/proto.c` | Record framing, Ethernet, ARP, DHCP, checksums. Platform-independent. |
| `src/bridge.c` | One session: event loop, DHCP/ARP state machine, forwarding |
| `src/main.c` | The process: arguments, privileges, reconnect loop |
| `src/utun.c` | utun interface creation and packet I/O |
| `src/netcfg.c` | `ifconfig` / `route` / SystemConfiguration |
| `src/adb.c` | ADB server protocol client |
| `src/util.c` | Logging, the byte FIFO, whole-buffer socket IO |
| `test/testutil.c` | Framing over a socket and the phone's half of DHCP |
| `test/unit.c` | Unit tests for the protocol logic |
| `test/protocol.c` | A full protocol conversation, driven end to end |
| `test/mockphone.c` | A fake phone that answers it |
| `check.sh` | Reports the live state of the tether |
| `docs/` | Protocol spec, architecture, porting guide, gotchas |
