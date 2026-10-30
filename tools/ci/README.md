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
tools/ci/vm-run.sh run smoke                 # HEAD: build, ctest, install, lifecycle, IP   (~70 s)
tools/ci/vm-run.sh run full                  # smoke + 3x benchmark gated on the baseline    (~6.5 min)
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
`build-user` → `ctest` → `build-driver` (with `-Analyze`) → `sign` → `clean-store` → `install` →
`lifecycle` → `ip` → [`bench-1..3`] → `restore`. `restore` runs whatever happened before it. If the
previous run was killed, the next one notices (wrong driver image, leftover adapters owned by the test
suites) and repairs it first.

Only one run at a time; a second is refused with the id of the one holding the VM.

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
| `vm-ci.ps1` | everything that runs on the VM (`Init Doctor Launch Run Poll Abort Restore Clean`) |
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
- BSD `sed` (macOS) does not understand `\r`; `vm-run.sh` strips carriage returns with `tr`.
