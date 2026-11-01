# 管理

[English](../administration.md) | **简体中文**

不写代码管理网卡和驱动包：适用于脚本、运维和故障恢复。应用通常直接使用 SDK（[sdk.md](sdk.md)）。
本文的所有操作都需要管理员权限的命令行。

## chmlink-device

`chmlink-device.exe` 以命令行形式提供 SDK 的网卡管理功能。它是静态链接的，可以单独运行。
与 SDK 不同，它总是需要显式指定 INF，并且从不改动其他网卡的驱动。

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

| 命令 | 作用 |
|---|---|
| `create` | 创建网卡 |
| `set-mtu` | 修改 MTU，网卡会重启 |
| `show` | 显示网卡配置 |
| `remove` | 删除网卡 |
| `install` | 在一块网卡上安装指定的驱动包 |
| `package-remove` | 删除一个未被使用的驱动包 |
| `check-idle` | 检查网卡上是否没有打开的会话 |
| `version` | 显示工具版本 |

`create`、`set-mtu` 和 `show` 以 JSON 格式输出网卡配置（`InstanceId`、`AdapterId`、`OwnerId`、`Mode`、`Mac`、
`Mtu`、`Name`、`Version`）。退出码：0 完成，1 被拒绝或失败，2 用法错误，3 只完成了一部分（请检查该网卡），
3010 需要重启。

创建时会检查 INF、SYS 和 CAT 是否与现有网卡使用的驱动包完全相同；如果不同，请先升级现有网卡。
名称默认为 `ChmLink-<网卡 GUID>`；除非指定 `--mac`，以太网 MAC 地址是随机的本地管理地址。
配置会拒绝组播地址和全局管理的 MAC 地址、空 GUID，以及本机上已被占用的名称、MAC 或身份。

## manage-adapter.ps1

同样操作的 PowerShell 前端，命名失败时会重试：

```powershell
.\tools\manage-adapter.ps1 -Action Create -InfPath C:\pkg\chmlink.inf -Mode ip -Name 'Office VPN' -Mtu 9000 -OwnerId <GUID>
.\tools\manage-adapter.ps1 -Action SetMtu -InstanceId 'ROOT\NET\0001' -Mtu 1500 -OwnerId <GUID>
.\tools\manage-adapter.ps1 -Action ApplyName -InstanceId 'ROOT\NET\0001' -OwnerId <GUID>
```

`ApplyName` 用于创建时报告命名未完成的网卡：它把保存的名称重新设置到对应的 Windows 接口上。
它绝不会为了撤销命名失败而删除网卡。退出码与 `chmlink-device` 相同。

**所有者。** 应用应使用自己固定的所有者 GUID；默认值供独立部署使用。删除网卡和修改 MTU 需要匹配的所有者。
所有者机制用于防止误操作，并不是针对管理员的安全边界。

## 升级驱动：manage-driver.ps1

所有网卡共用一个驱动包。使用 SDK 的应用在带来更新的驱动时会自行升级（见
[sdk.md](sdk.md#安装哪个驱动)）。需要受控维护时，`tools/manage-driver.ps1` 会在升级时记录恢复日志，并且可以回滚：

```powershell
# read-only: every ChmLink adapter, its package, identity and network settings
.\tools\manage-driver.ps1 -Action Plan -InfPath C:\pkg\next\chmlink.inf

.\tools\manage-driver.ps1 -Action Upgrade -InfPath C:\pkg\next\chmlink.inf `
    -RecoveryDirectory C:\ChmLinkRecovery\upgrade-001 -MaintenanceWindow

.\tools\manage-driver.ps1 -Action Rollback `
    -JournalPath C:\ChmLinkRecovery\upgrade-001\journal.json -MaintenanceWindow

.\tools\manage-driver.ps1 -Action RemovePackage -PublishedInf oem42.inf -MaintenanceWindow
```

`Plan` 只读，列出每块 ChmLink 网卡、它的驱动包、身份和网络设置。

请先停止所有 ChmLink 客户端；`-MaintenanceWindow` 表示你确认已经这样做。脚本会检查没有打开的会话，
但无法阻止客户端之后再打开。

**升级（Upgrade）** 要求所有网卡使用同一个驱动包。它会导出旧驱动包，复制新的 INF、SYS 和 CAT 并计算哈希，
写入恢复日志，然后逐块网卡安装。恢复日志记录机器、接口 GUID、配置、绑定、每个步骤和重启要求；
网络设置只用于诊断，不会重新应用。请使用一个新的恢复目录，放在只有管理员能修改的本地 NTFS 卷上，
在不再需要回滚之前保留它。目标版本必须更高，除非用 `-AllowDowngrade` 表示修复或降级；
如果有网卡使用版本 3 的配置，早于 0.4.2.0 的驱动包会被拒绝。

**失败时**，脚本按相反顺序恢复旧的绑定，包括安装失败的那块网卡，并保留恢复日志。如果升级过程中 Windows 要求重启，
恢复会推迟：重启后用同一份日志运行 Rollback。跨多个设备的安装不是原子操作；恢复是尽力而为的。

两种网卡共用驱动包，所以一次升级会影响所有网卡。请使用与驱动包同一版本的管理脚本：旧版本不认识 IP 网卡的硬件 ID，
会漏掉这些网卡。降级到不含 IP 网卡型号的驱动包会被拒绝。

**删除驱动包（RemovePackage）** 只接受能识别为 ChmLink 的 OEM INF，并拒绝仍在使用的驱动包。
脚本从不重启 Windows、删除网卡、删除恢复文件或强制删除驱动包。

| 退出码 | 含义 |
|---|---|
| 0 | 完成，不需要重启 |
| 1 | 在改动任何网卡之前被拒绝或失败 |
| 2 | 升级失败，已执行恢复；见恢复日志 |
| 3 | 恢复未完成或被推迟；请保留整个恢复目录 |
| 3010 | 完成；需要重启 |

## 查看状态

```powershell
.\build\Release\chmlink-list.exe                 # adapters, state, counters (no session)
.\build\Release\chmlink-device.exe show ROOT\NET\0001
Get-PnpDevice -Class Net | Where-Object { $_.HardwareID -match '^Root\\ChmLink' }
```

`chmlink-list.exe` 不打开会话，列出网卡、状态和计数器。
