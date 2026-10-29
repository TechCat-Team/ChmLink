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
- `tools/` — driver build and device provisioning tools.
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
.\build\Release\chmlink-device.exe create C:\path\to\signed\chmlink.inf
Get-NetAdapter -IncludeHidden | Where-Object InterfaceDescription -Like 'ChmLAN Link*'
.\build\Release\chmlink-capture.exe <ifIndex>
```

Configure a test IP address on that adapter and generate traffic to observe
outgoing frames. `chmlink_inject` accepts the batch format for the opposite
direction; `chmlink-inject.exe <ifIndex> <frame.bin>` submits a raw Ethernet
frame from a file. Close capture before running this separate example because
the session is exclusive. The provisioning utility restricts driver selection
to the supplied INF and installs it only on the newly created device. It does
not upgrade other matching adapters. It prints the device instance and, when
available, its interface GUID. To remove a device, stop capture and pass the exact instance ID
printed during creation to `chmlink-device.exe remove <instance-id>`.

All instances still share the `chmlink` driver service. Selecting a different
package is not an isolated per-adapter upgrade; additional adapters should use
the same package. A managed upgrade/rollback workflow is still pending.

Before any production use, the prototype still needs Windows runtime tests
for bidirectional traffic, Driver Verifier, concurrent cleanup and removal,
power transitions, repeated installation and upgrades, plus minimum-version
and architecture testing. The C interface and device contract are experimental.

## License

MIT. See [LICENSE](LICENSE).
