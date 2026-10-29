# ChmLink

ChmLink is a Windows virtual network adapter project for ChmLAN. It aims to
provide Ethernet and IP adapters through a single driver package, with tunnel
protocols implemented in the user-mode agent.

The project is at an early development stage. It includes an experimental
NDIS Ethernet driver, a synchronous C interface, a capture example, and a
device provisioning utility. IP mode, Rust device I/O, production signing,
and runtime qualification have not been completed. It cannot replace TAP or
Wintun in production yet.

The compatibility target is Windows 10 version 1607 (build 14393) /
Windows Server 2016 and later. These are development targets, not verified
support. The planned NDIS baseline is 6.60.

## Layout

- `include/chmlink/` — shared C interfaces.
- `src/packet/` — packet validation, independent of Windows APIs.
- `src/protocol/` — experimental batch framing and validation.
- `driver/` — NDIS 6.60 Ethernet miniport and INF package.
- `src/windows/` — user-mode C interface.
- `examples/` — small programs using the C interface.
- `tools/` — driver build, provisioning and package maintenance tools.
- `tests/` — portable C tests.
- `rust/chmlink/` — Rust types for the user-mode integration.

## Test

Run `cmake -S . -B build`, `cmake --build build`, and
`ctest --test-dir build --output-on-failure`. Run `cargo test --workspace` for
the Rust crate.

The protocol uses explicit little-endian fields and bounded batches. The
receiver validates the complete batch before submitting packets. Driver code
must first copy user input into an owned snapshot; parsing a live shared buffer
does not provide protection against concurrent modification. The current
packet checks cover minimum header sizes and IP version, not full protocol
validity. The transport format is experimental and may change before release.

For fuzz testing, build `tests/fuzz_batch.c`, `src/protocol/batch.c`, and
`src/packet/packet.c` with Clang using `-Iinclude` and
`-fsanitize=fuzzer,address,undefined`.

## Windows prototype

The current SDK requires device interface version 2. It rejects older drivers
without the metadata query instead of silently assuming compatible limits.
Version 1 packet batches and the existing receive/inject IOCTLs are unchanged.
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
advertised. Each adapter has an independent 256-packet transmit queue and one
exclusive administrator/SYSTEM session. Nonblocking receives on empty
queues return `ERROR_NO_MORE_ITEMS`; callers can wait for readiness instead
of polling. Packet injection honors the Windows
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

## Persistent adapter configuration

Driver package 0.2.0.0 requires configuration version 1. Each device stores an
immutable adapter GUID, owner GUID and locally administered unicast MAC in a
64-byte versioned registry value, together with its desired name. The driver
reads this state from the device hardware key rather than deriving a MAC from
the interface index. Removing and recreating a device creates a new identity;
this is not persistence across device deletion or cloning to another machine.
The adapter GUID is distinct from the Windows interface GUID used by the SDK.

Creation generates a random MAC unless `-Mac '02:11:22:33:44:55'` is supplied.
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

Migrate old devices explicitly while their original driver is still running:

```powershell
.\tools\manage-adapter.ps1 -Action Adopt -InstanceId 'ROOT\NET\0000'
.\build\Release\chmlink-device.exe show 'ROOT\NET\0000'
```

Adopt preserves the current MAC, records the current connection name unless
`-Name` is supplied, and never overwrites an existing identity. It does not
install or restart a driver. Unmanaged devices remain visible in the package
Plan output; they must be adopted before installation of 0.2.0.0. Missing or
invalid configuration causes the new driver to refuse initialization instead
of generating a replacement identity. Recovery to older drivers retains the
configuration, but older binaries do not implement the new persistence contract.

`manage-adapter.ps1` applies the stored name to the exact Windows interface GUID
after native creation/adoption. Native `create` alone sets the device friendly
name; the script also sets the network connection alias. If the interface is not
ready or naming fails, it keeps the device and reports partial completion. Retry
without creating another adapter:

```powershell
.\tools\manage-adapter.ps1 -Action ApplyName -InstanceId 'ROOT\NET\0000'
```

Use the same owner ID on retries. Creation/adoption return native code 3 when
device cleanup or configuration persistence is uncertain. The wrapper also
returns 3 when naming is incomplete; 3010 means a manual restart is required.
It never removes a successfully created device to undo a naming failure.

## Package maintenance

`tools/manage-driver.ps1` provides explicit upgrade and recovery. Initial x64
builds and VM checks have passed on Windows 11 build 26200 with PowerShell 5.1:
legacy migration, invalid configuration rejection, ownership checks, naming
retry, upgrades, rollback, ARP traffic and identity persistence across a reboot.
The driver also passed the build's INF and catalog checks. This is initial
coverage; minimum-version compatibility, Driver Verifier, stress, power and
fault-injection qualification remain incomplete. Continue using a disposable
test VM with a snapshot. The script targets Windows PowerShell 5.1 and the same
Windows baseline as the driver.

Plan is read-only and reports every installed ChmLink instance, including
disconnected devices, together with its package, identity and network settings:

```powershell
.\tools\manage-driver.ps1 -Action Plan -InfPath C:\packages\next\chmlink.inf
```

Stop all ChmLink clients and provisioning tools before changing packages. Run
the remaining commands in an elevated PowerShell, using a freshly built version
3 `chmlink-device.exe`. `-MaintenanceWindow` acknowledges that clients have been
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
forces package deletion. Version 3 tooling writes journal schema 2 and refuses
older journals; retain the corresponding older tools for any older recovery
directories. RemovePackage accepts only a recognized ChmLink OEM
INF and refuses packages still associated with devices. Windows also guards
the final deletion through `SetupUninstallOEMInf` with no force flag. Package
cleanup is explicit and separate from upgrade and rollback.

Before any production use, the prototype still needs Windows runtime tests
for bidirectional traffic, Driver Verifier, concurrent cleanup and removal,
power transitions, repeated installation and upgrades, plus minimum-version
and architecture testing. The C interface and device contract are experimental.

## License

MIT. See [LICENSE](LICENSE).
