# Architecture

2769 lines of C11 across seven modules, plus 1360 lines of tests. No
dependencies beyond libc and two Apple frameworks. Single process, single
thread, one `poll()` loop.

Read [PROTOCOL.md](PROTOCOL.md) first — the shape of this code follows directly
from the shape of the protocol.

---

## 1. The one-sentence version

Bridge a byte stream carrying Ethernet frames onto a layer-3 tunnel interface,
terminating ARP and DHCP in userspace because the interface we can create on
this OS is layer 3 and the protocol is layer 2.

```
   phone (EasyTether app, USB tethering on)
     │  USB
   adbd  ←→  adb server on 127.0.0.1:5037
     │  localabstract:easytetherx
     │
   ┌─┴──────────────────────────────────────────┐
   │ main.c     process, CLI, reconnect loop    │
   │ adb.c      open the stream                 │
   │ proto.c    record framing, ARP, DHCP       │
   │ bridge.c   one session: loop + state       │
   │ netcfg.c   address, routes, resolver       │
   │ utun.c     the kernel interface            │
   └─┬──────────────────────────────────────────┘
     │
   utunN  ←→  macOS network stack
```

## 2. Module map

| Module | Lines | Portable? | Responsibility |
| --- | --- | --- | --- |
| `src/proto.c/h` | 643 | **Yes, entirely** | Record framing, Ethernet, ARP, DHCP encode/decode, checksums, byte order. No syscalls, no platform headers beyond `<arpa/inet.h>`. |
| `src/util.c/h` | 259 | Mostly | Logging, the compacting byte FIFO, whole-buffer socket IO, monotonic clock, formatting. |
| `src/adb.c/h` | 283 | Mostly | ADB host-protocol client. BSD sockets + `posix_spawn`. |
| `src/bridge.c/h` | 825 | Partly | One session: event loop, DHCP/ARP state machine, forwarding. The logic is portable; the I/O multiplexing is not. |
| `src/main.c` | 230 | Partly | The process: arguments, privileges, signals, locating the ADB server, reconnect loop. |
| `src/utun.c/h` | 152 | **No** | The virtual interface. Wholly macOS-specific. |
| `src/netcfg.c/h` | 377 | **No** | Address, routes and resolver. Wholly macOS-specific. |

Line counts include headers. The split is deliberate: **`proto.c` is the
valuable part and it is pure.** A port reuses it unchanged. `utun.c` and `netcfg.c` are the two files that get
rewritten. See [PORTING.md](PORTING.md).

## 3. Data flow

### Inbound (phone → host)

```
read(tunfd) ──► sbuf in ──► parse_incoming() ──► handle_frame()
                                                     │
                        ┌────────────────────────────┼──────────────────┐
                        ▼                            ▼                  ▼
                   ETHERTYPE_ARP            ETHERTYPE_IPV4          anything else
                   handle_arp()          dhcp_parse_frame()            drop
                        │                   │          │
                   learn gateway MAC,   is DHCP?    not DHCP
                   answer requests      handle_dhcp() handle_ipv4()
                                                          │
                                              strip 14-byte header,
                                              trim Ethernet padding to
                                              the IP total-length field,
                                                          ▼
                                                   utun_write(AF_INET)
```

### Outbound (host → phone)

```
utun_read() ──► IPv4 only ──► eth_build(dst=gw_mac, src=our_mac)
                                          │
                                    queue_frame()  (prepend u16 LE len, pad)
                                          │
                                       sbuf out ──► write(tunfd)
```

### Buffers and back-pressure

Two `struct sbuf` FIFOs (`src/util.h`), each a compacting buffer with a hard
cap.

* `in` — 64 KB initial, 1 MB cap. Holds partial records between reads.
* `out` — 64 KB initial, 2 MB cap. Holds records waiting for a writable socket.

Back-pressure is one line in the poll setup: **utun is only polled for read
while the outbound queue is below half its cap.** Without that, a fast local
sender and a slow USB link would grow `out` until the cap and then drop packets
silently. `pump_utun()` also returns early once it has pushed the queue past the
halfway mark, so the writer gets a turn without waiting for the next poll.

`sbuf_reserve()` may `realloc`. Any pointer into a buffer is invalid across a
call that can append to *that* buffer. Inbound parsing holds a pointer into
`in` while calling handlers that only ever append to `out`, which is why that is
safe — if you ever make a handler touch `in`, that assumption breaks.

## 4. The state machine

Four states, in `src/bridge.c`:

```
                    ┌──────────────┐
   session start ──►│ ST_DISCOVER  │  broadcast DHCPDISCOVER, retry with
                    └──────┬───────┘  exponential backoff, 60s budget
                       OFFER│
                    ┌──────▼───────┐
                    │ ST_REQUEST   │  DHCPREQUEST, 4 retries then start over
                    └──────┬───────┘
                        ACK│  apply_lease(): configure the interface
                    ┌──────▼───────┐
                    │ ST_ARP       │  who-has <gateway>, retry 0.5s then 3s
                    └──────┬───────┘
                   ARP rply│  bridge_ready(): install the default route
                    ┌──────▼───────┐
                    │ ST_UP        │  forwarding; renew at T1 if the lease
                    └──────────────┘  is finite
```

Rules that are easy to break and that the tests pin:

* **The default route goes in at `bridge_ready()`, not at `apply_lease()`.**
  Installing it before the gateway's MAC is known points the whole machine at
  an interface that cannot yet send anything — every packet black-holes for as
  long as ARP takes.
* **`bridge_ready()` is only reachable from `ST_ARP`** (or directly from
  `apply_lease()` when the MAC is already known). Promoting out of
  `ST_DISCOVER`/`ST_REQUEST` — which is tempting because the gateway keeps
  ARPing at us after a NAK — makes the ACK that follows look like a renewal of
  a lease we no longer hold.
* **A renewal that changes our address drops the session** rather than
  reconfiguring in place, because reconfiguring would leave the old address
  aliased on the interface. The reconnect loop rebuilds cleanly.
* **`enter_discover()` resets `started`, `renew_at` and `lease_expires`.** A
  NAK mid-session otherwise inherits an exhausted 60-second budget and the
  session dies one second later with a misleading error.

### Timers

A single `deadline` for retransmits plus `renew_at` and `lease_expires`.
`next_deadline()` takes the minimum; **the poll timeout is then capped at one
second**. That cap is not cosmetic:

* An infinite or very long lease puts `renew_at` far enough out that the
  millisecond delta overflows `int`, and a negative timeout means `poll()`
  blocks forever — the timers simply die.
* It bounds how long a `SIGTERM` can sit unnoticed, since the handler only sets
  a flag and there is no self-pipe.

## 5. Session lifecycle

`bridge_run_session()` owns one connection. `main()` loops over it with
exponential backoff to 15 seconds.

The session state lives in a `struct bridge` allocated fresh inside
`bridge_run_session()`. State surviving a reconnect used to be a reliable
source of bugs — a stale `configured` flag made the daemon announce "tethering
is up" against a utun it had not configured yet — so the lifetime is now
structural rather than a reset list somebody has to remember to update.

After six consecutive sessions that never got as far as a lease, the daemon
exits **successfully**. Under launchd that means it is not restarted, and it
stays quiet until the next device-attach event rather than respawning forever
against an unplugged phone.

Cleanup is mostly automatic and deliberately so: closing the utun destroys the
interface, and every route bound to it disappears with it. That is also why
`netcfg_set_default_route()` refuses to `route change` an existing
`0.0.0.0/1` — our own routes cannot survive us, so a surviving one belongs to
somebody else, typically a VPN using the same split-default trick.

## 6. Why layer 2 in userspace

The protocol carries Ethernet. The interface we can create carries IP. The gap
has to be filled somewhere:

* The vendor filled it in the kernel by handing frames to a virtual NIC
  (`IOUserEthernet`), so macOS ran its own ARP and DHCP client. That API is now
  gated behind an Apple-private entitlement.
* We fill it in userspace: a small ARP responder plus a DHCP client, in
  `proto.c` and the state machine above.

**This is the piece to delete if your platform offers a layer-2 virtual
adapter.** With a TAP-style device the OS runs DHCP and ARP itself and the
daemon becomes a pure frame bridge — roughly half of `main.c` and most of
`proto.c` become dead code. On macOS the candidate is `feth`/`if_fake` driven
through BPF (what ReRNDIS does); on Windows it is TAP-Windows6. See
[PORTING.md](PORTING.md).

## 7. Extension points

Ranked by value-to-effort.

1. **A `feth` backend for macOS.** Layer 2 via `SIOCIFCREATE` +
   `IF_FAKE_S_CMD_SET_PEER` + `/dev/bpf`. Root only, no entitlement. Deletes
   the ARP/DHCP shim and lets the OS do addressing. The interface still would
   not appear in System Settings, so the only real win is less code — but it is
   a large chunk of less code.
2. **Direct USB ADB transport.** Removes the dependency on the Android platform
   tools. Behind the same `adb_open_stream()` interface. Costs an ADB protocol
   implementation with RSA auth, and exclusive interface access.
3. **IPv6.** Needs SLAAC and neighbour discovery in the shim. Only worth doing
   if a phone is ever observed offering it; none has been.
4. **Multiple phones.** The state machine is already per-session; it would need
   an outer loop over serials and one utun each.
5. **Bluetooth transport.** The vendor ships one over proprietary RFCOMM.
   Entirely unexamined.

Deliberately *not* worth doing: getting the interface into System Settings ▸
Network. Proven impossible for a utun — `SCNetworkInterfaceCopyAll()` returns
only IOKit-backed devices, so there is no `SCNetworkInterfaceRef` to bind a
service to. The only routes are a NetworkExtension system extension (appears
under VPN, needs a paid Developer ID) or a NetworkingDriverKit DEXT (needs an
Apple-approved managed entitlement).

## 8. Testing

Three layers, all runnable without a phone except where noted.

| Command | What it covers |
| --- | --- |
| `make check` | `test/unit.c` — record arithmetic at every boundary, the IPv4 checksum against the standard worked example, Ethernet padding, ARP round trip, DHCP encode walked layer by layer, DHCP decode including truncation and foreign-xid rejection, and the RENEWING request's exact option set. |
| `make protocol-test` | `test/protocol.c` against `test/mockphone.c` — a real conversation over a socket: ADB handshake, hello, DHCP, renewal, ARP, ICMP. The mock independently validates every record it receives and complains about anything off-spec. |
| `./test/protocol 5037 --dns` | The same client against a **real phone**, no root needed. Adds a DNS lookup that must reach the internet. This is how the protocol was confirmed. |

What none of them cover: `utun.c`, `netcfg.c` and the `poll()` loop, all of
which need root. For those, run the daemon against the mock:

```bash
./test/mockphone 15037                        # terminal 1
sudo ./easytether-bridge -p 15037 -D -R -v    # terminal 2
ping 192.168.117.1                            # terminal 3
```

`-D` leaves DNS alone and `-R` skips the default route, so a mock that forwards
nothing cannot black-hole the machine.

**The mock is the porting harness.** It speaks the phone's half of the protocol
over plain TCP, so a Windows build can be brought up against it with no phone,
no USB and no ADB — see [PORTING.md](PORTING.md) §6.

## 9. Conventions

* Linux kernel style: tabs, 8-column indent, ~85 column limit, `snake_case`,
  one declaration block at the top of a function.
* Comments explain *why*, and specifically why something non-obvious is
  necessary. Several of them encode a bug that has already been fixed once;
  they are load-bearing.
* Errors are logged where they happen with enough context to act on, and
  propagated as `-1`. The daemon prefers degrading (warn, carry on) to dying,
  except where continuing would be wrong — a desynchronised tunnel drops the
  session, because there is no way to resynchronise a stream whose framing you
  have lost.
