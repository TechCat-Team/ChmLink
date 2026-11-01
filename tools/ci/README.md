# ChmLink CI

Two layers. The test VM is not reachable from the internet and cannot reach GitHub, so the
runtime tests are pushed to it from a machine that can reach both.

| layer | where | what | when |
|---|---|---|---|
| cloud | GitHub Actions (`.github/workflows`) | portable tests, fuzz, driver build with `/W4 /WX` and PREfast, **DriverVer rule**, tests and lint of these scripts, PowerShell parse check | every push and PR |
| VM | the Windows test VM, driven by `vm-run.sh` | build and sign the candidate, install it over the baseline, lifecycle and IP suites, benchmark, **always restore the baseline** | on demand |

## Using the VM layer

One-time setup on the machine that runs `vm-run.sh` (macOS or Linux, needs `ssh`, `scp`, `git`, `python3`):

```sh
# ~/.chmlink-ci.env
export CHMLINK_VM_HOST=192.168.1.154
export CHMLINK_VM_USER=sshadmin
# optional: export CHMLINK_VM_SSH_OPTS="-i ~/.ssh/chmlink_vm"
```

Use an SSH key rather than a password: install your public key in
`C:\ProgramData\ssh\administrators_authorized_keys` on the VM (administrator accounts read that file,
not `~\.ssh`; it must be writable only by Administrators and SYSTEM).

```sh
tools/ci/vm-run.sh init --baseline-dir 'C:\path\on\vm\to\signed'   # once; see "VM prerequisites"
tools/ci/vm-run.sh doctor                    # is the VM ready?
tools/ci/vm-run.sh run smoke                 # HEAD: build, ctest, install, lifecycle, IP   (~1.5 min)
tools/ci/vm-run.sh run full                  # smoke + 3x benchmark gated on the baseline    (~8.5 min)
tools/ci/vm-run.sh run nightly               # smoke + Driver Verifier + two kinds of fault injection, four reboots
tools/ci/vm-run.sh run nightly --stress-minutes 1 --lowres-minutes 2 --systematic-minutes 3   # quick (~18 min)
#   defaults: 20 stress, 10 randomized and 5 systematic fault-injection minutes (0 skips a phase), or
#   NightlyStressMinutes / NightlyLowResMinutes / NightlySystematicMinutes in C:\ci\config.json
tools/ci/vm-run.sh renew-cert                # new test certificate; re-signs and re-installs the baseline
tools/ci/vm-run.sh run smoke --commit abc123 # any commit; the CI scripts are always this checkout's
tools/ci/vm-run.sh run smoke --worktree      # include uncommitted and untracked files
tools/ci/vm-run.sh run full --no-wait        # start and return; later:
tools/ci/vm-run.sh attach <run-id>           # follow it and fetch results (safe after a dropped connection)
tools/ci/vm-run.sh abort <run-id>            # kill a run, then:
tools/ci/vm-run.sh restore                   # put the baseline driver back and remove CI's leftover adapters
```

Exit status: `0` passed, `1` a test or the build failed, `2` benchmark regression, `3` the VM or infrastructure
needs attention, `64` usage. Results land in `build/ci-results/<run-id>/` (`summary.json`, `log.txt`,
per-suite output, `bench-summary.md` for `full`).

A run keeps going on the VM if your connection drops, because it is started detached and reports through files.

### What a run does

`preflight` (disk, tools, signing certificate, no reboot pending, only the expected adapters) →
`build-user` → `ctest` → `build-driver` (with `-Analyze`) → `sign` → `build-embedded` (chmlink.dll carrying the signed package) → `clean-store` → `install` →
`lifecycle` → `ip` → `quickstart` (creation from the embedded package, a ping, deletion) → `package` (upgrades with and without sessions, `chmlink_delete_driver`, concurrent creation, surprise removal; again as `verifier-package` in the nightly lane) → [`bench-1..3`] → `restore`. `restore` runs whatever happened before it. If the
previous run was killed, the next one notices (wrong driver image, leftover adapters owned by the test
suites) and repairs it first.

Only one run at a time; a second is refused with the id of the one holding the VM.

### The nightly lane (Driver Verifier)

Phase A is the smoke lane. It then registers a startup task (`ChmLinkCiResume`, SYSTEM; its run-time limit
follows the lane's deadline, since Task Scheduler silently ends a task past its limit), turns on the
**persistent** verifier setting for `chmlink.sys` (the standard options plus NDIS/WiFi verification,
`0x2209BB`) and reboots. The fault-injection phases below use the standard options only: with NDIS
verification the injection also fails NDIS's own allocations for the miniport, and AddDevice then fails
(Code 31) nearly every time before any chmlink code runs. Phase B runs after the boot: the lifecycle and
IP suites, a duplex benchmark and a stress loop (lifecycle, IP and a two-injector benchmark, repeated for
`--stress-minutes`), all with the driver verified from `DriverEntry` on. It ends by comparing the driver's live
pool allocations and bytes with the idle values taken right after boot; any growth is a leak. The verifier
setting is then cleared and a second reboot lands in phase C, which restores the baseline driver and writes
`summary.json`. Throughput is never compared with the baseline here, since Verifier slows the data path.

When phase B passed and `--lowres-minutes` is not 0, it switches to the standard checks plus **randomized low
resources simulation** (`0x4`, 6% of the driver's allocations failed from one minute after boot) and reboots
into phase L instead. `tools/test-lowres.ps1` then repeats, until the time is up: create an adapter, fuzz it in
tolerant mode while disabling and enabling it, remove it. Operations may fail; each PnP step runs with a
deadline, so a hang fails the run, and every adapter must still be removable. Phase L passes only if Verifier
reports deliberately failed allocations (otherwise no failure path ran) and the driver's live allocations are
back at the boot-idle level.

Phase S does the same under **systematic low resources simulation** (`0x40000`), switched on at run time
(`verifier /faultssystematic enableruntime`) once the idle allocations are measured. It fails every distinct
allocation call stack once, so each failure path runs at least once, `ClInitialize`'s included; it counts as
exercised when `querystatistics` reports a non-zero `InjectionCount` (these injections are not counted as
"failed deliberately"). Then Verifier is cleared and the last reboot lands in phase C.

Every reboot out of phase B, L or S happens with a **live session**: a detached fuzzer holds the resting
adapter's data session with requests in flight, so the driver's shutdown handler runs against it under
Verifier. A bugcheck on the way down still ends in the reboot that was asked for; the resume therefore also
looks for bugcheck and power-loss events (1001, 41, 6008) after the reboot request and treats them as a crash.

- An unexpected reboot or bugcheck in phase B is recognised by the boot id; the run records the bugcheck
  events and the newest minidump in `crash.json`, fails, and goes to phase C.
- After `abort` run `restore`: it also removes the startup task and the persistent setting, so that no later
  boot runs under verification or continues an abandoned run.
- Why persistent: `verifier /standard /driver chmlink.sys /now`-style volatile activation cannot be switched
  off, stops tracking a driver that is reloaded, and misses `DriverEntry`. The persistent form needs the reboot.
- The startup task starts seconds after boot, before PnP has loaded the adapter driver, so phase B waits up
  to 3 minutes for it to appear in `verifier /query`. `verifier /query` is localised; it is parsed by structure.
- `verifier /faults` and `verifier /flags` each reset the other's settings; the CI sets the flags and then
  writes `VerifierFaultProbability` and `VerifierFaultBootMinutes` directly.
- Not covered: surprise removal (a root-enumerated device cannot be surprise-removed without the WDK's
  Device Fundamentals tests), and sleep/resume on the current VM (VMware without S3/S4; see below).

### Sleep and resume

`chmlink-power-test` suspends the system with a session open (standby, else hibernation), resumes it with
a waitable timer 30 s later and checks that the session survived, the adapter is running and no longer in
low power, and the data path works. The lifecycle suite runs it when `"PowerTests": true` is in
`C:\ci\config.json`; it prints SKIP where the platform has no sleep state or the system did not sleep. In
VMware the guest needs VMware Tools (the basic display driver disables S1-S3) and standby allowed in the VM's
power options.

### IOCTL fuzzing

`chmlink-ioctl-fuzz` (`tests/windows/ioctl_fuzz.c`) issues raw requests below the SDK: the four contract
codes and foreign ones, edge lengths, mutated batches, wrong handle types for the read event, bad user
buffers, read/write/cancel. Workers share one session's file object and close their handles at random, so
`IRP_MJ_CLEANUP` races requests in flight; an inspection thread opens and closes read-only handles. In strict
mode every result must be the one the dispatch code assigns, and an injected batch must be accepted exactly
when `chmlink_batch_validate` accepts it. `--tolerant` (the adapter churns, or faults are injected) keeps only
"nothing invalid succeeds". The lifecycle suite runs it for `-FuzzSeconds` (20) strictly on one adapter and
tolerantly on another that is disabled and enabled meanwhile. A failure prints the seed; `--seed N` replays
the request sequence (thread timing differs).

The driver build writes `chmlink.pdb`; each run keeps it in its results, since a live dump or minidump of
that run is only readable with it. `LiveKernelReports` dumps that NDIS writes for a stuck callback are
deleted once Windows Error Reporting has processed them; copy them out early.

### Certificate renewal

`renew-cert` creates a new certificate, trusts it, re-signs the baseline package with a raised DriverVer
(Windows reuses a published package whose INF text is unchanged), reinstalls it and records the new
thumbprint in `C:\ci\config.json`. `-WhatIfRenew` on the VM script shows the plan only. The previous test
certificate expires on 2026-11-29.

### VM prerequisites

- Windows 10/11 x64, OpenSSH server, test signing on, an elevated account.
- Visual Studio 2022 Build Tools (C++), CMake, `tar`, and the pinned WDK kit at `C:\build\wdk`
  (`tools/build-driver.ps1 -Restore` downloads it).
- A test signing certificate with its private key in `LocalMachine\My`, trusted as a root and a
  trusted publisher. **Test certificates expire**; `doctor` warns 14 days ahead and runs refuse below 3.
- A signed baseline package (INF, SYS, CAT) and one adapter already installed from it. `init` copies the
  package to `C:\ci\baseline` and records the existing ChmLink devices as the expected ones; anything
  else is treated as leftover. Only adapters owned by the test suites' GUIDs are ever removed.

State lives in `C:\ci`: `config.json`, `baseline\`, `kit\` (the tools `restore` needs), `runs\<id>\`
(newest five kept).

### The DriverVer trap

Windows keys a published driver package on its INF. If the binary changes but `DriverVer` does not, it
reuses the old package and the new `.sys` is **not installed**. Every build also differs in its link
timestamp, so a leftover package of the same version beats the new binary. CI therefore:

- fails when driver code changed without a higher `DriverVer` (`check_driver_version.py`, cloud layer;
  comment-only edits to C sources do not count, an edit to the INF always does);
- removes leftover packages of the candidate's version before installing, and refuses a candidate whose
  version equals the baseline's.

When you change driver code, raise `DriverVer` in `driver/chmlink.inf` and `CHMLINK_VERSION_*` in
`include/chmlink/version.h` (`version_test` keeps the two equal).

## Benchmark gate

`full` runs the benchmark three times and gates each metric on its median against
`bench-baseline.json`, which is keyed by CPU model and logical processor count (a different machine needs
its own entry). Throughput may drop 15%, p50 latency rise 25%, p99 40%; packet loss may exceed the
baseline by 0.5 points (receive loss: 0.1); errors must be zero. Measured run-to-run spread on the VM is
at most about 14% for throughput, so these are deliberately not tighter. If loopback UDP is more than 25%
off its baseline the report warns that the machine itself is unusual; re-run before trusting a failure.

The TCP benchmark (`chmlink-tcp-bench`) runs through the Windows stack on an IP adapter, then on an
Ethernet adapter without and with task offloads (`tcp_eth_*_nooffload`, `tcp_eth_*`); IP mode has no
offloads to compare. Goodput (`tcp_mbps`) may drop 20%; the average adapter packet size is reported. With
offloads it also segments a sample of the stack's GSO packets and checks every segment independently
(IPv4 and TCP checksums, sequence numbers, data); any failure counts as an error. `offload-state.txt` in
each `bench-N` directory records what Windows reports for the Ethernet adapter's offloads in each half.
A full ring when injecting is back-pressure, reported as `rx_inject_full_per_s`, not as an error.

`vm-run.sh` takes the lane from the fetched `summary.json`, so `attach` to a `full` run also applies the
gate (it used to guess `smoke` while the run was in progress and skip it).

After an intended performance change, re-record in the same commit:

```sh
python3 tools/ci/compare_bench.py build/ci-results/<run-id>/bench-*/bench.jsonl \
    --env build/ci-results/<run-id>/bench-1/environment.json \
    --update-baseline --commit "$(git rev-parse --short HEAD)" --driver <DriverVer>
```

## Static analysis

`tools/build-driver.ps1 -Analyze` compiles every driver source with PREfast on top of `/W4 /WX`. Header
directories are external so only our code is reported. The WDK's driver rule sets are **not** used: on this
toolchain the engine rejects them (C6800) and no C28xxx driver rules load either way. A deliberate
suppression in `driver/adapter.c` (C28197, reference-counted release) is documented where it sits.

## Files

| file | purpose |
|---|---|
| `vm-run.sh` | entry point; pushes, launches, follows, fetches, applies gates |
| `vm-ci.ps1` | everything that runs on the VM (`Init Doctor Launch Run Poll Abort Restore Clean RenewCert Resume`) |
| `compare_bench.py` | benchmark gate and baseline recording |
| `check_driver_version.py` | the DriverVer rule |
| `bench-baseline.json` | recorded baseline |
| `test_ci.py` | tests for the two Python tools and a lint against `$PSScriptRoot` in param defaults |

## Pitfalls found while building this

- **`$PSScriptRoot` in a `param()` default is empty** in Windows PowerShell 5.1 for a `[CmdletBinding()]`
  script started with `-File`, so `"$PSScriptRoot/../build"` becomes `/../build`, the root of the current
  drive. Defaults are computed in the script body instead; `test_ci.py` enforces it.
- PowerShell 5.1 reads files in the system code page unless told otherwise. JSON written by these scripts
  is UTF-8 and read as UTF-8; `compare_bench.py` also accepts an older GBK `environment.json`.
- **A run lock records the boot.** After a reboot the pid in the lock file can name an unrelated process;
  a lock from an earlier boot is stale.
- `Start-Process -PassThru` in PowerShell 5.1 reports no `ExitCode` unless `.Handle` was read while the
  process ran.
- **PowerShell unwinds on shutdown.** Windows signals processes at shutdown and `catch`/`finally` blocks then
  run; a run that cleaned up there undid its own verifier handover. After a requested reboot those blocks
  only wait to be stopped.
- `GetNewClosure()` gives the block its own script scope: `$script:` variables and helper functions are not
  the caller's. The suite runner is a plain script block.
- BSD `sed` (macOS) does not understand `\r`; `vm-run.sh` strips carriage returns with `tr`.
