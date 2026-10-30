# ChmLink

ChmLink is a Windows virtual network adapter project for ChmLAN. It aims to
provide Ethernet and IP adapters through a single driver package, with tunnel
protocols implemented in the user-mode agent.

The project is at an early development stage. It includes an experimental
NDIS Ethernet driver, a synchronous C interface, a capture example, and a
device provisioning utility. IP mode, Rust device I/O, production signing,
and runtime qualification have not been completed. It cannot replace TAP or
Wintun in production yet.

An experimental raw-IP path includes a separate device ID, NDIS attributes,
packet queues, injection and SDK mode negotiation. Initial x64 builds and
IPv4/IPv6 UDP roundtrips passed in a Windows 11 build 26200 test VM, including
an Ethernet isolation check and a run with standard Driver Verifier. Broader
qualification remains incomplete; use a disposable test VM.

The compatibility target is Windows 10 version 1607 (build 14393) /
Windows Server 2016 and later. These are development targets, not verified
support. The planned NDIS baseline is 6.60.

## Layout

- `include/chmlink/` — shared C interfaces.
- `src/packet/` — packet validation, independent of Windows APIs.
- `src/protocol/` — experimental batch framing, validation and the device query encoding.
- `src/queue/` — the driver's transmit ring, portable so it can be tested off Windows.
- `driver/` — NDIS 6.60 miniport (Ethernet and raw IP) and INF package.
- `src/windows/` — user-mode C interface.
- `examples/` — small programs using the C interface.
- `tools/` — driver build, provisioning and package maintenance tools.
- `tools/ci/` — runtime CI for the Windows test VM; see [tools/ci/README.md](tools/ci/README.md).
- `tests/` — portable C tests, Windows runtime tests and the benchmark.
- `rust/chmlink/` — Rust types for the user-mode integration.

## Test

Run `cmake -S . -B build`, `cmake --build build`, and
`ctest --test-dir build --output-on-failure`. Run `cargo test --workspace` for
the Rust crate. `python3 -m unittest discover -s tools/ci` tests the CI scripts.

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

The protocol uses explicit little-endian fields and bounded batches. The
receiver validates the complete batch before submitting packets. Driver code
must first copy user input into an owned snapshot; parsing a live shared buffer
does not provide protection against concurrent modification. The current
packet checks cover Ethernet minimum size and IP header/declared lengths;
they do not validate checksums or complete protocol semantics. The transport
format is experimental and may change before release.

For fuzz testing, build `tests/fuzz_batch.c`, `src/protocol/batch.c`, and
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
termination, repeated GUID opens, queue saturation and recovery, 1000 uniquely
numbered UDP packets, adapter isolation, and waits during disable/removal.
Normal configuration traffic is drained before timing checks; readiness may
legally win a cancellation race. Process deadlines and per-stage stdout/stderr
logs distinguish hangs from assertion failures. The mutating suite is not
registered with CTest. A test failure does not establish a driver defect without
examining its logs and environment.

On Windows 11 build 26200 x64, initial runs of this suite and the ARP roundtrip
passed with standard Driver Verifier enabled for the earlier 0.2.0.0 Ethernet
driver. The 0.3.0.0 development revision also passed an initial Ethernet
lifecycle run. Broader
NDIS/WLAN verification, prolonged stress, power transitions, fault injection and
minimum-version runtime coverage remain outstanding.

## Windows prototype

The current SDK requires device interface version 3. It rejects older drivers
without the metadata query instead of silently assuming compatible limits.
Version 1 packet batches and the existing receive/inject IOCTL numbers are unchanged.
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
to 1522 bytes, including VLAN headers but excluding FCS. Offloads are not
advertised. Each adapter has an independent transmit queue (Windows to user
mode) and one exclusive administrator/SYSTEM session. The queue is a 4 MiB byte
ring holding at most 16384 packets: small packets are bounded by the count,
full-size frames by the bytes (about 2700). A packet that does not fit is
dropped and counted in `tx_queue_drops`. One `chmlink_receive` returns at most
one batch, and the queue can hold more, so keep receiving until
`ERROR_NO_MORE_ITEMS`; callers can wait for readiness instead of polling.
The SDK issues its IOCTLs on an overlapped handle, so threads receiving and
injecting on the same adapter do not serialize each other; each call still
blocks until it completes. Packet injection honors the Windows
packet filter and multicast list. Pausing drops queued frames and drains
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
the session is exclusive. The provisioning utility restricts driver selection
to the supplied INF and installs it only on the newly created device. It does
not upgrade other matching adapters. It prints JSON containing the device instance
ID and persistent configuration. To remove a device, stop capture and pass the
exact instance ID to `chmlink-device.exe remove <instance-id>`. Supply the same
`--owner <GUID>` used at creation when using a custom owner.

All instances still share the `chmlink` driver service. Selecting a different
package is not an isolated per-adapter upgrade; additional adapters should use
the same package. Native creation compares the supplied INF, SYS and CAT against
the packages bound to existing ChmLink instances and refuses different content;
upgrade the existing instances explicitly before creating additional adapters.

## Experimental IP adapter

The same INF describes `Root\ChmLink` (Ethernet) and `Root\ChmLinkIP` (raw IP).
The prototype uses `NdisMediumIP`, a 1500-byte MTU and one complete IPv4 or
IPv6 packet per batch record. The API's mode and capability fields identify
the data format; IP metadata has a zero MAC. IPv6 jumbograms are not supported.
IP packet validation checks framing, not IP checksums or transport semantics.

In a disposable test VM, after signing and installing the experimental package,
the intended creation command is:

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath C:\packages\signed\chmlink.inf -Mode ip -Name 'ChmLink IP Test'
```

Initial creation and binding passed on Windows 11 build 26200 x64. With IPv4/IPv6 addresses and
routes explicitly configured on the test adapter, `chmlink-ip-test <ifIndex>
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
are not automatically registered in CTest. Initial normal and standard Driver
Verifier runs passed in the Windows 11 VM.

Both modes share one service and package, so package upgrades affect all
instances. Stop clients and use version 4 maintenance tooling for both modes.
Do not use older management tools while IP devices exist: their inventory does
not include the IP hardware ID. A downgrade is refused if the target INF lacks
the IP model. Complete validation must cover mixed instances, interruption,
rollback, minimum Windows versions, signing and Driver Verifier.

## Persistent adapter configuration

Driver packages from 0.3.0.0 on require configuration version 2. Each device stores an
immutable adapter GUID, owner GUID and mode in a 64-byte versioned registry
value, together with its desired name. Ethernet also stores a locally
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

This unreleased version deliberately breaks configuration version 1 and removes
the old Adopt command. Before replacing an older test package, back up the old
package and the device identity, address, route and DNS settings. Remove old
devices using their matching old tool and owner ID, then install the new package
and create new devices. Recreating a device assigns a new identity and can
change its Windows interface GUID and network configuration. The new tool does
not upgrade or remove a device whose configuration it cannot validate. Preserve
the old signed package and version 3 tool before building into any directory
that could overwrite them; use a separate build directory for 0.3.0.0.

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

`tools/manage-driver.ps1` provides explicit upgrade and recovery. Initial x64
builds and VM checks have passed on Windows 11 build 26200 with PowerShell 5.1:
legacy migration, invalid configuration rejection, ownership checks, naming
retry, upgrades, rollback, ARP traffic and identity persistence across a reboot.
Those results apply to the earlier 0.2.0.0 Ethernet package. The 0.3.0.0
prototype separately passed the x64 build, INF and catalog checks, initial
dual-stack IP roundtrips and an Ethernet lifecycle run. This is initial
coverage; minimum-version compatibility, Driver Verifier, stress, power and
fault-injection qualification remain incomplete. Continue using a disposable
test VM with a snapshot. The script targets Windows PowerShell 5.1 and the same
Windows baseline as the driver.

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

`tests/windows_bench.c`, run through `tools/bench.ps1` on the test VM, measures an
IP adapter in a VM on an AMD Ryzen 9 9950X host with 8 vCPUs, Windows 11 build
26200. These are in-VM figures, not network figures: there is no encryption and
no physical NIC. A single thread moves roughly 450k to 550k packets per second
in each direction (about 5 Gbps with 1400-byte payloads), with 64-byte round
trips at a median of about 60 microseconds and a 99th percentile of about 300.
Both directions at once hold the same rates. A consumer stalled for 20 ms every
100 ms loses well under 1% of 64-byte packets, about 7% of 512-byte ones and
about 14% of 1400-byte ones, which is the limit of the 4 MiB queue. Run-to-run spread is up to about 14%.
`tools/ci/vm-run.sh run full` compares a build with a recorded baseline.

The data path uses one IOCTL per batch. A shared-memory ring would cut system
calls and copies further; it would change the device contract and is not done.

## Status

Not production ready. Open work: signing and installation outside a test-signed
VM, minimum-version and ARM64 testing, power transitions (the PnP and shutdown
callbacks are empty), longer stress and fault injection, the Driver Verifier low
resources options, and the Rust device I/O and agent integration. Lifecycle and
IP suites last passed with standard Driver Verifier on package 0.3.1.0. The C
interface and device contract are experimental.

## License

MIT. See [LICENSE](LICENSE).
