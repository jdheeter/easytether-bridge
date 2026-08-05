# The EasyTether wire protocol

Everything here was recovered by disassembling Mobile Stream's own macOS
binaries. None of it is published anywhere — the socket name `easytetherx`
returns zero hits on GitHub code search and on the open web. Treat this file as
the specification; it is the one artefact a port to another platform cannot do
without.

All of it has been **verified against a real device** (a Motorola moto g power
2022 running EasyTether Pro, `com.mstream.etpc`), not just read out of a
disassembly. `test/protocol.c` performs the whole conversation and is the
executable form of this document.

---

## 1. Transport

The tunnel is an ordinary ADB stream to an abstract-namespace Unix socket
published by the phone app.

```
service name:  localabstract:easytetherx
```

The app listens on the Linux abstract socket `@easytetherx`. You can confirm it
is there before writing a byte:

```
$ adb shell cat /proc/net/unix | grep easytether
0000000000000000: 00000002 00000000 00010000 0001 01 3344167 @easytetherx
```

There are two ways to reach it.

### 1a. Through the local ADB server (what this project does)

Connect a TCP socket to `127.0.0.1:5037` and speak the ADB *host* protocol.
Every request is a 4-hex-digit length prefix followed by the request text; every
reply starts with `OKAY` or `FAIL`, and `FAIL` is followed by another 4-hex-digit
length and a human-readable reason.

```
->  0012 host:transport-any            (or "host:transport:<serial>")
<-  OKAY
->  0019 localabstract:easytetherx
<-  OKAY
<raw bidirectional stream from here on>
```

`0012` is 18, the length of `host:transport-any`; `0019` is 25, the length of
`localabstract:easytetherx`. This is exactly what `adb forward tcp:N
localabstract:easytetherx` does internally, minus the listening socket.

The `FAIL` reasons are the most useful diagnostics in the whole system, and a
port should surface them verbatim:

| Reason | Meaning |
| --- | --- |
| `device unauthorized` | The RSA prompt has not been accepted on the phone. |
| `no devices/emulators found` | Nothing plugged in, or USB debugging is off, or a charge-only cable. |
| *(fails on the second request)* | adbd refused the OPEN: nothing is listening on `easytetherx`, i.e. the app is closed or USB tethering is switched off inside it. |

Advantages: device enumeration, USB driver handling and RSA authorization are
already solved, and you coexist with a running `adb` instead of fighting it for
exclusive access to the interface.

### 1b. Direct USB (what the vendor does)

The vendor's binary speaks ADB itself over libusb/IOUSBLib. The relevant USB
interface is:

```
bInterfaceClass    255  (vendor specific)
bInterfaceSubClass  66
bInterfaceProtocol   1
```

That triple is what both the vendor's LaunchDaemon and ours use for IOKit
device matching. Doing this yourself means implementing the ADB device protocol
(`CNXN`, `AUTH`, `OPEN`, `OKAY`, `WRTE`, `CLSE`) including RSA host-key
authentication, and claiming the interface exclusively — which fights `adb`.
The vendor's Linux packaging confirms the cost: their OpenWrt builds ship in
`-openssl` and `-tiny` flavours specifically because RSA auth needs a crypto
library.

Not recommended unless you must run without the Android platform tools.

---

## 2. Framing

### The hello

Immediately after the stream opens, **the host sends four bytes and nothing
else**:

```
51 b7 04 00        (uint32 little-endian 0x0004b751)
```

Evidence: in `easytether-usb` at `0x247a`, `movl $0x4b751,(%rcx)` writes the
value into the tunnel's output buffer and the following `movl $0x4,0x10(%rax)`
sets that buffer's length to 4. `easytether-local` does the same thing with a
static initialiser (`{off=0, len=0, outoff=0, outlen=4}` at `0x100001c70`, plus
`movl $0x4b751,(%rax)`).

The phone does not reply to it. Its internal structure is unconfirmed — likely
a magic (`0xb751`) plus a version (`4`) — but it does not matter: send those
four bytes verbatim. Omitting them has not been tested and the phone is
unlikely to proceed without them.

### Records

After the hello, every packet in both directions is a record:

```
 0        1        2                                    2+len
 +--------+--------+------------------------------------+---------+
 | len lo | len hi |        raw Ethernet frame          | padding |
 +--------+--------+------------------------------------+---------+

 <----------------- (len + 5) & ~3 bytes total -------------------->
```

* `len` is **little-endian** and counts only the frame, not the header or
  padding.
* The record occupies `(len + 5) & ~3` bytes, i.e. `2 + len` rounded up to a
  multiple of 4. The padding is 0–3 zero bytes.
* Both directions use the same format.

Evidence for the stride: `easytether-local` at `0x100001911`
(`addl $0x5,%eax; andl $-0x4,%eax`) and `easytether-usb` at `0x1000015aa`
(identical), in both cases immediately after `movw %ax,(%r12)` stores the
16-bit length at the head of the record.

### Limits and special cases

| Rule | Value | Evidence |
| --- | --- | --- |
| Maximum frame | 1514 (`0x5ea`) | Read size in both binaries: `movl $0x5ea,%edx` before the read into the frame buffer. 1500 MTU + 14 byte Ethernet header. |
| First rejected length | 1515 (`0x5eb`) | `cmpl $0x5eb,%ecx; jae <error>` — the vendor treats a longer record as a protocol error and tears the tunnel down. Do the same: a desynchronised stream cannot be recovered. |
| Records carrying no frame | `len <= 13` | `cmpl $0xd,%ecx; jbe <skip>` — too short to hold an Ethernet header. Skip them, do not deliver them. Presumably keepalives. |

A conforming implementation must:

1. Reject `len >= 1515` by dropping the connection, not by skipping.
2. Skip `len <= 13` silently.
3. Never emit a frame longer than 1514.
4. Zero the padding (the vendor does; whether the phone checks is unknown).

`et_record_size()` in `src/proto.h` is the one-line form of the stride rule, and
`test/unit.c:test_record_size` pins every boundary value.

---

## 3. What is on the other side

The phone behaves as an ordinary Ethernet peer on a point-to-point link. It is
**not** a dumb pipe — it emulates the whole local network.

### Addressing (observed, and consistent with the vendor's own documentation)

| Property | Value |
| --- | --- |
| Host address | `192.168.117.2` |
| Gateway / DHCP server / DNS server | `192.168.117.1` |
| Netmask | `255.255.255.0` |
| Gateway MAC | `02:00:6d:73:62:62` (note the ASCII: `\x02\x00msbb`) |
| Lease time | `0xffffffff` — infinite |
| Option 26 (MTU) | not offered |
| Option 15 (domain) | not offered |

The vendor's FreeBSD documentation states the same subnet, so treat
`192.168.117.0/24` as fixed rather than discovered — but still learn it from
DHCP, because nothing guarantees it across app versions.

### Services the phone provides

* **DHCP server.** Standard DISCOVER/OFFER/REQUEST/ACK. It answers a broadcast
  DISCOVER from `0.0.0.0`. Set the BOOTP broadcast flag while you have no
  address, since the server would otherwise have to ARP for an address you
  cannot yet defend.
* **ARP.** Answers `who-has 192.168.117.1`. Also expected to ARP for you, so
  send a gratuitous ARP after configuring.
* **ICMP echo to the gateway.** Answered locally and synthetically.
* **DNS.** Forwards or answers on `192.168.117.1:53`. The app has a "Resolver"
  setting toggling between built-in and Google Public DNS.

### The critical limitation: TCP and UDP only

The Android app is unprivileged, so it cannot open raw sockets. It implements
NAT in userspace by opening ordinary Java/POSIX sockets to the destination —
which means it terminates your TCP connections and re-originates them.

Consequences a porter must understand:

* **ICMP to the internet is silently dropped.** `ping 8.8.8.8` never works.
  `ping 192.168.117.1` does, because that is emulated locally. Any diagnostic
  or health check built on ping is worthless here; use TCP or UDP.
* Protocols other than TCP and UDP do not traverse. The vendor's FAQ says so
  explicitly about GRE: *"PPTP will not work because there is no way to
  implement GRE passthrough on a non-rooted smartphone."*
* Path MTU beyond the phone does not apply to your frames, because the phone
  terminates TCP. 1500 on the tether link is safe. (One Linux deployment
  reported needing 1448, so keep an MTU override.)

### IPv6

Not observed. The link is IPv4 in practice. Our daemon drops `0x86dd` frames
rather than half-supporting them.

---

## 4. Reproducing the analysis

If you need to re-verify any of this, or a future app version changes something:

```bash
# Which binaries are involved
pkgutil --files com.mobile-stream.pkg.EasyTether

# The strings that name every error path and the socket
strings -a /usr/local/bin/easytether-usb

# The framing arithmetic
otool -tv /usr/local/bin/easytether-usb | grep -n -E '0x5ea|0x5eb|0x4b751'
otool -tv /usr/local/bin/easytether-local
```

The two binaries agree byte for byte on the framing; `easytether-local` is the
smaller and far easier one to read (about 1,100 lines of disassembly versus
4,800), and it uses a plain Unix socket instead of USB, so the tunnel logic is
not tangled up with ADB. **Start there.**

To watch the live protocol without any of our code, `adb forward tcp:9999
localabstract:easytetherx` and then attach anything you like to port 9999.

---

## 5. Known unknowns

Honest list of what has *not* been established:

* The internal meaning of the 4-byte hello. Replicated verbatim; never parsed.
* Whether the phone validates the record padding.
* Whether the phone tolerates a missing hello.
* What the `len <= 13` records actually are. Never observed being sent by the
  phone in our testing; the skip rule is copied from the vendor.
* Whether newer app versions can present raw IP instead of Ethernet. Newer
  *Linux* builds of the vendor client create `tun-easytether` (layer 3) rather
  than `tap-easytether` (layer 2), which suggests the client synthesises and
  strips the Ethernet header itself — the same thing we do — but a negotiated
  L3 mode cannot be ruled out. `check_link_framing()` in `src/main.c` detects
  and reports the case where records turn out to be raw IPv4 rather than
  Ethernet.
* Bluetooth transport. The vendor ships `easytether-bluetooth` using a
  proprietary RFCOMM tunnel (not BNEP/PAN). Unexamined.
