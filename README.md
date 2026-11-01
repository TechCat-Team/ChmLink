# ChmLink

**English** | [简体中文](README.zh-CN.md)

[![Check](https://github.com/TechCat-Team/ChmLink/actions/workflows/check.yml/badge.svg)](https://github.com/TechCat-Team/ChmLink/actions/workflows/check.yml)
[![Driver](https://github.com/TechCat-Team/ChmLink/actions/workflows/driver.yml/badge.svg)](https://github.com/TechCat-Team/ChmLink/actions/workflows/driver.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Fast virtual network adapters for Windows.** One driver provides IP
adapters (TUN) and Ethernet adapters (TAP); one DLL lets your VPN, overlay
network or packet tool create them and move packets through them.

> **Pre-release.** ChmLink is not yet signed for production use, so it
> installs only on machines with test signing enabled. Interfaces may still
> change before 1.0.

## Why ChmLink

- **Fast towards Windows.** Packets go through shared-memory rings without a
  copy or a system call each, and the driver hands them to the stack on up to
  four cores, by flow, as RSS does on a physical NIC: 1.75M packets per second
  from one thread, 5× Wintun's TCP throughput.
- **TUN and TAP in one package.** IP adapters for layer-3 tunnels, Ethernet
  adapters with a MAC for layer-2 ones, side by side.
- **Big-packet paths.** Ethernet adapters offer LSO, checksum offload and RSC;
  IP adapters take an MTU of up to 65535; software GRO merges a tunnel's
  1500-byte packets before Windows sees them.
- **One file to ship.** `chmlink.dll` carries the signed driver and installs,
  upgrades or reuses it on its own. Adapters get their Windows names, an owner,
  and are cleaned up on failure.
- **Safe to share.** Applications bundling different SDK releases coexist: the
  newest driver wins, and every SDK keeps working with newer drivers of the
  same major version.
- **Tested under pressure.** Validated under Driver Verifier, randomized and
  systematic low-resources fault injection and IOCTL and ring fuzzing, with a
  benchmark gate against regressions.
- **MIT licensed**, plain C, targeting Windows 10 1607 and later (x64).

## Performance

Same VM, same benchmark programs, MTU 1500 (details and method in
[docs/performance.md](docs/performance.md)):

| | ChmLink | Wintun 0.14.1 | tap-windows6 9.27.0 |
|---|---|---|---|
| UDP to Windows, one thread, 64 flows | **1.75M pps** | 348k pps | 74k pps |
| UDP to Windows, one thread, one flow | **550k pps** | 350k pps | 72k pps |
| UDP from Windows, one sender | 479k pps | 473k pps | 45k pps |
| TCP, one stream | **21.0 Gbps** | 4.2 Gbps | 0.89 Gbps |
| TCP at MTU 65535 | **97 Gbps** | 52 Gbps | — |
| 64 B round trip (median) | 14.6 µs | 14.7 µs | 41 µs |

## Quick start

```c
#include <chmlink/api.h>

chmlink_create_options options = { sizeof(options) };
chmlink_device device;
options.mode = CHMLINK_MODE_IP;              /* or CHMLINK_MODE_ETHERNET */
options.name = L"My VPN";                    /* the Windows connection name */
chmlink_create_adapter(&options, &device);   /* installs the driver if needed */

chmlink_adapter *adapter = chmlink_open(device.interface_index);
/* add an address (CreateUnicastIpAddressEntry), then: */
for (;;) {
    uint32_t size;
    const uint8_t *packet;
    chmlink_wait_readable(adapter, CHMLINK_WAIT_INFINITE);
    while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) {
        /* a packet Windows sent: encrypt and forward it */
        chmlink_release_packet(adapter, packet);
    }
}
/* packets for Windows: chmlink_allocate_packet, fill in, chmlink_send_packet */

chmlink_close(adapter);
chmlink_delete_adapter(device.instance_id, NULL);
```

[`examples/quickstart.c`](examples/quickstart.c) is a complete program: it
creates an IP adapter, answers `ping 192.0.2.2` and removes the adapter on
exit. The [SDK guide](docs/sdk.md) covers everything else.

## Building

Requirements: Windows 10 or later (x64), Visual Studio 2022 with the C++
workload, CMake 3.16+, PowerShell 5.1. The WDK is fetched by the build script.

```powershell
powershell -File tools\build-driver.ps1 -Restore    # driver -> build\driver
# sign it for your test machine (docs/development.md), then:
cmake -S . -B build -A x64 -DCHMLINK_DRIVER_DIR=C:\path\to\signed
cmake --build build --config Release
cmake --install build --config Release --prefix C:\sdk\chmlink   # bin\chmlink.dll, lib\chmlink.lib, include\chmlink
```

## Documentation

| | |
|---|---|
| [SDK guide](docs/sdk.md) | creating adapters, sessions, sending and receiving, offloads, GRO, errors |
| [Architecture](docs/architecture.md) | how the driver and the SDK work: rings, spreading, offloads, the device contract |
| [Administration](docs/administration.md) | `chmlink-device`, PowerShell managers, driver upgrades with rollback |
| [Development](docs/development.md) | building, signing for tests, the test suites, versioning rules |
| [Performance](docs/performance.md) | benchmark results, comparison with Wintun and tap-windows6, method |
| [Runtime CI](tools/ci/README.md) | the test-VM pipeline: smoke, full and nightly lanes |

## Status

Done: both adapter kinds, the shared-ring data path with spreading by flow,
offloads, configurable MTU, software GRO, adapter management and driver
installation in the SDK (upgrades with open sessions, concurrent creation,
clean uninstall), version negotiation, surprise removal, and validation under
Driver Verifier and fault injection. Tested on Windows 11 and on Windows 10
1607, the minimum target, without the Visual C++ Redistributable.

Before 1.0: production signing, sleep and resume on hardware with sleep states
(the test VMs have none), ARM64 and x86 builds.

## Contributing

Issues and pull requests are welcome. Please read
[docs/development.md](docs/development.md) first: driver changes need a higher
`DriverVer`, and commit messages follow Conventional Commits.

## License

[MIT](LICENSE)
