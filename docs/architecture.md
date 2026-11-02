# Architecture

**English** | [简体中文](zh-CN/architecture.md)

How the driver and the SDK work. For using the SDK, see [sdk.md](sdk.md).

## Components

| part | where | role |
|---|---|---|
| driver | `driver/` | NDIS 6.60 miniport `chmlink.sys`; one package for both adapter kinds, `Root\ChmLink` (Ethernet) and `Root\ChmLinkIP` (IP, `NdisMediumIP`) |
| SDK | `src/windows/` | `chmlink.dll`: sessions, rings, adapter management, the embedded driver package |
| shared code | `src/ring/`, `src/protocol/`, `src/packet/` | the ring, the wire formats between driver and SDK, packet checks and helpers; portable C, compiled into both and tested off Windows |
| tools | `tools/` | `chmlink-device.exe` (the SDK's adapter management as a command line, linked statically) and the PowerShell managers |

## Data path

A session is a set of shared-memory rings that the SDK allocates in the
application and registers with the driver (`CHMLINK_IOCTL_REGISTER_RINGS`),
which locks and maps them:

- one ring **from Windows**: the driver produces in `MiniportSendNetBufferLists`,
  the application consumes;
- up to four rings **to Windows**: the application produces, and a driver
  thread per ring consumes and indicates. The first comes with the session;
  the SDK adds one per sending thread (`CHMLINK_IOCTL_ADD_INJECT_RING`).

A ring (`include/chmlink/ring.h`) has a power-of-two capacity from 128 KiB to
64 MiB (4 MiB by default) and a 256-byte header with four fields, each on its
own cache line: the consumer's `head`, the producer's `tail`, the consumer's
`wait` flag and the driver's `state` bits (`CHMLINK_RING_STATE_RUNNING`,
`CHMLINK_RING_STATE_CLOSED`). Records are `[u32 size][12-byte offload
descriptor][packet]`, padded to 16 bytes, and never wrap: a record that does
not fit before the end is placed at the start, and the rest of the end is
marked skipped.

Signalling follows Wintun: a consumer that finds its ring empty polls briefly,
sets `wait`, checks again and waits on an event; a producer sets the event
only when `wait` is set. Both sides can poll before sleeping: the SDK's
wait (up to 64 µs) and the driver's to-Windows threads (up to 100 µs,
yielding). At high packet rates and in request-response traffic the next
packet usually arrives within that time, which spares a wake-up per packet and
roughly halves the 64-byte round trip (15 µs instead of about 35 µs at the
median).

Polling is adaptive, because at moderate rates it would only keep a core busy.
Each side keeps a smoothed estimate of how long its ring stays empty before
the next packet (wake-ups without data do not count) and polls only while that
is under 64 µs; otherwise it sleeps at once. A request-response exchange stays
under the threshold even without polling, so it turns polling back on. With
10,000 evenly spaced packets per second each way a session then costs 0.21
cores instead of a core or more ([performance.md](performance.md#resource-use)).

**From Windows.** `MiniportSend` reserves space, copies the packet and
publishes it while holding the session's send lock, so senders on several
CPUs take turns on that lock; one copy of a packet is short next to the stack
work around it (four senders reach about 1.27M packets per second, as many as
Wintun). A packet that finds the ring full is dropped and counted
(`tx_queue_drops`).

**To Windows.** A ring's thread reads a batch of records, builds NBLs that
point into the locked ring (no copy, no `NDIS_RECEIVE_FLAGS_RESOURCES`) and
indicates them. Each ring tracks its batches; its head advances, in order, as
the stack returns them. Pausing waits for every indicated packet to return.

**Spreading by flow.** The stack processes a received packet on the thread
that indicates it, so one indicating thread tops out at one core (about 650k
packets per second here). Each session therefore starts one worker per active
processor, up to four (none on a single processor). The ring thread puts every
packet into a bucket by `chmlink_flow_hash` (addresses, protocol and TCP/UDP
ports, past IPv6 extension headers; symmetric, so both directions of a
connection share a bucket; fragments without ports, so all fragments of a
packet share one). Per
batch it indicates the largest bucket whose worker is idle itself, which costs
a single flow no hand-off, and queues the others to their workers. A bucket
with packets still in flight always goes to its worker, which keeps every flow
in order. This is what RSS does on a physical NIC.

**Untrusted memory.** The rings stay in application memory while the driver
reads them. The driver reads every index and record size exactly once,
validates it against the ring and closes the session on any inconsistency.
Request buffers (session registration, offload settings) are copied by the I/O
manager before they are decoded, and every format that crosses the kernel
boundary uses explicit little-endian fields. Packet checks cover the Ethernet
minimum size and IP header and length framing, not checksums or protocol
semantics. Injected packets honor the Windows packet filter and multicast list
in Ethernet mode.

## Device contract

Applications may ship their own `chmlink.dll` while all adapters share one
driver. The two agree on a numbered contract (`include/chmlink/version.h`):

- the driver speaks `CHMLINK_DEVICE_VERSION_MIN` to `CHMLINK_DEVICE_VERSION`
  and reports both in its query response;
- the SDK names its contract in `REGISTER_RINGS` and refuses a driver whose
  range does not include it;
- a new contract only adds: new IOCTLs, capability bits, fields in reserved or
  appended bytes that readers ignore, behavior a session opts into;
- the oldest contract a driver speaks rises only with a new major version.

So an SDK works with every newer driver of the same major version. Adapter
creation picks the newest package (see [sdk.md](sdk.md#which-driver-is-installed)).
The driver package version (`DriverVer`, `CHMLINK_VERSION_*`) is separate from
the contract.

## Adapter kinds

| | Ethernet (`Root\ChmLink`) | IP (`Root\ChmLinkIP`) |
|---|---|---|
| medium | 802.3 | `NdisMediumIP` |
| packets | Ethernet frames up to 1522 bytes, VLAN tags allowed, no FCS | one complete IPv4 or IPv6 packet each (no jumbograms) |
| MTU | 1500 | 1500 by default, 1280–65535 per adapter |
| offloads | LSOv2, TCP/UDP checksum, RSC, per session | none (Windows does not use task offloads on `NdisMediumIP`) |
| identity | MAC (locally administered unicast) | none |

**Offloads.** A session asks for them (`chmlink_set_offload`); they stay off
until it does, and Windows sees them advertised only while such a session is
open. Packets then carry a descriptor with Linux `virtio_net_hdr` semantics.
From Windows: LSO packets of up to 64 KiB and packets with checksums left to
compute, with IP lengths and the IPv4 header checksum fixed up by the driver.
To Windows: packets marked verified skip the stack's checks, and coalesced TCP
segments are indicated as RSC. On an IP-mode adapter Windows ignores the same
offload configuration (measured on build 26200), so IP adapters offer none.

**Large MTU.** On an IP adapter the MTU is the counterpart of LSO and RSC: the
stack sends TCP as packets of up to the MTU, and accepts injected packets of
that size. The default stays 1500 so the adapter behaves like any NIC; with a
larger value the application cuts packets for smaller paths
(`chmlink_ip_split`) and may merge them on the way in (the coalescer).

**Software GRO.** `src/packet/gro.c` merges consecutive TCP segments of a flow
(same addresses, ports, IP fields, acknowledgement and options; the next
sequence number; only ACK and PSH; every segment but the last as large as the
first) into a large IP packet or an RSC frame. In IP mode the merged checksum
is derived from the segments' checksum fields, so no payload is read; in
Ethernet mode, where Windows trusts RSC data, each segment is verified unless
the caller says the data is intact. TCP that does not show its ports (behind IPv6 extension
headers, or an IPv4 fragment) is not merged; whatever is held for the same
hosts goes out before it, so no flow is reordered.

## Configuration

Each adapter stores its identity in its device registry key: a 64-byte
versioned value (configuration version 3) with an adapter GUID, an owner GUID,
the mode, the MAC (Ethernet) and the MTU, plus the name. The driver reads it at
start, rather than deriving a MAC from the interface index, and refuses
duplicate MACs or adapter IDs among its instances. The identity is fixed at
creation; removing and recreating an adapter gives a new one, and the Windows
interface GUID may change with it. Drivers read version 2 from older tools as
MTU 1500.

## Lifecycle and power

Pausing (a configuration change, disable) closes the rings to new packets,
waits for indicated packets to return and drains I/O; the session survives and
reports `CHMLINK_STATE_PAUSED`. System sleep is reported as
`CHMLINK_STATE_LOW_POWER`, shutdown as `CHMLINK_STATE_SHUTDOWN`, removal as
`CHMLINK_STATE_REMOVING`; removal ends the session.

Pausing and closing a session wait without a time limit until the stack has
returned every packet indicated from the rings: those packets point into the
application's memory, which NDIS does not allow to be released while the stack
still holds them. The stack returns packets promptly; a component that kept
them would hold up the close (and the exit of the process that owns the
session) until it let go.

## Installation model

The INF has two installation paths: builds before 25319 copy the driver to the
system drivers directory (DIRID 12), newer builds run it from the Driver Store
(DIRID 13). All adapters share the `chmlink` service and therefore one package;
installing a package on one adapter is never done as a side effect of creating
another.
