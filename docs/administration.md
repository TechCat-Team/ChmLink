# Administration

**English** | [简体中文](zh-CN/administration.md)

Managing adapters and driver packages without writing code: for scripts,
operations and recovery. Applications normally use the SDK instead
([sdk.md](sdk.md)). Everything here needs an elevated prompt.

## chmlink-device

`chmlink-device.exe` is the SDK's adapter management as a command line. It is
linked statically, so it runs on its own. Unlike the SDK it always takes an
explicit INF, and it never changes the driver of other adapters.

```text
chmlink-device create <signed-inf> [--owner <GUID>] [--name <name>] [--mac <MAC>] [--mode ethernet|ip] [--mtu <1280-65535>]
chmlink-device set-mtu <instance-id> <mtu> [--owner <GUID>]      restarts the adapter
chmlink-device show <instance-id>
chmlink-device remove <instance-id> [--owner <GUID>]
chmlink-device install <instance-id> <signed-inf>                 install a package on one adapter
chmlink-device package-remove <oemNN.inf>                         remove an unused package
chmlink-device check-idle <interface-index>                       is no session open?
chmlink-device version
```

`create`, `set-mtu` and `show` print the adapter's configuration as JSON
(`InstanceId`, `AdapterId`, `OwnerId`, `Mode`, `Mac`, `Mtu`, `Name`,
`Version`). Exit codes: 0 done, 1 refused or failed, 2 usage, 3 partly applied
(inspect the adapter), 3010 a restart is required.

Creation checks that the INF, SYS and CAT are identical to the package the
existing adapters run; upgrade those first if they differ. Names default to
`ChmLink-<adapter GUID>`; Ethernet MACs are random and locally administered
unless `--mac` is given. Configuration refuses multicast and globally
administered MACs, empty GUIDs, and names, MACs or identities already in use
on this machine.

## manage-adapter.ps1

A PowerShell front end for the same operations, with retries for naming:

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath C:\pkg\chmlink.inf -Mode ip -Name 'Office VPN' -Mtu 9000 -OwnerId <GUID>
.\tools\manage-adapter.ps1 -Action SetMtu -InstanceId 'ROOT\NET\0001' -Mtu 1500 -OwnerId <GUID>
.\tools\manage-adapter.ps1 -Action ApplyName -InstanceId 'ROOT\NET\0001' -OwnerId <GUID>
```

`ApplyName` sets the stored name on the exact Windows interface again, for an
adapter whose creation reported that naming was incomplete. It never removes an
adapter to undo a naming failure. Exit codes as for `chmlink-device`.

**Owners.** Applications should use their own fixed owner GUID; the default is
meant for standalone provisioning. Removal and MTU changes require the matching
owner. Ownership prevents mistakes; it is not a security boundary against
administrators.

## Upgrading the driver: manage-driver.ps1

All adapters share one driver package. Applications using the SDK upgrade it
themselves when they bring a newer one (see
[sdk.md](sdk.md#which-driver-is-installed)). For controlled maintenance,
`tools/manage-driver.ps1` upgrades with a recovery journal and can roll back:

```powershell
# read-only: every ChmLink adapter, its package, identity and network settings
.\tools\manage-driver.ps1 -Action Plan -InfPath C:\pkg\next\chmlink.inf

.\tools\manage-driver.ps1 -Action Upgrade -InfPath C:\pkg\next\chmlink.inf `
    -RecoveryDirectory C:\ChmLinkRecovery\upgrade-001 -MaintenanceWindow

.\tools\manage-driver.ps1 -Action Rollback `
    -JournalPath C:\ChmLinkRecovery\upgrade-001\journal.json -MaintenanceWindow

.\tools\manage-driver.ps1 -Action RemovePackage -PublishedInf oem42.inf -MaintenanceWindow
```

Stop every ChmLink client first; `-MaintenanceWindow` confirms that. The
script checks that no session is open, but cannot stop a client from opening
one later.

**Upgrade** requires all adapters on one package. It exports the old package,
copies and hashes the new INF, SYS and CAT, writes a recovery journal, then
installs on each adapter in turn. The journal records the machine, interface
GUIDs, configurations, bindings, each step and restart requirements; network
settings are recorded for diagnosis only and not reapplied. Use a new recovery
directory on a local NTFS volume that only administrators can change, and
keep it until you no longer need to roll back. The target version must be
higher unless `-AllowDowngrade` asks for a repair or downgrade, and a package
older than 0.4.2.0 is refused while an adapter has a version 3 configuration.

**On failure** the script restores the old bindings in reverse order, including
the adapter whose installation failed, and keeps the journal. If Windows asks
for a restart during an upgrade, recovery waits: restart, then run Rollback
with the same journal. Installation across several devices is not atomic;
recovery is best effort.

Both adapter kinds share the package, so an upgrade affects all of them. Use
the managers from the same release as the package: older ones do not know the
IP adapter's hardware ID and would miss those adapters. A downgrade to a
package without the IP model is refused.

**RemovePackage** accepts only a recognized ChmLink OEM INF and refuses
packages still in use. The script never restarts Windows, removes adapters,
deletes recovery files or forces package deletion.

| exit code | meaning |
|---|---|
| 0 | done, no restart required |
| 1 | refused or failed before any adapter changed |
| 2 | the upgrade failed and recovery was applied; see the journal |
| 3 | recovery incomplete or deferred; keep the whole recovery directory |
| 3010 | done; a restart is required |

## Inspecting

```powershell
.\build\Release\chmlink-list.exe                 # adapters, state, counters (no session)
.\build\Release\chmlink-device.exe show ROOT\NET\0001
Get-PnpDevice -Class Net | Where-Object { $_.HardwareID -match '^Root\\ChmLink' }
```
