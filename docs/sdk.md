# ChmLink SDK

The SDK is a C library, `chmlink.dll`, with headers in `include/chmlink/`. It
creates and deletes virtual adapters, installs the driver they need, and moves
packets between your application and the Windows network stack.

- [Getting the SDK](#getting-the-sdk)
- [Concepts](#concepts)
- [Conventions](#conventions)
- [Adapters](#adapters): create, list, change, delete
- [Sessions](#sessions): open, query, close
- [Receiving packets from Windows](#receiving-packets-from-windows)
- [Sending packets to Windows](#sending-packets-to-windows)
- [Waiting](#waiting)
- [Offloads (Ethernet mode)](#offloads-ethernet-mode)
- [Large MTU and software GRO](#large-mtu-and-software-gro)
- [Packet helpers](#packet-helpers)
- [Batches](#batches)
- [Diagnostics and versions](#diagnostics-and-versions)
- [Driver compatibility](#driver-compatibility)
- [Errors](#errors)
- [Examples](#examples)

## Getting the SDK

Build it with CMake and Visual Studio 2022 (x64):

```powershell
cmake -S . -B build -A x64 -DCHMLINK_DRIVER_DIR=C:\path\to\signed\package
cmake --build build --config Release
cmake --install build --config Release --prefix C:\sdk\chmlink
```

`CHMLINK_DRIVER_DIR` names a directory with a signed `chmlink.inf`,
`chmlink.sys` and `chmlink.cat` (see [development.md](development.md) for
building and signing the driver). The DLL then carries the package as
resources and installs it itself, so an application ships `chmlink.dll` and
nothing else. Without it the DLL works the same, but
`chmlink_create_adapter` needs the path of an INF.

The install step produces:

| path | content |
|---|---|
| `bin/chmlink.dll` | the library, with a version resource |
| `lib/chmlink.lib` | its import library |
| `include/chmlink/*.h` | `api.h` (include this one) and the headers it pulls in |

```c
#include <chmlink/api.h>   /* link chmlink.lib */
```

Creating adapters and opening sessions require an administrator (or SYSTEM).

## Concepts

**Adapter.** A virtual network interface that Windows sees as a NIC. Each one
has a mode, fixed at creation:

| mode | Windows sees | your application exchanges |
|---|---|---|
| `CHMLINK_MODE_IP` | a point-to-point interface (TUN-like) | IPv4 and IPv6 packets |
| `CHMLINK_MODE_ETHERNET` | an Ethernet NIC with a MAC (TAP-like) | Ethernet frames (no FCS, VLAN tags allowed) |

**Session.** An open data path to one adapter. One session per adapter at a
time; it ends when you close it or your process exits. Between sessions the
adapter stays installed and keeps its addresses, but carries no traffic.

**Direction.** *From Windows* is traffic the stack sends through the adapter
(you receive it). *To Windows* is traffic you inject (the stack receives it).
In `chmlink_adapter_info` the counters say `tx` for from Windows and `rx` for
to Windows, as a NIC driver would.

**Rings.** A session moves packets through shared memory, one ring from
Windows and up to four towards Windows. The SDK allocates them; the driver
reads and writes them directly, without a system call per packet.

**Owner.** Every adapter records an owner GUID. Deleting an adapter or changing
its MTU requires the same owner, so applications do not remove each other's
adapters. Pick a fixed GUID for your application.

## Conventions

- Functions returning `int` return nonzero on success and zero on failure;
  functions returning a pointer return `NULL` on failure. The reason is in
  `GetLastError()` (see [Errors](#errors)).
- Structures with a `size` field must have it set to `sizeof` the structure;
  zero every field you do not set.
- Strings are UTF-16 (`wchar_t`), except `interface_id` (ASCII GUID text).
- Session functions may be called from several threads at once, as described
  per function. You must stop calling into a session before closing it, and
  close it exactly once.

## Adapters

Declared in `chmlink/manage.h`.

### Creating

```c
static const GUID my_owner = { 0x1b2c3d4e, 0x5f60, 0x4718, {0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0} };

chmlink_create_options options = { sizeof(options) };
chmlink_device device;
options.mode = CHMLINK_MODE_IP;
options.name = L"My VPN";
options.owner = &my_owner;
if (!chmlink_create_adapter(&options, &device))
    fail(GetLastError());
/* device.interface_index, device.interface_guid, device.instance_id ... */
```

`chmlink_create_options`:

| field | meaning | default when zero/NULL |
|---|---|---|
| `size` | `sizeof(chmlink_create_options)` | required |
| `mode` | `CHMLINK_MODE_IP` or `CHMLINK_MODE_ETHERNET` | Ethernet |
| `name` | Windows connection name, up to 128 characters | `ChmLink-<adapter id>` |
| `owner` | owner GUID | `CHMLINK_DEFAULT_OWNER_ID` (meant for tools; use your own) |
| `mac` | 6 bytes, Ethernet only; must be unicast and locally administered | random, locally administered |
| `mtu` | IP only, 1280 to 65535 | 1500 |
| `driver_inf` | path of a signed `chmlink.inf` to install from | the package embedded in `chmlink.dll` |

`chmlink_create_adapter` installs the driver when needed, creates the device,
waits until Windows has a network interface for it and gives the interface
its name. On success, `device` (may be `NULL`) describes the new adapter. If
`device.flags` has `CHMLINK_DEVICE_REBOOT_REQUIRED`, Windows asks for a restart
before the adapter works.

On failure nothing is left behind. The exception is `ERROR_INSTALL_FAILURE`:
creation failed and so did removing the half-created device; it shows up in
`chmlink_enumerate_devices` and can be deleted.

Common failures: `ERROR_DUP_NAME` (the name, MAC or identity is in use),
`ERROR_REVISION_MISMATCH` (an unusable driver package, see
[Driver compatibility](#driver-compatibility)), `ERROR_BUSY` (another process
is changing adapters for more than ten seconds), `ERROR_INVALID_PARAMETER`.

Creating an adapter does not configure addresses or routes; use the IP Helper
API (`CreateUnicastIpAddressEntry`, `CreateIpForwardEntry2`) or `netsh` on
`device.interface_index` or the interface LUID. Open the session first, then
add addresses: the link comes up with the session.

### Which driver is installed

All ChmLink adapters on a machine share one driver. When `driver_inf` is
`NULL`, the SDK compares its embedded package with the one existing adapters
run, and the newer one wins:

- no adapters yet: the embedded package is installed;
- the existing package is as new or newer: it is used as it is;
- the embedded package is newer: every existing adapter is upgraded to it
  first (their sessions end), unless one of them has a session open; then the
  existing package is kept, provided this SDK can talk to it.

With `driver_inf` set, the existing adapters must already run exactly that
package (same INF, SYS and CAT); nothing else is changed.

### Listing and reading

```c
static int show(const chmlink_device *d, void *context)
{
    wprintf(L"%ls  %ls  index %u  mtu %u\n", d->instance_id, d->name, d->interface_index, d->mtu);
    return 1;   /* 0 stops */
}

chmlink_enumerate_devices(&my_owner, show, NULL);   /* NULL owner: all adapters */
chmlink_get_device(L"ROOT\\NET\\0001", &device);
```

`chmlink_device`:

| field | meaning |
|---|---|
| `mode`, `mtu` | as configured; the MTU applies from the next device start |
| `interface_index` | 0 while Windows has no interface for the adapter; can change across restarts |
| `interface_guid` | the Windows interface GUID (zero while unknown) |
| `mac` | Ethernet MAC; zero for IP adapters |
| `adapter_id`, `owner_id` | the adapter's stable identity and its owner |
| `name` | the name chosen at creation |
| `instance_id` | the device instance ID, the key for delete and MTU changes |
| `flags` | `CHMLINK_DEVICE_REBOOT_REQUIRED` from the call that filled it in |

Enumeration skips devices whose configuration cannot be read.
`chmlink_get_device` fails with `ERROR_FILE_NOT_FOUND` for a ChmLink device
without configuration and `ERROR_ACCESS_DENIED` for a device that is not
ChmLink.

### Changing the MTU

```c
uint32_t flags;
chmlink_set_adapter_mtu(device.instance_id, &my_owner, 9000, &flags);
```

IP adapters only, 1280 to 65535; Ethernet adapters stay at 1500. The adapter
restarts, which ends its session. If the value was stored but the restart
failed, the call fails with `ERROR_INSTALL_FAILURE` and the MTU applies at the
next restart. See [Large MTU and software GRO](#large-mtu-and-software-gro).

### Deleting

```c
chmlink_delete_adapter(device.instance_id, &my_owner);
chmlink_delete_driver();   /* in an uninstaller, after the last adapter */
```

Deleting ends the adapter's session. `chmlink_delete_driver` removes every
ChmLink driver package from the Driver Store that no adapter uses; it fails
with `ERROR_INF_IN_USE_BY_DEVICES` while adapters still exist (after removing
the unused packages).

Adapter changes are serialized across processes: a call waits up to ten
seconds for another to finish, then fails with `ERROR_BUSY`.

## Sessions

Declared in `chmlink/api.h`.

```c
chmlink_adapter *adapter = chmlink_open(device.interface_index);
chmlink_adapter_info info;
chmlink_query(adapter, &info);
/* ... */
chmlink_close(adapter);
```

| function | |
|---|---|
| `chmlink_open(index)` | open by interface index, with 4 MiB rings |
| `chmlink_open_ex(index, &options)` | `chmlink_open_options.ring_capacity`: bytes per ring, a power of two from 128 KiB to 64 MiB |
| `chmlink_open_by_id("{GUID}")` | open by interface GUID (stable while the adapter exists) |
| `chmlink_close(adapter)` | cancels a wait, lets calls in progress finish, ends the session, frees everything |
| `chmlink_query(adapter, &info)` | the adapter's state and counters |
| `chmlink_inspect(index, &info)` | the same without opening a session |
| `chmlink_enumerate(callback, context)` | `chmlink_adapter_info` of every adapter |

Opening fails with `ERROR_SHARING_VIOLATION` when another session is open,
`ERROR_ACCESS_DENIED` without administrator rights, and
`ERROR_REVISION_MISMATCH` when the driver cannot serve this SDK.

`chmlink_adapter_info`:

| field | meaning |
|---|---|
| `mode`, `mtu`, `packet_limit` | packet format and the largest packet (IP: the MTU; Ethernet: 1522 bytes, VLAN tag included) |
| `mac`, `interface_index`, `interface_id` | identity |
| `state` | `CHMLINK_STATE_*` bits, below |
| `capabilities` | `CHMLINK_CAP_*` bits; `CHMLINK_CAP_OFFLOAD` means offloads can be requested |
| `offload` | offloads in effect for this session |
| `version`, `min_version` | the device contracts the driver speaks |
| `ring_capacity`, `ring_pending_bytes` | the from-Windows ring and how much of it is unread |
| `packet_filter` | the Windows packet filter (Ethernet) |
| `tx_packets`, `tx_bytes`, `tx_errors`, `tx_queue_drops` | from Windows; drops are packets that found the ring full |
| `rx_packets`, `rx_bytes`, `rx_discards` | to Windows; discards are packets the driver refused |

States:

| bit | meaning |
|---|---|
| `CHMLINK_STATE_RUNNING` | the data path runs |
| `CHMLINK_STATE_SESSION` | a session is open |
| `CHMLINK_STATE_PAUSED`, `CHMLINK_STATE_PAUSING` | Windows paused the adapter (configuration change, disable) |
| `CHMLINK_STATE_LOW_POWER` | the system sleeps; the data path resumes after wake-up |
| `CHMLINK_STATE_SHUTDOWN` | the system shuts down |
| `CHMLINK_STATE_REMOVING` | the adapter is being removed; the session will end |

While the adapter is paused or asleep, waits fail with `ERROR_NOT_READY` and
packets are not moved; keep the session and wait again.

## Receiving packets from Windows

Zero copy: the packet stays in the ring until you release it.

```c
uint32_t size;
const uint8_t *packet;
while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) {
    handle(packet, size);
    chmlink_release_packet(adapter, packet);
}
if (GetLastError() != ERROR_NO_MORE_ITEMS) fail(GetLastError());
```

- `NULL` with `ERROR_NO_MORE_ITEMS` means the ring is empty: [wait](#waiting).
- Packets may be released in any order; ring space is reclaimed up to the
  oldest unreleased packet, so do not hold one for long.
- The third argument receives the offload descriptor when the session has
  offloads (see below); pass `NULL` otherwise.
- When the ring is full, Windows' further packets are dropped and counted in
  `tx_queue_drops`; a 4 MiB ring absorbs stalls of a few tens of
  milliseconds.

`chmlink_receive(adapter, buffer, capacity, &bytes)` copies up to 256 packets
into one [batch](#batches) instead.

## Sending packets to Windows

Zero copy: reserve space in the ring, write the packet, publish it.

```c
uint8_t *slot = chmlink_allocate_packet(adapter, size, NULL);
if (slot == NULL) {
    /* ERROR_BUFFER_OVERFLOW: the ring is full; the driver is behind. Retry shortly. */
}
memcpy(slot, packet, size);   /* or decrypt straight into it */
chmlink_send_packet(adapter, slot);
```

- Write exactly `size` bytes; one complete IP packet (IP mode) or Ethernet
  frame (Ethernet mode) per allocation. The driver checks the framing and
  discards invalid packets (`rx_discards`).
- Several threads may allocate and send at once. Each sending thread gets its
  own ring (up to four per session), and the driver hands every ring to the
  stack on its own core. Packets of one thread arrive in order.
- The driver also spreads packets over up to four cores by flow (addresses,
  protocol, ports), as a NIC's RSS does: one thread carrying many connections
  still uses several cores, and each connection stays in order. Run the
  sending thread at normal priority or above.
- Packets are published in allocation order within a ring: do not allocate
  and then stall.

`chmlink_inject(adapter, batch, length)` copies a whole [batch](#batches)
instead: all of it or nothing (`ERROR_BUFFER_OVERFLOW` when it does not fit,
and the same batch can be retried).

## Waiting

```c
for (;;) {
    if (!chmlink_wait_readable(adapter, CHMLINK_WAIT_INFINITE)) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_READY) continue;   /* paused or asleep */
        break;                                /* aborted, removed, shutting down */
    }
    /* receive until ERROR_NO_MORE_ITEMS */
}
```

- `chmlink_wait_readable(adapter, timeout_ms)` returns when packets from
  Windows are available. `0` polls; `CHMLINK_WAIT_INFINITE` has no limit.
  It polls the ring briefly before sleeping, which keeps latency low at high
  packet rates.
- One waiter per session (`ERROR_BUSY` for a second one).
- Failures: `ERROR_TIMEOUT`; `ERROR_OPERATION_ABORTED` (cancelled, closed, or
  the session ended); `ERROR_NOT_READY` (paused or asleep); device removal
  errors; `ERROR_SHUTDOWN_IN_PROGRESS`.
- `chmlink_cancel_wait(adapter)` wakes the current wait from another thread.
- `chmlink_get_read_event(adapter)` returns the auto-reset event behind it,
  for a `WaitForMultipleObjects` loop of your own. Do not close it. After it
  fires, receive until the ring is empty.

## Offloads (Ethernet mode)

Ethernet adapters can take over segmentation and checksums, as a physical NIC
does. Windows does not use task offloads on IP-mode adapters, so IP adapters
offer none; use a [large MTU](#large-mtu-and-software-gro) there instead.

```c
uint32_t granted;
chmlink_set_offload(adapter, CHMLINK_OFFLOAD_ALL, &granted);
```

| bit | effect |
|---|---|
| `CHMLINK_OFFLOAD_TX_CHECKSUM` | Windows hands over TCP/UDP packets without the checksum computed |
| `CHMLINK_OFFLOAD_TX_LSO` | Windows hands over TCP data of up to 64 KiB as one packet (LSOv2) |
| `CHMLINK_OFFLOAD_RX_CHECKSUM` | you may mark injected packets as verified |
| `CHMLINK_OFFLOAD_RX_RSC` | you may inject coalesced TCP segments as one packet |

Offloads last for the session; `granted` says what Windows accepted. With
offloads on, packets carry a `chmlink_offload` descriptor (Linux
`virtio_net_hdr` semantics):

| field | meaning |
|---|---|
| `flags` | `CHMLINK_OFFLOAD_NEEDS_CSUM`: the L4 checksum at `csum_start + csum_offset` is not computed. `CHMLINK_OFFLOAD_DATA_VALID`: checksums are verified (injection only) |
| `gso_type` | `CHMLINK_GSO_TCPV4` / `CHMLINK_GSO_TCPV6`: one TCP packet that stands for several segments |
| `gso_size` | payload bytes per segment |
| `hdr_len` | bytes of Ethernet, IP and TCP header |
| `csum_start`, `csum_offset` | where the checksum is (offsets from the packet start) |

From Windows, `chmlink_receive_packet` fills the descriptor. If the other end
needs plain packets, `chmlink_gso_segment` cuts a GSO packet into wire
segments and `chmlink_offload_complete` fills in a missing checksum.

To Windows, pass a descriptor to `chmlink_allocate_packet`: `DATA_VALID` for a
packet whose checksums you verified, plus a GSO type for coalesced segments
of one TCP flow (indicated as RSC). Or let the
[coalescer](#large-mtu-and-software-gro) build them.

## Large MTU and software GRO

Most of Windows' cost is per packet, so fewer, larger packets are faster.

**Large MTU (IP mode).** Create the adapter with `mtu` up to 65535 (or change
it later). Windows then sends TCP as packets of up to the MTU, and accepts
injected packets of that size. Your application owns what a NIC would do:
before packets leave through a path with a smaller MTU, cut them with
`chmlink_ip_split`:

```c
static int forward(const uint8_t *piece, size_t length, void *context) { tunnel_send(piece, length); return 1; }

uint8_t scratch[1500];
chmlink_result r = chmlink_ip_split(packet, size, 1500, next_ipv6_id++, scratch, sizeof(scratch), forward, NULL);
if (r == CHMLINK_TOO_BIG) { /* IPv4 with DF that is not TCP: drop, or answer ICMP "fragmentation needed" */ }
```

TCP becomes complete segments; other IPv4 and IPv6 packets are fragmented.
With the default 1500 the adapter behaves like any NIC and none of this is
needed.

**Software GRO (both modes).** A tunnel delivers 1500-byte packets. A
coalescer merges consecutive TCP segments of a flow before they reach Windows:
into a large IP packet on an IP adapter with an MTU above 1500, or into an RSC
frame on an Ethernet adapter whose session has `CHMLINK_OFFLOAD_RX_RSC`.

```c
chmlink_coalescer *c = chmlink_coalescer_create(adapter, 0);   /* after chmlink_set_offload */
for (;;) {
    while (tunnel_has_packet()) {
        packet = tunnel_receive(&size);
        chmlink_coalescer_send(c, packet, size);
    }
    chmlink_coalescer_flush(c);   /* before waiting: segments are held until here */
    tunnel_wait();
}
chmlink_coalescer_free(c);        /* flushes; before chmlink_close */
```

- Other packets pass through unchanged; each flow keeps its order.
- One coalescer per sending thread; it is not thread-safe. Send and flush wait
  while the ring is full.
- IP mode reads no payload: the merged checksum is derived from the
  segments' checksums, and Windows still checks it. Ethernet mode verifies
  every segment's checksum, because Windows trusts RSC data; pass
  `CHMLINK_GRO_TRUSTED` when the data is already known to be intact (an
  authenticated tunnel) to skip that.
- Without a way to merge (MTU 1500, or no RSC), packets pass straight through.

`chmlink/gro.h` has the same merging as a portable, allocation-free API
(`chmlink_gro_init`, `chmlink_gro_add`, `chmlink_gro_flush`) with an output
callback, for applications that inject through their own path.

## Packet helpers

Portable functions (`chmlink/packet.h`, `chmlink/offload.h`), usable without a
session:

| function | |
|---|---|
| `chmlink_packet_valid(mode, data, length, limit)` | the framing checks the driver applies |
| `chmlink_ip_packet_valid(data, length, limit)` | one complete IPv4 or IPv6 packet |
| `chmlink_flow_hash(mode, data, length)` | the flow hash the driver spreads by (symmetric, past IPv6 extension headers; fragments without ports; 0 for non-IP) |
| `chmlink_checksum_add(data, length, seed)` | one's complement sum |
| `chmlink_offload_complete(mode, packet, length, &offload)` | fill in a `NEEDS_CSUM` checksum |
| `chmlink_gso_segment(...)` | cut a GSO packet into wire segments |
| `chmlink_ip_split(...)` | cut an IP packet to a smaller MTU |

## Batches

`chmlink_receive` and `chmlink_inject` use a simple buffer format
(`chmlink/protocol.h`): a 24-byte header, then for each packet its length,
optionally a 12-byte descriptor, and its bytes padded to four. Build one with
`chmlink_batch_begin` (or `_begin_offload`) and `chmlink_batch_append` (or
`_append_offload`); read one with `chmlink_batch_read_begin` and
`chmlink_batch_read_next`; check one with `chmlink_batch_validate`. A batch
holds up to 256 packets and 1 MiB. The zero-copy calls are faster; batches
suit code that wants one buffer per call.

## Diagnostics and versions

```c
static void log_line(chmlink_log_level level, const wchar_t *message, void *context)
{
    fwprintf(stderr, L"chmlink[%d] %ls\n", level, message);
}
chmlink_set_logger(log_line, NULL);
```

The SDK is silent without a logger. With one, it reports what it does (driver
installed, upgraded or kept, adapters created and deleted) and why something
failed, with the step and the Windows error text. The callback may run on
any thread; do not call `chmlink_set_logger` from it.

Versions are 64-bit values built with `CHMLINK_MAKE_VERSION(major, minor,
patch, build)`, so they compare as numbers:

| call | returns |
|---|---|
| `chmlink_get_sdk_version()` | this `chmlink.dll` (also in its file version resource) |
| `chmlink_get_driver_version(&v)` | the newest driver package on any adapter (`ERROR_FILE_NOT_FOUND` without adapters) |
| `info.version`, `info.min_version` | the device contracts the running driver speaks |

## Driver compatibility

Several applications may each ship their own `chmlink.dll`, of different
releases, while all adapters share one driver. The SDK and the driver agree on
a numbered device contract: a driver speaks a range of contracts, an SDK one;
a session opens when the range includes the SDK's. New contracts only add,
and a driver keeps speaking older contracts within a major version, so:

- an application keeps working when another one installs a newer driver of
  the same major version;
- creation never replaces a driver with an older one;
- an SDK that needs a newer driver than the one installed fails with
  `ERROR_REVISION_MISMATCH` and logs both versions.

## Errors

| `GetLastError()` | typical cause |
|---|---|
| `ERROR_INVALID_PARAMETER` | a wrong argument, size field or option |
| `ERROR_ACCESS_DENIED` | not an administrator; the wrong owner; not a ChmLink device |
| `ERROR_SHARING_VIOLATION` | another session is open on the adapter |
| `ERROR_NO_MORE_ITEMS` | nothing to receive right now |
| `ERROR_BUFFER_OVERFLOW` | the ring towards Windows is full; retry |
| `ERROR_TIMEOUT` | a wait timed out |
| `ERROR_OPERATION_ABORTED` | a wait was cancelled, or the session ended |
| `ERROR_NOT_READY` | the adapter is paused or the system sleeps |
| `ERROR_SHUTDOWN_IN_PROGRESS` | the system shuts down |
| `ERROR_REVISION_MISMATCH` | the driver and SDK do not share a contract, or the package is unusable |
| `ERROR_DUP_NAME` | the name, MAC or identity is in use |
| `ERROR_BUSY` | another process is changing adapters; or a second waiter |
| `ERROR_INSTALL_FAILURE` | a change was only partly applied; inspect the adapter |
| `ERROR_RESOURCE_NAME_NOT_FOUND` | no INF given and the DLL embeds no driver |
| `ERROR_INF_IN_USE_BY_DEVICES` | `chmlink_delete_driver` while adapters exist |

## Examples

| program | shows |
|---|---|
| [`examples/quickstart.c`](../examples/quickstart.c) | the whole lifecycle: create an IP adapter, address it, answer pings, delete it, with a logger |
| [`examples/list.c`](../examples/list.c) | list adapters and their counters without opening a session |
| [`examples/capture.c`](../examples/capture.c) | print what Windows sends through an adapter |
| [`examples/inject.c`](../examples/inject.c) | inject a raw frame or packet from a file |

`bench/packet_bench.c` and `bench/tcp_bench.c` are larger users of the API,
including several sending threads, offloads and the coalescer.
