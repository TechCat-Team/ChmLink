# Development

Building, testing and changing ChmLink.

## Layout

| path | content |
|---|---|
| `include/chmlink/` | public headers (`api.h`, `manage.h`, `packet.h`, `protocol.h`, `offload.h`, `gro.h`, `version.h`) and the driver contract (`device.h`, `query.h`, `session.h`, `ring.h`, `config.h`) |
| `driver/` | the NDIS miniport and its INF |
| `src/windows/` | the SDK (`chmlink.dll`, exports in `chmlink.def`) |
| `src/ring/`, `src/protocol/`, `src/packet/` | portable code shared by the driver and the SDK |
| `examples/` | small programs using the SDK |
| `tools/` | `chmlink-device`, the PowerShell managers, driver build, test and benchmark runners |
| `tools/ci/` | the runtime CI for a Windows test VM ([README](../tools/ci/README.md)) |
| `tests/unit/` | portable tests (CTest) and the batch fuzzer |
| `tests/windows/` | runtime tests for a test VM |
| `bench/` | packet-rate, latency and TCP benchmarks, with Wintun and tap-windows6 back ends |

## Building

User mode (SDK, tools, tests, examples), with Visual Studio 2022:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The portable parts and their tests also build on Linux (`cmake -S . -B build
&& cmake --build build && ctest --test-dir build`).

The driver, with pinned WDK and SDK packages fetched on first use:

```powershell
powershell -File tools\build-driver.ps1 -Restore -Analyze
```

This writes an unsigned `chmlink.sys`, the INF and a catalog to
`build/driver/`, validates the INF (`/u`, and `/w` isolation checks for build
25319 on), and with `-Analyze` runs PREfast over the driver sources and fails
on findings. The driver compiles with `/W4 /WX`.

### Signing for a test machine

Until release signing is in place, install only test-signed packages, on a
machine with test signing enabled (`bcdedit /set testsigning on`) and a
disposable snapshot. Sign `chmlink.sys`, regenerate the catalog with Inf2Cat,
then sign the catalog:

```powershell
signtool sign /fd sha256 /sha1 <test-cert-thumbprint> build\driver\chmlink.sys
Inf2Cat /driver:build\driver /os:10_X64,Server10_X64
signtool sign /fd sha256 /sha1 <test-cert-thumbprint> build\driver\chmlink.cat
```

Point `CHMLINK_DRIVER_DIR` at that directory to embed the package in
`chmlink.dll` ([sdk.md](sdk.md#getting-the-sdk)).

## Testing

| what | how |
|---|---|
| portable unit tests | `ctest` (above); includes ring, batch, query and session encodings, offload helpers, `chmlink_ip_split`, GRO, flow hash, configuration |
| batch fuzzer | `clang -fsanitize=fuzzer,address,undefined -Iinclude src/packet/packet.c src/protocol/batch.c tests/unit/fuzz_batch.c` |
| CI scripts | `python3 -m unittest discover -s tools/ci` |
| runtime suites | on a test VM, through `tools/ci/vm-run.sh` (below) or by hand |

Every push to GitHub runs the portable tests on Linux and Windows, the fuzzer,
the driver build with PREfast, a PowerShell parse check and the version rules
below.

### Runtime tests

These change the machine (adapters, addresses, sleep) and belong on a
disposable VM with the signed package:

| runner | covers |
|---|---|
| `tools\test-lifecycle.ps1 -InfPath <inf> -BuildDirectory <dir> -OutputDirectory <new>` | waits, cancellation, close races, ring saturation, process exit, disable and removal, 16-thread close, IOCTL and ring-content fuzzing, optionally sleep and resume (`-PowerCycle`) |
| `tools\test-ip.ps1 -InfPath <inf> -OutputDirectory <new>` | IPv4 and IPv6 round trips through an IP adapter at MTU 9000, Ethernet isolation, MTU changes, naming |
| `tools\test-lowres.ps1` | adapter churn and fuzzing under low-resources simulation (used by the nightly lane) |
| `chmlink-arp-test <ifIndex> <local-ipv4> <peer-ipv4>` | both directions on an Ethernet adapter via ARP |
| `chmlink-quickstart --self-test` | creation from the embedded package, a ping round trip, deletion |

`tools/ci/vm-run.sh` drives all of them from a Linux or macOS machine: it
builds a commit on the VM, signs it with the VM's test certificate, installs it
over a baseline driver, runs the suites, and restores the baseline. Lanes:
`smoke` (about 2 minutes), `full` (plus three benchmark rounds gated against a
recorded baseline), `nightly` (plus Driver Verifier, randomized and systematic
low-resources simulation, leak checks and reboots). See
[tools/ci/README.md](../tools/ci/README.md).

### Benchmarks

```powershell
.\tools\bench.ps1 -InfPath <signed-inf> -BuildDirectory build\Release -OutputDirectory <new>
.\tools\bench-wintun.ps1 -WintunDll <official wintun.dll> -BuildDirectory build\Release -OutputDirectory <new>
.\tools\bench-tap.ps1 -BuildDirectory build\Release -OutputDirectory <new>   # needs one TAP-Windows Adapter V9
```

The same benchmark programs run on all three drivers through
`bench/wintun_shim.c` and `bench/tap_shim.c`. Results and method:
[performance.md](performance.md).

## Versioning rules

- **Driver changes need a higher `DriverVer`.** Windows keys a published
  package on its INF: a new binary with an unchanged `DriverVer` is silently
  not installed. Raise `DriverVer` in `driver/chmlink.inf` together with
  `CHMLINK_VERSION_*` in `include/chmlink/version.h` whenever anything compiled
  into the driver changes (`driver/`, `src/ring/`, `src/protocol/`,
  `src/packet/`, the contract headers). `version_test` keeps the two equal;
  CI refuses a driver change without a higher version
  (`tools/ci/check_driver_version.py`).
- **The `DriverVer` date is a UTC date.** Inf2Cat refuses a date in the future,
  judged by the build machine's clock, and hosted runners use UTC.
- **The device contract only grows.** Changes to what crosses the kernel
  boundary follow the rules in [architecture.md](architecture.md#device-contract);
  raise `CHMLINK_DEVICE_VERSION` for a new contract, and
  `CHMLINK_DEVICE_VERSION_MIN` only with a new major version.
- **Configuration formats are versioned** (`include/chmlink/config.h`); drivers
  keep reading older versions.

Commit messages follow Conventional Commits (`feat(sdk): ...`,
`fix(driver): ...`, `!` for breaking changes).
