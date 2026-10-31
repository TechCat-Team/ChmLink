# ChmLink

ChmLink is a Windows virtual network adapter: one driver package provides
Ethernet (TAP-like) and raw IP (TUN-like) adapters, and applications exchange
packets with them through a C library, `chmlink.dll`. MIT licensed.

It includes an NDIS 6.60 miniport with Ethernet and raw-IP adapters, a C SDK
(`chmlink.dll`) that creates adapters and exchanges packets with the driver through shared-memory rings (with
LSOv2, checksum and RSC offloads in Ethernet mode), a device provisioning tool,
examples, and a runtime CI for a Windows test VM. It is unreleased and
experimental: production signing and minimum-version and ARM64 testing are
not done, so it cannot replace TAP or Wintun in production yet. Install it only in a disposable test VM.

The compatibility target is Windows 10 version 1607 (build 14393) /
Windows Server 2016 and later. These are development targets, not verified
support. The planned NDIS baseline is 6.60.

## Layout

- `include/chmlink/` — shared C interfaces.
- `src/packet/` — packet validation and the software offload fallbacks (segmentation, checksums), independent of Windows APIs.
- `src/protocol/` — batch framing and validation, and the encodings of the device query and session requests.
- `src/ring/` — the packet ring shared by the driver and the SDK, portable so it can be tested off Windows.
- `driver/` — NDIS 6.60 miniport (Ethernet and raw IP) and INF package.
- `src/windows/` — user-mode C SDK, built as `chmlink.dll` (exports in `chmlink.def`): sessions (`adapter.c`), receiving, sending and waiting, with shared internals in `internal.h`; adapter management (`manage.c`, `include/chmlink/manage.h`) over the stored configuration (`device_config.c`).
- `examples/` — small programs using the C interface.
- `tools/` — driver build, provisioning and package maintenance tools.
- `tools/ci/` — runtime CI for the Windows test VM; see [tools/ci/README.md](tools/ci/README.md).
- `tests/unit/` — portable C tests (CTest) and the batch fuzzer.
- `tests/windows/` — runtime tests for a Windows test VM (lifecycle, IP, ARP, power, IOCTL fuzzing).
- `tests/support/` — helpers shared by tests and benchmarks.
- `bench/` — packet-rate, latency and TCP benchmarks for the test VM.

## Test

Run `cmake -S . -B build`, `cmake --build build`, and
`ctest --test-dir build --output-on-failure`. `python3 -m unittest discover -s tools/ci`
tests the CI scripts.

Every push runs the portable tests on Linux and Windows, a fuzz pass, an x64
driver build with `/W4 /WX`, and a rule that a changed driver needs a higher INF
`DriverVer` (see Package maintenance). `tools/build-driver.ps1 -Analyze` adds a
PREfast pass over the driver sources and fails on findings in them. The driver
rule sets shipped with the WDK are not used: the analysis engine rejects them on
the toolchain used here.

Runtime tests need a disposable Windows VM. `tools/ci/vm-run.sh` builds a
commit on the VM, signs it with the VM's test certificate, installs it over the
baseline driver, runs the lifecycle and IP suites and optionally the benchmark,
then restores the baseline. It works from a machine that can reach the VM; the
VM does not need internet access.

Every format crossing the kernel boundary uses explicit little-endian fields.
Request buffers (session registration, offload settings) are copied by the I/O
manager before the driver decodes them. The rings stay in application memory
while the driver reads them, so the driver reads every index and record size
once, validates it against the ring before use, and closes the session on any
inconsistency instead of trusting the application. The SDK validates a whole
batch before writing any of it. Packet checks cover Ethernet minimum size and
IP header and declared lengths, not checksums or protocol semantics. The
formats are experimental and may change before release.

For fuzz testing, build `tests/unit/fuzz_batch.c`, `src/protocol/batch.c`, and
`src/packet/packet.c` with Clang using `-Iinclude` and
`-fsanitize=fuzzer,address,undefined`.

For Windows lifecycle checks, build `chmlink-lifecycle-test` and run the following
from an elevated PowerShell in a disposable test VM with an installed driver
and the identical signed package:

```powershell
.\tools\test-lifecycle.ps1 -InfPath C:\packages\signed\chmlink.inf `
    -BuildDirectory .\build\verify\Release -OutputDirectory .\build\lifecycle-run-001
```

This opt-in runner creates two owned temporary adapters, assigns active-store
test addresses `169.254.250.1/24` and `169.254.251.1/24`, then removes the adapters
in cleanup. It refuses addresses already assigned locally. It exercises wait
timeout/cancel/close races, concurrent waiter rejection, abrupt child-process
termination, repeated GUID opens, ring saturation and recovery, 1000 uniquely
numbered UDP packets, adapter isolation, waits during disable/removal, SDK handle
release after concurrent calls from 16 threads, and IOCTL fuzzing (`chmlink-ioctl-fuzz`,
`-FuzzSeconds`, default 20): strictly on one adapter, and tolerantly on another
that is disabled and enabled while the requests run. With `-PowerCycle` it also suspends
and resumes the system with a session open (`chmlink-power-test`); this puts the
machine to sleep and is skipped where no sleep state exists.
Normal configuration traffic is drained before timing checks; readiness may
legally win a cancellation race. Process deadlines and per-stage stdout/stderr
logs distinguish hangs from assertion failures. The mutating suite is not
registered with CTest. A test failure does not establish a driver defect without
examining its logs and environment.

The CI's nightly lane runs this suite under Driver Verifier with NDIS
verification and under fault injection; see Status for the current result.

## Quick start

An application ships `chmlink.dll` alone: built with `CHMLINK_DRIVER_DIR`
pointing at a signed `chmlink.inf`, `chmlink.sys` and `chmlink.cat`, the DLL
carries the driver package and installs it when it creates the first adapter.

```powershell
cmake -S . -B build -DCHMLINK_DRIVER_DIR=C:\path\to\signed
cmake --build build --config Release
.\build\Release\chmlink-quickstart.exe   # as administrator; then: ping 192.0.2.2
```

`examples/quickstart.c` is the whole lifecycle in about a hundred lines:

```c
chmlink_create_options options = {sizeof(options)};
chmlink_device device;
options.mode = CHMLINK_MODE_IP;            /* or CHMLINK_MODE_ETHERNET */
options.name = L"My VPN";                  /* the Windows connection name */
chmlink_create_adapter(&options, &device); /* installs the driver if needed */
chmlink_adapter *adapter = chmlink_open(device.interface_index);
/* add addresses with CreateUnicastIpAddressEntry, then move packets:
 * chmlink_receive_packet / chmlink_release_packet (from Windows),
 * chmlink_allocate_packet / chmlink_send_packet (to Windows) */
chmlink_close(adapter);
chmlink_delete_adapter(device.instance_id, NULL);
```

`include/chmlink/manage.h` creates, lists, changes and deletes adapters;
`include/chmlink/api.h` (which includes it) runs the data session. All calls
return nonzero on success and report errors through `GetLastError()`.

All adapters share one driver package. When a DLL creates an adapter, the
newer of its embedded package and the one the existing adapters run wins: an
older package is upgraded on every adapter, unless one has a data session
open, in which case the existing package is kept when it supports creation.
`chmlink_delete_driver` removes packages that no adapter uses, for
uninstallers. Every adapter records an owner GUID (`options.owner`); deleting
it or changing its MTU requires the same owner, so applications do not remove
each other's adapters.

## Windows driver and SDK

### Compatibility between chmlink.dll and the driver

Several applications may each ship their own `chmlink.dll`, of different
releases, while all adapters share one driver. The driver and the SDK
therefore agree on a device contract (`CHMLINK_DEVICE_VERSION` in
`include/chmlink/version.h`, 6 at present): a driver speaks a range of
contracts, from `CHMLINK_DEVICE_VERSION_MIN` up, and reports it in QUERY; the
SDK names its contract when it opens a session and refuses a driver whose
range does not include it, rather than guess. A new contract only adds (new
IOCTLs, capability bits, fields in reserved or appended bytes, behavior a
session opts into), and the oldest contract a driver speaks rises only with a
new major version. So a `chmlink.dll` keeps working when a newer driver of the
same major version is installed, and creation never replaces a driver with an
older one. `chmlink_get_sdk_version`, `chmlink_get_driver_version` and the
`version`/`min_version` fields of `chmlink_adapter_info` report what is in use;
the DLL also carries a Windows version resource.

`chmlink_set_logger` receives what the SDK does (driver installed or upgraded,
adapters created and deleted) and why something failed, with the Windows error
text; without a logger the SDK is silent and errors come only from
`GetLastError()`.

### Sessions

Batches are an SDK convenience over the rings.
`chmlink-list` enumerates accessible adapters and prints their GUID, state,
capabilities and cumulative counters without connecting a data session.
`chmlink_open_by_id` opens by interface GUID and checks the resolved identity;
the GUID remains stable for that installed adapter, but can change on removal
and recreation. Queries remain available while the data path is paused.

Drivers advertising `CHMLINK_CAP_WAIT` support `chmlink_wait_readable` and
`chmlink_cancel_wait`. Readiness uses a referenced private event, not a pending
kernel I/O request. Data arrival, pause, removal and session cleanup wake the
waiter; state is rechecked after wakeup. One wait is allowed per session.
Timeouts, cancellation and lifecycle errors are reported through GetLastError.
Cancellation affects only the active wait. Readiness does not reserve packets.
Before announcing that it sleeps, a wait polls the ring for a few tens of
microseconds: at high packet rates the next packet usually arrives within that
time, which spares the driver a wake-up per packet.
The caller must prevent new SDK calls before closing and close exactly once;
close wakes a waiter and drains in-progress calls before freeing resources.

`chmlink-arp-test <ifIndex> <local-ipv4> <unused-peer-ipv4>` tests both
directions in one session. It requires a link-local IPv4 address already
assigned to the adapter and a distinct unused link-local peer with no cached
neighbor entry. It responds to the system's ARP request and passes only when
Windows resolves the simulated MAC. Run it in an isolated test environment;
the tool does not configure addresses or remove existing neighbor entries.

Build user-mode tools with the CMake commands above in a Visual Studio C++
environment. Run `powershell -File tools/build-driver.ps1 -Restore` to build
the x64 driver using pinned Microsoft WDK/SDK packages. This creates an
unsigned `.sys`, INF and catalog in `build/driver/`. The driver workflow
checks compilation, INF validation and catalog creation; it does not load
the driver or certify compatibility with the minimum operating system.

The INF uses OS-specific installation sections: builds before 25319 use
the system drivers directory (DIRID 12), and newer builds run from the Driver
Store (DIRID 13). The build validates the complete INF with `/u` and applies
`/w` isolation checks from build 25319 onward.

The current adapter has an MTU of 1500 and accepts inline Ethernet frames up
to 1522 bytes, including VLAN headers but excluding FCS. A session may ask for
LSOv2, TCP/UDP checksum and RSC offloads; they stay off until it does. Offloads
exist in Ethernet mode only: the Windows TCP/IP stack does not use task offloads
on `NdisMediumIP` adapters (measured on build 26200 with the same configuration
on both modes), so an IP adapter reports no `CHMLINK_CAP_OFFLOAD` and grants none.
Coalesced segments injected with RSC must arrive as one TCP flow in order; the
SDK's `chmlink_gso_segment` and `chmlink_offload_complete` turn offload records
into plain packets for applications that want them. Each
adapter has one exclusive administrator/SYSTEM session. The data path is a pair
of shared-memory rings (`include/chmlink/ring.h`), 4 MiB per direction by
default: the driver produces Windows traffic into one, and injects packets the
application produces into the other, without a copy or a per-batch IOCTL. A
packet that does not fit is dropped and counted in `tx_queue_drops`.
Each thread that injects gets its own to-Windows ring, up to four per session,
so the driver indicates several threads' packets on several cores; order holds
per thread. `chmlink_receive` and `chmlink_inject` remain batch-shaped
conveniences over the rings; `chmlink_inject` takes a whole batch or none of it (a full ring returns
`ERROR_BUFFER_OVERFLOW` and the batch can be retried as is); `chmlink_receive_packet`, `chmlink_release_packet`,
`chmlink_allocate_packet` and `chmlink_send_packet` are zero copy.
Packet injection honors the Windows packet filter and multicast list. Pausing
closes the rings, waits for every indicated packet to come back and drains
active I/O; removing the device invalidates existing sessions.

Install only a test-signed package in a disposable Windows test VM configured
for test signing. Both the driver binary and the final catalog require test
signing; regenerate the catalog after signing the binary. Once Windows trusts
the test certificate, use an elevated terminal:

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath C:\path\to\signed\chmlink.inf -Name 'ChmLink Test'
Get-NetAdapter -IncludeHidden -Name 'ChmLink Test'
.\build\Release\chmlink-capture.exe <ifIndex>
```

Configure a test IP address on that adapter and generate traffic to observe
outgoing frames. `chmlink_inject` accepts the batch format for the opposite
direction; `chmlink-inject.exe <ifIndex> <frame.bin>` submits a raw Ethernet
frame from a file. Close capture before running this separate example because
the session is exclusive. `chmlink-device`, the command-line form of
`manage.h` used by the PowerShell managers, takes an explicit INF: it restricts
driver selection to that INF and installs it only on the newly created device,
which it also names. It does not upgrade other matching adapters. It prints JSON containing the device instance
ID and persistent configuration. To remove a device, stop capture and pass the
exact instance ID to `chmlink-device.exe remove <instance-id>`. Supply the same
`--owner <GUID>` used at creation when using a custom owner.

All instances still share the `chmlink` driver service. Selecting a different
package is not an isolated per-adapter upgrade; additional adapters should use
the same package. Native creation compares the supplied INF, SYS and CAT against
the packages bound to existing ChmLink instances and refuses different content;
upgrade the existing instances explicitly before creating additional adapters.

## IP adapter

The same INF describes `Root\ChmLink` (Ethernet) and `Root\ChmLinkIP` (raw IP).
IP mode uses `NdisMediumIP` and one complete IPv4 or IPv6 packet per ring
record, without task offloads (see above). The API's mode and capability fields identify
the data format; IP metadata has a zero MAC. IPv6 jumbograms are not supported.
IP packet validation checks framing, not IP checksums or transport semantics.

In a disposable test VM, after signing and installing the experimental package,
the intended creation command is:

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath C:\packages\signed\chmlink.inf -Mode ip -Name 'ChmLink IP Test'
```

### MTU

An IP adapter's MTU is the largest IP packet it carries: 1500 by default,
configurable per adapter from 1280 (the IPv6 minimum) to 65535. Ethernet
adapters stay at 1500 and use task offloads instead. The value reaches NDIS
and the stack's interface MTU, and QUERY reports it as `mtu` and
`packet_limit`. Applications can still lower the interface MTU at run time
(`Set-NetIPInterface -NlMtuBytes`), not raise it above this value.

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath ... -Mode ip -Mtu 65535
.\tools\manage-adapter.ps1 -Action SetMtu -InstanceId 'ROOT\NET\0001' -Mtu 9000   # restarts the adapter
```

`chmlink-device create ... --mtu N` and `chmlink-device set-mtu <instance> N`
do the same. Changing the MTU restarts the adapter, which ends an open data
session.

A large MTU is the IP-mode counterpart of LSO and GRO: the stack hands over a
TCP flow as packets of up to the MTU instead of 1500-byte ones, so the
per-packet cost is paid far less often (see Performance). It is an opt-in,
because the application then owns what a NIC would do:

- Packets from Windows can be as large as the MTU. Before they leave through a
  path with a smaller MTU, cut them with `chmlink_ip_split` (`chmlink/offload.h`):
  TCP becomes complete segments, other IPv4 and IPv6 packets are fragmented.
  An IPv4 packet with DF set that is not TCP is refused (`CHMLINK_TOO_BIG`);
  drop it or answer with ICMP "fragmentation needed".
- Packets injected into Windows may be as large as the MTU, so an application
  can merge consecutive TCP segments of a flow before injecting them. Plain
  packets are always fine.
- TCP connections over the adapter advertise an MSS derived from the MTU; the
  peer still sends no more than its own MTU allows.

With the default 1500, the adapter behaves like any NIC and none of this is
needed. Wintun, by contrast, reports 65535 and leaves the interface there
until the application lowers it.

With IPv4/IPv6 addresses and routes explicitly configured on the test adapter, `chmlink-ip-test <ifIndex>
<local4> <peer4> <local6> <peer6>` is intended to exercise UDP in both
directions using simulated peers. An optional final Ethernet interface index
adds an isolation check. It opens exclusive sessions and does not configure the
addresses or routes. Use isolated addresses and run only in the test VM.
`chmlink-capture` and `chmlink-inject` select the reported adapter format.

`tools/test-ip.ps1 -InfPath <signed-inf> -OutputDirectory <new-directory>` is
an opt-in test runner for the disposable VM. It creates owned IP and Ethernet adapters,
temporarily configures documentation-range IPv4/IPv6 addresses in ActiveStore,
executes the two UDP roundtrips and an isolation check, then removes both. The addresses and
peer routes must be isolated from the rest of the test VM. The runner and test
are not registered in CTest; the runtime CI runs them in every lane.

Both modes share one service and package, so package upgrades affect all
instances. Stop clients and use version 5 maintenance tooling for both modes.
Do not use older management tools while IP devices exist: their inventory does
not include the IP hardware ID. A downgrade is refused if the target INF lacks
the IP model.

## Persistent adapter configuration

Each device stores an immutable adapter GUID, owner GUID and mode, and its MTU,
in a 64-byte versioned registry value (configuration version 3, written by the
device tool from 0.4.2.0 on), together with its desired name. Drivers from
0.4.2.0 on also read version 2 from earlier tools, which has no MTU and means
1500; the maintenance script refuses to install a package older than 0.4.2.0
while any device has a version 3 configuration. Ethernet also stores a locally
administered unicast MAC; IP has no MAC address. The driver
reads this state from the device hardware key rather than deriving a MAC from
the interface index. Removing and recreating a device creates a new identity;
this is not persistence across device deletion or cloning to another machine.
The adapter GUID is distinct from the Windows interface GUID used by the SDK.

Ethernet creation generates a random MAC unless `-Mac '02:11:22:33:44:55'` is supplied.
Names default to `ChmLink-<adapter-guid>`. Configuration rejects multicast and
globally administered MACs, empty GUIDs, malformed values, duplicate configured
identities/names/MACs and conflicts with MACs visible in the local interface
table. This cannot detect conflicts on remote machines. The driver independently
refuses duplicate MACs or adapter IDs among its active instances.

Applications should supply their own fixed `-OwnerId <GUID>`. The default owner
is reserved for standalone provisioning. Removal requires a matching owner ID;
ownership is an administrative mistake-prevention check, not a security boundary
against administrators. Package maintenance still coordinates every instance
because they share a driver service.

Recreating a device assigns a new identity and can change its Windows
interface GUID and network configuration. The tool does not upgrade or remove a
device whose configuration it cannot validate.

`manage-adapter.ps1` applies the stored name to the exact Windows interface GUID
after native creation. Native `create` alone sets the device friendly
name; the script also sets the network connection alias. If the interface is not
ready or naming fails, it keeps the device and reports partial completion. Retry
without creating another adapter:

```powershell
.\tools\manage-adapter.ps1 -Action ApplyName -InstanceId 'ROOT\NET\0000'
```

Use the same owner ID on retries. Creation returns native code 3 when
device cleanup or configuration persistence is uncertain. The wrapper also
returns 3 when naming is incomplete; 3010 means a manual restart is required.
It never removes a successfully created device to undo a naming failure.

## Package maintenance

`tools/manage-driver.ps1` provides explicit upgrade and recovery; the runtime CI
installs every build through it and restores the baseline afterwards. It covers
invalid configuration rejection, ownership checks, naming retry, upgrades,
rollback and identity persistence across a reboot. Minimum-version
compatibility is not tested yet; use a disposable test VM with a snapshot. The
script targets Windows PowerShell 5.1 and the same Windows baseline as the
driver.

Windows keys a published driver package on its INF: a new binary with an
unchanged `DriverVer` is silently not installed, and the upgrade fails its
payload check. Raise `DriverVer` in `driver/chmlink.inf` together with
`CHMLINK_VERSION_*` in `include/chmlink/version.h` whenever driver code changes;
`version_test` keeps the two equal and CI rejects a changed driver that keeps the
old version.

Plan is read-only and reports every installed ChmLink instance, including
disconnected devices, together with its package, identity and network settings:

```powershell
.\tools\manage-driver.ps1 -Action Plan -InfPath C:\packages\next\chmlink.inf
```

Stop all ChmLink clients and provisioning tools before changing packages. Run
the remaining commands in an elevated PowerShell, using a freshly built version
4 `chmlink-device.exe`. `-MaintenanceWindow` acknowledges that clients have been
stopped. The idle probe briefly opens a data session and can change link state;
it cannot prevent a client from opening later. The maintenance mutex serializes
this script only. Keep other installers and low-level device tools stopped.

```powershell
.\tools\manage-driver.ps1 -Action Upgrade `
    -InfPath C:\packages\next\chmlink.inf `
    -RecoveryDirectory C:\ChmLinkRecovery\upgrade-001 -MaintenanceWindow

.\tools\manage-driver.ps1 -Action Rollback `
    -JournalPath C:\ChmLinkRecovery\upgrade-001\journal.json -MaintenanceWindow

.\tools\manage-driver.ps1 -Action RemovePackage `
    -PublishedInf oem42.inf -MaintenanceWindow
```

Upgrade requires all instances to use the same package. It exports the old
package, copies the new INF/SYS/CAT payload, hashes the files and persists a
recovery journal before installing on each exact device instance. Use a new
recovery directory on a trusted local NTFS volume; protect it from modification
by other users and keep it until recovery is no longer needed. The journal
records machine identity, interface GUIDs, persistent configuration, bindings, attempted changes and
restart requirements. Address, route and DNS entries are diagnostic snapshots;
the script does not reapply network settings. It preserves existing devices
rather than deleting and recreating them.

The target version must increase unless `-AllowDowngrade` explicitly requests
a repair or downgrade. Windows checks package signatures and compatibility
during installation. When no restart is requested, the script checks the bound
package files and the configured service image on disk. This does not verify
the kernel's loaded image or packet traffic.

On installation failure, recovery attempts the old bindings in reverse order,
including the instance whose installation failed. Recovery retains the journal
and reports incomplete steps. If Windows requests a restart during upgrade,
failure recovery is deferred until after that restart. Restart manually, then
rerun Rollback with the same journal. If rollback itself requires a restart,
rerun Rollback after restarting to finish checking the recorded package files.
Changed device inventories or bindings outside the transaction require manual
review. Windows package installation across multiple devices is not atomic;
recovery is best effort and cannot promise uninterrupted networking.

| Exit code | Meaning |
| --- | --- |
| 0 | Requested operation completed without a reported restart requirement. |
| 1 | Operation refused or failed before this invocation changed devices. |
| 2 | Upgrade failed and recovery was applied; inspect the journal for restart requirements. |
| 3 | Recovery is incomplete or deferred; retain the entire recovery directory. |
| 3010 | Requested upgrade or rollback was applied and requires a manual restart. |

The script never restarts Windows, removes devices, deletes recovery files or
forces package deletion. Version 4 tooling writes journal schema 3 and refuses
older journals; retain the corresponding older tools for any older recovery
directories. RemovePackage accepts only a recognized ChmLink OEM
INF and refuses packages still associated with devices. Windows also guards
the final deletion through `SetupUninstallOEMInf` with no force flag. Package
cleanup is explicit and separate from upgrade and rollback.

## Performance

`tools/bench.ps1` runs `bench/packet_bench.c` and `bench/tcp_bench.c` on
the test VM (AMD Ryzen 9 9950X host, 4 vCPUs, Windows 11 build 26200, package
0.4.2.0, medians of three runs). These are in-VM figures, not network figures:
there is no encryption and no physical NIC.

| scenario | shared rings (v2) | IOCTL batches (v1, 8 vCPUs) |
|---|---|---|
| UDP from Windows, one sender, 64 B | 468k pps | 460k pps |
| UDP from Windows, four senders, 64 B | 1.27M pps (1.61M with 8 vCPUs) | 1.54M pps |
| UDP to Windows, one injector, batches of 1 | 663k pps | 184k pps |
| UDP to Windows, one injector, batches of 32 | 634k pps | 536k pps |
| UDP to Windows, two injector threads | 1.19M pps | 1.06M pps |
| 64 B round trip, median / 99th percentile | 15 / 69 µs | 59 / 299 µs |
| TCP through the stack, IP mode, one stream | 20.6 Gbps | about 1.6 Gbps (VM slow at the time) |
| TCP, IP mode at MTU 65535, cut to 1500 before injecting | 21.0 Gbps, 19% less CPU | — |
| TCP, IP mode at MTU 65535, injected whole | 96 Gbps (50 KB per packet) | — |
| TCP, Ethernet mode, one stream, no offloads | 19.3 Gbps | — |
| TCP, Ethernet mode, one stream, LSO + RSC | 117 Gbps (45 KB per packet) | — |
| TCP, Ethernet mode, four streams, LSO + RSC | 66.5 Gbps | — |

The stack processes a receive indication on the indicating thread, so one
to-Windows ring is indicated at about one core's worth (650k pps). The SDK
gives each injecting thread its own ring, up to four per session
(`CHMLINK_IOCTL_ADD_INJECT_RING`), each indicated by its own driver thread:
two injecting threads reach 1.19M pps. Packets from one thread stay in order;
packets from different threads are not ordered against each other. A consumer
stalled for 20 ms every 100 ms still receives 84% to 100% of the offered rate,
depending on packet size, with the default 4 MiB ring. `tools/ci/vm-run.sh run
full` gates a build against the recorded baseline for the machine profile.

The TCP benchmark reflects every packet back into the same adapter, so one
host carries both ends. At MTU 65535 the sending side gets large packets and
uses less CPU, but when the reflector cuts them to 1500 bytes the receiving
side still processes 1500-byte packets, and that side is the limit; on two
hosts the sender keeps the saving. Injecting them whole shows what a tunnel
that carries large packets end to end reaches.

`tools/bench-wintun.ps1` runs the same IP-mode benchmarks over Wintun 0.14.1
(`bench/wintun_shim.c`) on the same VM. With both at MTU 1500, ChmLink
injects about 1.9 times as many packets per second from one thread and 3.5
times from two, carries TCP about 5 times as fast, uses about half the CPU per
injected packet, and matches Wintun for packets from Windows and round-trip
latency. At Wintun's default MTU of 65535 with packets passed whole, Wintun
reaches 52 Gbps of TCP against ChmLink's 96 Gbps above.

## Status

Not production ready. Open work: signing and installation outside a test-signed
VM, minimum-version and ARM64 testing, running the sleep/resume test on a
platform with sleep states, and surprise-removal testing. Sleep is reported to a session as
`CHMLINK_STATE_LOW_POWER`, a system power-off as `CHMLINK_STATE_SHUTDOWN`
(`ERROR_SHUTDOWN_IN_PROGRESS` from a wait), and a surprise removal as
`CHMLINK_STATE_REMOVING`. Package 0.4.1.0 (shared rings, per-thread injection rings and offloads) passed
the smoke, full and nightly lanes: lifecycle (with IOCTL and ring-content
fuzzing), IP and stress suites under standard Driver Verifier with NDIS
verification, then adapter churn and fuzzing under randomized and under
systematic low resources simulation (initialization failures included), with no
leaked allocations, and reboots with a live session. The C interface and device
contract are experimental.

## License

MIT. See [LICENSE](LICENSE).
