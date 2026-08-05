# Porting guide

Written for an agent or engineer bringing this to another platform, with
Windows as the worked example. Read [PROTOCOL.md](PROTOCOL.md) and
[ARCHITECTURE.md](ARCHITECTURE.md) first.

---

## 1. What actually has to be rewritten

Measured, not estimated. `proto.c` contains **zero** platform calls — it is
pure computation over byte buffers.

| File | Lines | Platform calls | Verdict |
| --- | --- | --- | --- |
| `src/proto.c/h` | 643 | none (only `htons`/`htonl`/`ntohl` from `<arpa/inet.h>`, plus `<string.h>`) | **Reuse verbatim.** Swap one include. |
| `src/util.c/h` | 259 | `clock_gettime`, `fcntl`, `flockfile`, `read`/`write` | Reuse with a ~30-line shim. |
| `src/adb.c/h` | 283 | BSD sockets, `posix_spawn`, `waitpid` | Reuse the logic; swap the socket calls. |
| `src/bridge.c/h` | 825 | `poll` | Reuse the state machine and forwarding; replace the multiplexing. |
| `src/main.c` | 230 | `signal`, `getopt`, `gethostuuid`, `getpwnam`, `usleep` | Mechanical. |
| `src/utun.c/h` | 152 | entirely macOS | **Rewrite.** |
| `src/netcfg.c/h` | 377 | entirely macOS | **Rewrite**, or delete most of it — see §3. |

Of 2769 lines under `src/`: **643 move unchanged**, about 1597 need mechanical
changes, and 529 (`utun.c` + `netcfg.c`) are genuinely new work. The part that
was hard to obtain — the reverse-engineered protocol — is the part that moves
for free.

The test harness ports too: `test/testutil.c` holds the framing over a socket
and the phone's half of DHCP, so `mockphone` and the protocol test come across
with the same socket shim as `adb.c`.

## 2. The decision that shapes everything: layer 2 or layer 3

The tunnel carries **raw Ethernet frames**. macOS forced us to layer 3, because
`utun` is the only virtual interface an unprivileged-of-entitlements daemon can
create there — so we terminate layer 2 ourselves with an ARP responder and a
DHCP client.

**If your platform offers a layer-2 virtual adapter, take it.** The OS then
runs its own DHCP client and ARP, and the daemon collapses into a pure frame
bridge:

| | Layer 2 adapter (TAP-style) | Layer 3 adapter (utun-style) |
| --- | --- | --- |
| Frames from the phone | write straight to the adapter | strip the 14-byte header, trim padding, write the IP packet |
| Frames to the phone | read straight from the adapter | read the IP packet, synthesise an Ethernet header |
| ARP | the OS does it | `handle_arp()`, ~40 lines |
| DHCP | the OS does it | the whole DISCOVER→REQUEST→ACK→renew state machine |
| Address / route / DNS | the OS's DHCP client sets all of it | you set every piece yourself |
| Code you keep from `main.c` | framing, the event loop | all of it |
| Code you keep from `proto.c` | `et_record_size` and the framing constants | all of it |

Layer 2 deletes roughly half of `main.c` and most of `proto.c`, and — more
valuably — it deletes the entire class of bugs documented in
[GOTCHAS.md](GOTCHAS.md) under "the scoped-resolver trap", because you are no
longer fighting the OS's network configuration database.

The vendor's own Windows product is an NDIS "EasyTether network adapter", i.e.
layer 2. Their Linux client historically created `tap-easytether` (layer 2) and
newer builds create `tun-easytether` (layer 3, synthesising the header
client-side, exactly as we do). Both approaches are proven against this phone.

Design the port so this is a **compile-time or run-time choice**, not a fork.

## 3. Suggested shape for a multi-platform tree

Rather than `#ifdef`-ing the existing files, hoist an interface. The natural
seam is already there — `main.c` only touches the platform through `utun_*`
and `netcfg_*`.

```
src/
  proto.c/h          unchanged
  util.c/h           + a small platform shim
  main.c             unchanged except the wait primitive
  platform.h         the interface below
  darwin/link_utun.c     existing utun.c
  darwin/netcfg.c        existing netcfg.c
  win32/link_tap.c       new
  win32/netcfg.c         new
```

```c
/* platform.h -- everything main.c needs from the OS. */

struct link;                       /* opaque: utun fd, or a TAP HANDLE */

/*
 * Layer 2 links carry Ethernet frames and the OS runs DHCP and ARP itself.
 * Layer 3 links carry bare IP and this daemon must terminate layer 2.
 * main.c branches on this once, at startup.
 */
enum link_kind { LINK_L2_ETHERNET, LINK_L3_IP };

int          link_open(struct link **out, const char *name_hint);
void         link_close(struct link *l);
enum link_kind link_kind(const struct link *l);
const char  *link_name(const struct link *l);

/* L2: buf is a whole Ethernet frame. L3: buf is an IP packet, af is set. */
ssize_t      link_read(struct link *l, uint8_t *buf, size_t cap, int *af);
ssize_t      link_write(struct link *l, int af, const uint8_t *pkt, size_t len);

/* No-ops on a layer-2 link where the OS's DHCP client does the work. */
int          netcfg_configure(struct link *l, const struct dhcp_lease *lease);
int          netcfg_set_default_route(struct link *l);
int          netcfg_set_dns(struct link *l, const struct dhcp_lease *lease);
int          netcfg_clear_dns(void);

/*
 * Wait until either handle is ready or the deadline passes.
 * poll() on POSIX; WSAEventSelect + WaitForMultipleObjects on Windows.
 */
struct waitset { int link_readable, tunnel_readable, tunnel_writable; };
int          plat_wait(struct link *l, int tunnel_fd, int want_write,
                       int timeout_ms, struct waitset *out);
```

With `LINK_L2_ETHERNET`, `main.c` skips `enter_discover()` entirely, never
calls the `netcfg_*` functions, and its inbound path becomes
`link_write(frame)` with no parsing at all.

## 4. Bring-up order

This is the important part. **Defer the virtual adapter until last** — it is
the hardest, least portable piece, and everything else can be proven without
it, without a phone, and without USB.

1. **Compile `proto.c` and `test/unit.c`.** No I/O whatsoever. Proves the
   toolchain, the byte-order assumptions and struct packing on the new target.
   Should be an afternoon, and it validates the most valuable 400 lines.
2. **Port `adb.c` to the platform's sockets, then run `test/protocol` against
   `test/mockphone` over loopback TCP.** Still no phone, no USB, no driver, no
   admin rights. This proves the ADB handshake, the hello, the record framing,
   DHCP encode/decode, ARP and an ICMP round trip. `mockphone` is plain C over
   a TCP socket and should port with the same shim as `adb.c`.
3. **Run `test/protocol <port> --dns` against a real phone** through the
   platform's own `adb`. Proves the whole protocol end to end on real hardware
   — still with no adapter and no privileges.
4. **Now** write the virtual adapter, and test it by pointing the daemon at
   `mockphone` (`-p 15037 -D -R`) rather than a phone, so a half-working
   adapter cannot black-hole the machine.
5. **Last**, network configuration.

Steps 1–3 need no elevated privileges and no hardware beyond a phone in step 3.
If a port stalls, it will stall at step 4; everything before it is mechanical.

## 5. C portability inventory

Every non-portable construct in the tree, and what to do about it.

| Used | Where | Replacement |
| --- | --- | --- |
| `poll()` | `main.c` | `WSAPoll` handles sockets only, and cannot wait on a driver handle — you need `WSAEventSelect` + `WaitForMultipleObjects`, or overlapped I/O. This is the one structural change to `main.c`. |
| `read`/`write` on sockets | `adb.c`, `main.c` | `recv`/`send`. |
| `close` on sockets | `adb.c` | `closesocket`. |
| `int` file descriptors | everywhere | `SOCKET` is unsigned and `INVALID_SOCKET` is not −1. Do not test `< 0`. |
| `EAGAIN` | `main.c`, `adb.c` | `WSAEWOULDBLOCK` via `WSAGetLastError()`; `errno` is not set by Winsock. |
| `EINTR` | several | Does not occur on Windows; the handling is harmless to keep. |
| `SIGPIPE` / `MSG_NOSIGNAL` | `netcfg.c`, `main.c` | No equivalent needed. |
| `fcntl(O_NONBLOCK)` | `util.c` | `ioctlsocket(FIONBIO)`. |
| `clock_gettime(CLOCK_MONOTONIC)` | `util.c` | `QueryPerformanceCounter`, or `GetTickCount64` — ms resolution is plenty here. |
| `arc4random_buf` | `main.c` (DHCP xid) | `BCryptGenRandom`, or `rand_s`. Must not be predictable across restarts, or a stale reply could be accepted. |
| `gethostuuid` | `main.c` (stable MAC) | Any stable per-machine value: the `MachineGuid` registry value, or a hash of the volume serial. It only needs to be stable so the phone reissues the same lease. |
| `getpwnam`/`getpwuid`, `/dev/console` | `main.c` (find the console user) | Not needed — see §6 on the ADB server. |
| `posix_spawn` + `waitpid` | `adb.c`, `netcfg.c` | `CreateProcess` + `WaitForSingleObject`, or drop entirely if you configure via APIs rather than command-line tools. |
| `getopt` | `main.c` | No system `getopt`; vendor a ~40-line one or hand-roll the parse. |
| `strlcpy` | `utun.c` | `strncpy_s`, or a two-line local. |
| `usleep` | `main.c` | `Sleep(ms)`. |
| `__attribute__((format(printf)))` | `util.h` | MSVC ignores it; guard with `#if defined(__GNUC__)`. Keep it — it catches real bugs on the platforms that honour it. |
| Designated initialisers, compound literals, `//` comments | throughout | Fine on clang-cl and MinGW. Check your MSVC version if you use it. |
| `flockfile`/`funlockfile` | `util.c` | Drop, or use a critical section. Single-threaded today. |

Two portability details in `proto.c` that are *not* problems but are worth
knowing you got right: it never dereferences a multi-byte integer out of a
packet buffer directly — everything goes through `memcpy` or explicit byte
shifts — so there are no alignment or strict-aliasing hazards to fix. Keep that
discipline in any new parsing code.


---

# Windows

Everything below was verified against real documentation and real source
(OpenVPN's `tap-windows6`, WireGuard's `wintun`, AOSP's `adb`), not written
from memory. Where two sources disagreed, the disagreement is called out.

## 6. Choosing the adapter — the decision that costs the most to get wrong

Windows has exactly two redistributable user-mode virtual adapters.

### TAP-Windows6 (`tap0901`) — layer 2

The right architectural fit. It is unambiguously Ethernet:

```c
/* tap-windows6 src/constants.h */
#define TAP_MEDIUM_TYPE        NdisMedium802_3
#define TAP_MAX_FRAME_SIZE     1514          /* exactly EasyTether's maximum */
#define TAP_MIN_FRAME_SIZE     60
```

```
; OemVista.inf
*MediaType = 0x0                 ; NdisMedium802_3
HKR, Ndi\Interfaces, LowerRange, 0, "ethernet"
```

`TAP_MAX_FRAME_SIZE` is 1514 — a byte-for-byte match with the protocol's
maximum frame, which is not a coincidence, both being Ethernet. The driver also
zero-pads anything shorter than 60 bytes on the way in, so a 42-byte ARP reply
from the phone needs no handling.

What you get for free: **Windows' own DHCP client and ARP**, and with them the
lease, the default gateway, the DNS servers, the domain, classless static
routes, lease renewal, and NLA/NCSI network identification — which is what
makes Windows stop believing the machine is offline. That last item is the
exact class of problem that cost the most time on macOS (see
[GOTCHAS.md](GOTCHAS.md), "the scoped-resolver trap"). On this path it simply
does not exist.

Cost: it is a kernel driver you must ship and install.

### Wintun — layer 3

WireGuard's driver. A ring-buffer API in a signed DLL, no `.inf` juggling, and
a structural match for the existing `utun.c` — the ARP responder and DHCP
client carry over unchanged, so it is the smaller code delta. But it is the
wrong layer for this protocol, and it leaves you re-solving the network
configuration problem by hand.

### Recommendation

**Start with TAP-Windows6.** Fall back to Wintun if driver installation proves
unacceptable in your deployment.

Note one contradiction the research surfaced, because it matters and is easy to
get wrong in either direction: TAP-Windows6 has a real history of blocking
Memory Integrity (HVCI), and Microsoft is removing default kernel trust for
**cross-signed** drivers on Windows 11 24H2/25H2/26H1 and Server 2025 in the
April 2026 update. That applies to the `dist.win7.zip` catalog, which is signed
by `CN=OpenVPN Inc.` cross-signed to the Microsoft Code Verification Root. The
`dist.win10.zip` catalog is signed by `CN=Microsoft Windows Hardware
Compatibility Publisher` — an attestation signature, which is unaffected and
loads with Secure Boot and HVCI on. **Ship the win10 catalog and verify HVCI on
your actual target build before committing.** If that verification fails,
switch to Wintun and accept the layer-3 shim.

### Opening a TAP adapter

Create the adapter once, in the installer, as Administrator:

```
tapctl create --hwid tap0901 --name "EasyTether"     # prints the adapter GUID
```

The default `--hwid` in current OpenVPN is `ovpn-dco`, so passing `tap0901`
explicitly is mandatory. `devcon install OemVista.inf tap0901` and in-process
SetupAPI are the other two routes.

The daemon itself does **not** need administrator to open it — the INF ships
`AllowNonAdmin = 1` and the driver sets a permissive SDDL. Opening is exclusive;
one client at a time.

```c
/* Device path: USERMODEDEVICEDIR + "{adapter GUID}" + TAP_WIN_SUFFIX */
h = CreateFile("\\\\.\\Global\\{GUID}.tap", MAXIMUM_ALLOWED, 0, NULL,
               OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, NULL);

/* Without this NDIS reports "media disconnected" and DHCP never starts. */
ULONG status = TRUE; DWORD len;
DeviceIoControl(h, TAP_WIN_IOCTL_SET_MEDIA_STATUS,
                &status, sizeof status, &status, sizeof status, &len, NULL);
```

Then plain overlapped `ReadFile`/`WriteFile` of whole Ethernet frames.

Of the twelve IOCTLs you need **only #6**. Specifically **do not** use
`TAP_WIN_IOCTL_CONFIG_DHCP_MASQ` (#7): it turns the driver into an in-kernel
fake DHCP server that answers Windows' DISCOVER itself. It exists because TAP
normally has no real DHCP server behind it. You *do* have one — the phone — and
the masquerade server will race it and win, handing Windows a hardcoded lease
instead of the real one. `CONFIG_POINT_TO_POINT` (#5) and `CONFIG_TUN` (#10)
switch it to layer 3; avoid both.

`src/tap-windows.h` is dual GPLv2/MIT specifically so consumers can copy it.
The driver binary is GPLv2 — shipping it carries a source offer, easy to satisfy
since upstream is public and you are shipping it unmodified. Do **not** copy
`openvpn/src/tapctl/tap.c` into anything proprietary; it is GPLv2 only.

### If you take the layer-2 path, turn the shim off

**Do not run the ARP responder on a TAP adapter.** Windows' DHCP client does
address-conflict detection by ARPing for the address it is about to accept. If
your responder answers for it, Windows sees a conflict and sends DHCPDECLINE,
and the lease never completes. Same reasoning for the DHCP client: two clients
on one link is a bug, not redundancy.

This is what `enum link_kind` in §3 is for.

## 7. Transport

Good news: **`adb.exe` on Windows speaks the identical host protocol on TCP
`127.0.0.1:5037`** — same 4-hex-digit length prefix, same `OKAY`/`FAIL`, same
`host:transport-any` and `localabstract:` service names. No named-pipe variant,
no different port. Verified in AOSP `client/commandline.cpp`
(`kDefaultServerPort = 5037`). So `adb.c`'s logic is correct as written; only
the socket calls change.

Two things in `src/adb.c` are outright bugs on Windows rather than mere
incompatibilities:

1. **`sa.sin_len = sizeof sa;`** — `SOCKADDR_IN` has no `sin_len` member.
   A hard compile error. It is optional even on macOS; delete or guard it.
2. **`setsockopt(SO_RCVTIMEO/SO_SNDTIMEO, &struct timeval)`** — on Windows the
   option value is a `DWORD` of **milliseconds**. Passing a `timeval`
   reinterprets `tv_sec` as milliseconds, so the 10-second handshake guard
   silently becomes ~10 ms and every connect fails. This one compiles.

```c
DWORD ms = 10000;
setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
```

Other transport notes:

* Sockets are **inheritable by default** on Windows — the opposite of the
  `FD_CLOEXEC` posture `set_cloexec()` establishes. If the daemon ever spawns
  `adb.exe`, the child inherits the tunnel socket and the phone-side connection
  will not drop when the daemon exits. Use
  `WSASocketW(..., WSA_FLAG_NO_HANDLE_INHERIT)` or `SetHandleInformation`.
* `WSAStartup(MAKEWORD(2,2), ...)` before any Winsock call, in `ServiceMain`,
  not lazily.
* Include `winsock2.h` **before** `windows.h`, with `WIN32_LEAN_AND_MEAN`, or
  you get a wall of redefinition errors from the legacy `winsock.h`.
* Android 10 (API 29) and later ship Microsoft OS Descriptors declaring
  compatible ID `WINUSB`, so Windows binds in-box `winusb.sys` and **no driver
  INF is needed**. Older devices need the Google USB Driver.

### Do not start the ADB server from the service

This is the Windows form of a trap we already hit on macOS. `adb` hard-codes
its key path to `CSIDL_PROFILE\.android\adbkey` and does not honour
`ANDROID_SDK_HOME`. A LocalSystem service therefore lands on
`C:\Windows\System32\config\systemprofile\.android\adbkey`, gets a **second,
separate** "Allow USB debugging?" prompt, and the user's existing authorization
does not carry over. Connect to an existing server and log a clear error if
there is none; `ADB_VENDOR_KEYS` (semicolon-separated on Windows) is the clean
workaround if you must.

### Detecting the phone being plugged in

Three options, cheapest first:

1. **`host:track-devices`** on a second connection to the ADB server. Never
   closes, pushes a new length-prefixed device list on every add, remove or
   state change. No Windows API at all, and it reports *authorization* changes
   too — which USB notifications cannot.
2. **`CM_Register_Notification`** (`cfgmgr32.h`, Windows 8+) filtering
   `CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE` on the ADB interface GUID
   `{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}`. Callback-based, needs no window —
   the correct choice for a service.
3. `RegisterDeviceNotificationW` with `DEVICE_NOTIFY_SERVICE_HANDLE`, arriving
   as `SERVICE_CONTROL_DEVICEEVENT` in your `HandlerEx`.

### Direct USB is a worse default than on macOS

WinUSB interfaces are **exclusive**: a second `CreateFile` on an already-open
device fails with `ERROR_ACCESS_DENIED`. So a direct-USB implementation and a
running ADB server cannot both own the phone — you would have to kill the
user's `adb` server and break Android Studio. Combined with reimplementing the
ADB device protocol and RSA-2048 host-key auth, this is a clear no.

## 8. The event loop — the single largest structural change

`poll()` does not port. `WSAPoll` accepts **only sockets**, and the adapter is a
file handle (TAP overlapped I/O) or a Win32 event (`WintunGetReadWaitEvent`).
You cannot mix the two in one wait primitive on Windows.

The shape that works:

```
WaitForMultipleObjects over:
   [0] WSAEVENT from WSAEventSelect(tunnel_sock, ev, FD_READ|FD_WRITE|FD_CLOSE)
   [1] OVERLAPPED.hEvent for the pending adapter ReadFile
   [2] a manual-reset quit event, signalled from the service control handler
```

Four traps, each of which produces a silent hang rather than an error:

* **`FD_WRITE` is edge-triggered.** `POLLOUT` is level-triggered, and
  `flush_out()` depends on that. `FD_WRITE` is signalled once when the socket
  becomes writable and is **only re-armed after a `send()` fails with
  `WSAEWOULDBLOCK`**. Port `flush_out()` naively and the connection deadlocks
  with data sitting in the outbound queue. Track writability yourself: assume
  writable, clear the flag on `WSAEWOULDBLOCK`, set it on the next `FD_WRITE`.
* **Reset the event with `WSAEnumNetworkEvents`, never `WSAResetEvent`.** Only
  the former resets the event and adjusts the FD state atomically. Hand-resetting
  loses wakeups.
* **Drain the adapter in a loop.** The readiness signal means "the ring may be
  non-empty", not "one packet is ready". Read until it says empty before
  waiting again.
* `WSAEventSelect` silently forces the socket non-blocking, and the only way
  back is `WSAEventSelect(s, NULL, 0)` then `ioctlsocket(FIONBIO, 0)`.

`MAXIMUM_WAIT_OBJECTS` is 64. Not a constraint for three handles, but it is why
this design does not scale to many phones without moving to IOCP.

## 9. Network configuration

Only needed on the layer-3 path. On layer 2, Windows' DHCP client does all of
it, and your entire `netcfg.c` becomes about thirty lines: raise media status,
ensure `EnableDHCP=1`, set the MTU, set the interface metric.

Everything below is IP Helper (`netioapi.h`, `Iphlpapi.lib`, Vista+),
Administrator-only, satisfied by LocalSystem with no manifest.

**Identify the adapter by LUID, never by index.** The interface index changes
on disable/enable. Resolve the LUID once at startup:
`WintunGetAdapterLuid()`, or `ConvertInterfaceGuidToLuid()` from the TAP
adapter's `NetCfgInstanceId`.

```c
MIB_UNICASTIPADDRESS_ROW row;
InitializeUnicastIpAddressEntry(&row);            /* mandatory: fills defaults */
row.InterfaceLuid                = luid;
row.Address.si_family            = AF_INET;
row.Address.Ipv4.sin_family      = AF_INET;
row.Address.Ipv4.sin_addr.s_addr = lease_ip;      /* network byte order */
row.OnLinkPrefixLength           = 24;
row.ValidLifetime = row.PreferredLifetime = 0xffffffff;
row.DadState = IpDadStatePreferred;   /* Win10+: skip duplicate-address wait */
CreateUnicastIpAddressEntry(&row);
```

Without `IpDadStatePreferred` the address starts *tentative* and is unusable
until duplicate address detection finishes.

| Job | API |
| --- | --- |
| Address | `CreateUnicastIpAddressEntry` |
| Routes | `CreateIpForwardEntry2` with `MIB_IPFORWARD_ROW2` |
| MTU, metric | `SetIpInterfaceEntry` |
| DNS | `SetInterfaceDnsSettings` — takes the adapter **GUID**, not the LUID. Documented as build 19041+, but the export exists from 1809; `GetProcAddress` it and fall back to the registry on `ERROR_PROC_NOT_FOUND`, as WireGuard does. |
| Friendly name | `NciSetConnectionName` from `nci.dll` (no SDK import library — generate one from a `.def`). Writing the registry `Connection\Name` value directly does not stick. |

There is **no IP Helper API to switch an adapter back to DHCP**. If anything
ever ran `netsh … set address … static` on it, it is pinned to static
permanently, and the only ways back are `netsh interface ipv4 set address
name=<idx> source=dhcp` or WMI `EnableDHCP()`.

On DNS ranking, Windows differs from macOS in a helpful way: it is decided by
interface metric, and there is no equivalent of the scoped-resolver trap where
a resolver is registered and then silently never consulted.

## 10. Service, build and distribution

**Build.** clang-cl or MinGW-w64, not `cl.exe`. MSVC needs `/std:c11`
explicitly (its default C mode is C89-plus-extensions) and lacks
`__attribute__((format(printf)))`, which catches real bugs. MSVC *does* support
the designated initialisers and the one compound literal this codebase uses —
but only in C mode; compiling `proto.c` as C++ fails with C4576. MinGW-w64 also
ships `getopt.h`, removing one shim.

**Service.** `StartServiceCtrlDispatcher` must be called within 30 seconds of
process start, and you should report `SERVICE_RUNNING` within ~100 ms. Do
**not** block that on reaching the phone — report running immediately and let
the existing reconnect/backoff loop do its job. `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`
is the documented way to detect a console launch, which preserves the `-v`
foreground debugging path exactly as it works today. Use
`RegisterServiceCtrlHandlerEx`, not the non-`Ex` version, so device events can
arrive later.

Run as LocalSystem — creating the adapter and every IP Helper call is
admin-only — then harden with `SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO` rather
than downgrading the account.

`sc.exe` requires the equals sign attached to the option and a space before the
value: `start= delayed-auto`, never `start=delayed-auto`. Before `sc delete`,
run `sc config … start= disabled`, because a queued `SC_ACTION_RESTART` cannot
be cancelled and the service will come back after you thought you removed it.

**Logging.** Classic Event Log via `ReportEvent` needs a compiled `.mc` message
file in a resource-only DLL registered under `EventMessageFile`, or admins see
"The description for Event ID cannot be found". The pragmatic split is a plain
file log — matching today's `/var/log/easytether-bridge.log` — plus a handful of
Event Log entries for start, stop and fatal errors.

**Signing.** An unsigned user-mode service installs and runs fine, except where
Smart App Control is active. Only the kernel driver strictly needs a signature,
which is the whole argument for shipping someone else's signed driver rather
than authoring one. Do not budget for an EV certificate to escape SmartScreen —
that behaviour was removed in 2024. SignPath Foundation offers free OV signing
for qualifying open-source projects. Self-authored drivers are effectively off
the table: attestation signing is now documented as testing-only, and production
retail drivers require the full WHCP/HLK path.

## 11. Checklist

- [ ] `proto.c` + `test/unit.c` compile and pass. No I/O yet.
- [ ] `adb.c` on Winsock. Delete `sin_len`; fix `SO_RCVTIMEO` to `DWORD` ms.
- [ ] `mockphone` compiles; `test/protocol` passes against it over loopback.
- [ ] `test/protocol 5037 --dns` passes against a real phone via `adb.exe`.
- [ ] Decide layer 2 vs layer 3. **Verify the TAP driver loads with HVCI on
      your target Windows build** before committing to it.
- [ ] Adapter open/read/write, tested against `mockphone` with `-D -R`.
- [ ] Event loop on `WaitForMultipleObjects`; handle edge-triggered `FD_WRITE`.
- [ ] Layer 2: media status, `EnableDHCP=1`, MTU, metric, and **shim disabled**.
      Layer 3: address, routes, DNS via IP Helper.
- [ ] Service wrapper, install/uninstall, log file.
- [ ] Port `check.sh` — the equivalent diagnostic saves more time than it costs.
