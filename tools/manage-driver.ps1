[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Upgrade', 'Rollback', 'RemovePackage')]
    [string]$Action = 'Plan',
    [string]$InfPath,
    [string]$RecoveryDirectory,
    [string]$JournalPath,
    [string]$PublishedInf,
    [string]$DeviceTool,
    [switch]$MaintenanceWindow,
    [switch]$AllowDowngrade
)
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $DeviceTool) { $DeviceTool = Join-Path $PSScriptRoot '../build/Release/chmlink-device.exe' }
$ErrorActionPreference = 'Stop'

# Exit codes: 0 completed, 1 failed before device changes, 2 upgrade failed but
# recovery was applied (inspect RestartRequired), 3 recovery incomplete or
# deferred, 3010 requested operation applied with restart required.
# No action restarts Windows or deletes a recovery directory.
function Invoke-DeviceTool([string[]]$Arguments) {
    if ($Arguments[0] -eq 'install') { $script:deviceChangesStarted = $true }
    & $DeviceTool @Arguments | Out-Host
    $code = $LASTEXITCODE
    if ($code -notin @(0, 3010)) { throw "Device operation failed (exit $code)." }
    if ($code -eq 3010) { $script:restartRequired = $true }
}

function Get-Property($Device, [string]$Key) {
    (Get-PnpDeviceProperty -InstanceId $Device.InstanceId -KeyName $Key -ErrorAction Stop).Data
}

function Get-Inventory {
    $result = @()
    foreach ($device in @(Get-PnpDevice -Class Net -ErrorAction Stop)) {
        $ids = @(Get-Property $device 'DEVPKEY_Device_HardwareIds')
        if ($ids -inotcontains 'Root\ChmLink' -and $ids -inotcontains 'Root\ChmLinkIP') { continue }
        if ((Get-Property $device 'DEVPKEY_Device_Service') -ine 'chmlink') {
            throw "Unexpected service on $($device.InstanceId)."
        }
        $inf = [string](Get-Property $device 'DEVPKEY_Device_DriverInfPath')
        if ($inf -notmatch '^oem[0-9]+\.inf$') { throw "Unrecognized installed INF: $inf" }
        $adapter = @(Get-CimInstance Win32_NetworkAdapter -Filter "PNPDeviceID='$($device.InstanceId.Replace('\', '\\'))'" -ErrorAction Stop)
        $index = 0; $guid = $null
        if ($adapter.Count -eq 1) { $index = [uint32]$adapter[0].InterfaceIndex; $guid = $adapter[0].GUID }
        $configuration = & $DeviceTool show $device.InstanceId
        if ($LASTEXITCODE -ne 0) { throw "Missing or incompatible identity on $($device.InstanceId); back up and recreate old devices." }
        $configuration = $configuration | ConvertFrom-Json
        if ((!$configuration.Managed -or $configuration.Version -ne 2) -and $Action -ne 'Plan') {
            throw 'Configuration version 2 is required; recreate old experimental devices after backing up their settings.'
        }
        $result += [pscustomobject]@{
            InstanceId = $device.InstanceId; PublishedInf = $inf
            Version = [string](Get-Property $device 'DEVPKEY_Device_DriverVersion')
            InterfaceIndex = $index; InterfaceGuid = $guid; Status = [string]$device.Status
            InterfaceName = if ($adapter.Count -eq 1) { $adapter[0].NetConnectionID } else { $null }
            Configuration = $configuration
            Addresses = @(); Routes = @(); Dns = @()
        }
        if ($index) {
            $result[-1].Addresses = @(Get-NetIPAddress -InterfaceIndex $index -ErrorAction SilentlyContinue |
                Select-Object IPAddress, AddressFamily, PrefixLength, PrefixOrigin, SuffixOrigin, SkipAsSource)
            $result[-1].Routes = @(Get-NetRoute -InterfaceIndex $index -ErrorAction SilentlyContinue |
                Select-Object DestinationPrefix, NextHop, RouteMetric, Protocol, PolicyStore)
            $result[-1].Dns = @(Get-DnsClientServerAddress -InterfaceIndex $index -ErrorAction SilentlyContinue |
                Select-Object AddressFamily, ServerAddresses)
        }
    }
    return $result
}

function Assert-Idle($Devices) {
    foreach ($device in @($Devices)) {
        if ($device.InterfaceIndex) {
            Invoke-DeviceTool -Arguments @('check-idle', [string]$device.InterfaceIndex)
        } elseif ($device.Status -eq 'OK') {
            throw "Cannot establish interface identity for $($device.InstanceId)."
        }
    }
}

function Get-PackageVersion([string]$Path) {
    $section = ''; $version = $null; $netClass = $false
    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($line in ($text -split '\r?\n')) {
        $clean = ($line -split ';', 2)[0].Trim()
        if ($clean -match '^\[([^\]]+)\]$') { $section = $matches[1]; continue }
        if ($section -ieq 'Version') {
            if ($clean -match '^Class\s*=\s*Net$') { $netClass = $true }
            if ($clean -match '^DriverVer\s*=\s*[^,]+,\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)$') {
                if ($null -ne $version) { throw 'Duplicate DriverVer.' }
                $version = [version]$matches[1]
            }
        }
    }
    if (!$netClass -or $null -eq $version -or
        $text -notmatch '(?im)^\s*[^;=\r\n]+\s*=\s*Install(?:Legacy|Store)\s*,\s*Root\\ChmLink\s*$' -or
        $text -notmatch '(?im)^\s*AddService\s*=\s*chmlink\s*,') {
        throw 'The INF is not a recognized ChmLink package.'
    }
    return $version
}

function Assert-TargetModes([string]$Path, $Devices) {
    if (@($Devices | Where-Object { $_.Configuration.Mode -eq 2 }).Count) {
        $targetInf = Get-Content -LiteralPath $Path -Raw
        foreach ($install in @('InstallIpLegacy', 'InstallIpStore')) {
            if ($targetInf -notmatch "(?im)^\s*[^;=\r\n]+\s*=\s*$install\s*,\s*Root\\ChmLinkIP\s*`$") {
                throw 'Target package does not support existing IP devices on both OS paths.'
            }
        }
    }
}

function Save-Journal {
    $temporary = "$script:journal.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($script:record | ConvertTo-Json -Depth 16))
        $stream = [IO.FileStream]::new($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if (Test-Path -LiteralPath $script:journal) {
            # PowerShell 5.1 binds $null to an empty string for this overload.
            [IO.File]::Replace($temporary, $script:journal, [NullString]::Value)
        }
        else { [IO.File]::Move($temporary, $script:journal) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Get-Hashes([string]$Directory) {
    $root = (Resolve-Path -LiteralPath $Directory).Path
    @(Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        [pscustomobject]@{ Relative = $_.FullName.Substring($root.Length + 1)
            Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}

function Export-Package([string]$Name, [string]$Destination) {
    if ($Name -notmatch '^oem[0-9]+\.inf$') { throw 'Invalid published INF.' }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    & "$env:SystemRoot/System32/pnputil.exe" /export-driver $Name $Destination | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Export failed for $Name." }
    $files = @(Get-ChildItem -LiteralPath $Destination -Recurse -Filter chmlink.inf -File)
    if ($files.Count -ne 1) { throw 'Export did not contain exactly one ChmLink package.' }
    return $files[0].Directory.FullName
}

function Get-BootTime {
    (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime().ToString('o')
}

function Assert-NoPendingReboot {
    if ((Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') -or
        (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') -or
        (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations) {
        throw 'Windows has pending reboot operations; finish them before package maintenance.'
    }
}

function Assert-InstalledFiles([string]$InstanceId, $Manifest) {
    $device = @(Get-Inventory | Where-Object InstanceId -eq $InstanceId)
    if ($device.Count -ne 1) { throw 'Installed device is no longer identifiable.' }
    $destination = Join-Path (Split-Path $script:journal) ('verify-' + [guid]::NewGuid().ToString('N'))
    $installed = Export-Package $device[0].PublishedInf $destination
    foreach ($file in @($Manifest)) {
        if ((Get-FileHash -LiteralPath (Join-Path $installed $file.Relative) -Algorithm SHA256).Hash -ne $file.Hash) {
            throw 'Installed package differs from the recorded payload.'
        }
    }
    # A legacy DIRID 12 package can be bound while a different binary remains
    # in System32\drivers. Check the service's on-disk image as well as the store.
    # Neither check proves which binary is currently loaded by the kernel.
    $binary = @($Manifest | Where-Object Relative -eq 'chmlink.sys')
    if ($binary.Count -ne 1) { throw 'Missing driver binary hash.' }
    $image = [Environment]::ExpandEnvironmentVariables(
        [string](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\chmlink' -Name ImagePath).ImagePath).Trim('"')
    if ($image.StartsWith('\??\')) { $image = $image.Substring(4) }
    if ($image.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $image = Join-Path $env:SystemRoot $image.Substring(12)
    } elseif (![IO.Path]::IsPathRooted($image)) { $image = Join-Path $env:SystemRoot $image }
    if ([IO.Path]::GetFileName($image) -ine 'chmlink.sys' -or
        (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash -ne $binary[0].Hash) {
        throw 'The configured service binary differs from the recorded package; runtime recovery is not established.'
    }
}

function Assert-Backup($Package) {
    $root = [IO.Path]::GetFullPath((Join-Path (Split-Path $script:journal) $Package.Directory))
    $allowed = [IO.Path]::GetFullPath((Split-Path $script:journal)).TrimEnd('\') + '\'
    if (!$root.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid recovery path.' }
    if (@($Package.Files).Count -lt 3) { throw 'Incomplete recovery file manifest.' }
    foreach ($required in @('chmlink.inf', 'chmlink.sys', 'chmlink.cat')) {
        if (@($Package.Files | Where-Object Relative -eq $required).Count -ne 1) {
            throw "Missing recovery file $required."
        }
    }
    foreach ($file in @($Package.Files)) {
        $path = [IO.Path]::GetFullPath((Join-Path $root $file.Relative))
        if (!$path.StartsWith(($root.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.Hash) {
            throw 'Recovery file hash or path does not match the journal.'
        }
    }
    $inf = Join-Path $root 'chmlink.inf'
    $null = Get-PackageVersion $inf
    return $inf
}

function Restore-Bindings {
    $script:rollbackStarted = $true
    $script:record.Status = 'RollingBack'; Save-Journal
    $failed = $false
    $steps = @($script:record.Steps)
    [array]::Reverse($steps)
    foreach ($step in $steps) {
        if ($step.State -eq 'Restored') { continue }
        try {
            $packages = @($script:record.Backups | Where-Object PublishedInf -eq $step.OldInf)
            if ($packages.Count -ne 1) { throw 'Missing or ambiguous recovery package.' }
            $old = Assert-Backup $packages[0]
            if ($step.State -eq 'RestorePendingReboot') {
                Assert-InstalledFiles $step.InstanceId $packages[0].Files
                $step.State = 'Restored'; $step.Error = $null; Save-Journal
                continue
            }
            $step.State = 'Restoring'; Save-Journal
            # C helper rechecks hardware ID and installs only on this exact instance.
            Invoke-DeviceTool -Arguments @('install', $step.InstanceId, $old)
            if ($script:restartRequired) { $step.State = 'RestorePendingReboot' }
            else { Assert-InstalledFiles $step.InstanceId $packages[0].Files; $step.State = 'Restored' }
            $step.Error = $null
        } catch { $step.State = 'RestoreFailed'; $step.Error = $_.Exception.Message; $failed = $true }
        $script:record.RestartRequired = $script:restartRequired; Save-Journal
    }
    if (!$failed) {
        try {
            Assert-Inventory @(Get-Inventory)
            if (!$script:restartRequired) {
                foreach ($step in $steps) {
                    $package = @($script:record.Backups | Where-Object PublishedInf -eq $step.OldInf)
                    Assert-InstalledFiles $step.InstanceId $package[0].Files
                }
            }
        } catch { $script:record.Error = $_.Exception.Message; $failed = $true }
    }
    $script:record.Status = if ($failed) { 'RollbackIncomplete' }
        elseif ($script:restartRequired) { 'RollbackPendingReboot' } else { 'RolledBack' }
    Save-Journal
    if ($script:restartRequired) { Write-Warning 'Binding recovery requires a Windows restart; runtime recovery is not yet verified.' }
    return !$failed
}

function Assert-Inventory($Current) {
    $expected = @($script:record.Devices.InstanceId | Sort-Object)
    if (@(Compare-Object $expected @($Current.InstanceId | Sort-Object)).Count) {
        throw 'Device inventory changed; manual recovery is required.'
    }
    foreach ($device in $Current) {
        $original = @($script:record.Devices | Where-Object InstanceId -eq $device.InstanceId)[0]
        if ($original.InterfaceGuid -and $device.InterfaceGuid -ine $original.InterfaceGuid) {
            throw "Interface identity changed for $($device.InstanceId)."
        }
        if ($original.InterfaceName -and $device.InterfaceName -cne $original.InterfaceName) {
            throw "Connection name changed for $($device.InstanceId)."
        }
        foreach ($field in @('Version', 'AdapterId', 'OwnerId', 'Mac', 'Name')) {
            if ($device.Configuration.$field -cne $original.Configuration.$field) {
                throw "Persistent configuration changed for $($device.InstanceId)."
            }
        }
        if ($device.Configuration.Mode -ne $original.Configuration.Mode) { throw "Adapter mode changed for $($device.InstanceId)." }
        if ($device.PublishedInf -ieq $original.PublishedInf) { continue }
        $step = @($script:record.Steps | Where-Object InstanceId -eq $device.InstanceId)
        if ($step.Count -ne 1) { throw 'An untouched device changed package.' }
        $allowedHashes = @($script:record.TargetFiles | Where-Object Relative -eq 'chmlink.inf')
        $installedPath = Join-Path "$env:SystemRoot/INF" $device.PublishedInf
        if ($step[0].State -in @('Restoring', 'Restored', 'RestorePendingReboot', 'RestoreFailed')) {
            $backup = @($script:record.Backups | Where-Object PublishedInf -eq $original.PublishedInf)
            if ($backup.Count -ne 1) { throw 'Missing restored package manifest.' }
            $oldHash = @($backup[0].Files | Where-Object Relative -eq 'chmlink.inf')
            if ($oldHash.Count -ne 1) { throw 'Missing recovery INF hash.' }
            $allowedHashes += $oldHash
        }
        if (!$allowedHashes.Count -or
            $allowedHashes.Hash -notcontains (Get-FileHash -LiteralPath $installedPath -Algorithm SHA256).Hash) {
            throw 'Device binding no longer matches this transaction; refusing to overwrite a later change.'
        }
    }
}

$restartRequired = $false
$deviceChangesStarted = $false; $rollbackStarted = $false
$mutex = $null; $locked = $false; $record = $null
$configMutex = $null; $configLocked = $false
try {
    $DeviceTool = (Resolve-Path -LiteralPath $DeviceTool).Path
    $toolVersion = & $DeviceTool version
    if ($LASTEXITCODE -ne 0 -or ($toolVersion -join '').Trim() -ne '4') { throw 'Device tool version 4 is required.' }
    if ($Action -eq 'Plan') {
        $inventory = @(Get-Inventory)
        $target = if ($InfPath) { [string](Get-PackageVersion (Resolve-Path -LiteralPath $InfPath).Path) } else { $null }
        [pscustomobject]@{ TargetVersion = $target; Devices = $inventory
            Note = 'Stop all clients before maintenance. Idle checks do not lock out noncooperating processes.' } |
            ConvertTo-Json -Depth 12
        exit 0
    }
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'An elevated PowerShell is required.' }
    if (!$MaintenanceWindow) { throw 'Specify -MaintenanceWindow after stopping all ChmLink clients.' }
    $mutex = [Threading.Mutex]::new($false, 'Global\ChmLink.PackageMaintenance')
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] {
        $locked = $true; throw 'Previous maintenance was interrupted; inspect its recovery journal first.'
    }
    if (!$locked) { throw 'Another package maintenance operation is running.' }
    $configMutex = [Threading.Mutex]::new($false, 'Global\ChmLink.AdapterConfiguration')
    try { $configLocked = $configMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] {
        $configLocked = $true; throw 'Adapter configuration was interrupted; inspect the device inventory.'
    }
    if (!$configLocked) { throw 'Adapter configuration is busy.' }
    if ($Action -eq 'RemovePackage') {
        Assert-NoPendingReboot
        if ($PublishedInf -notmatch '^oem[0-9]+\.inf$') { throw 'Specify -PublishedInf oemNN.inf.' }
        $boundDevices = @(Get-Inventory | Where-Object PublishedInf -eq $PublishedInf)
        if ($boundDevices.Count) { throw 'Remove associated devices before deleting this package.' }
        Invoke-DeviceTool -Arguments @('package-remove', $PublishedInf)
        exit 0
    }
    if ($Action -eq 'Rollback') {
        $journal = (Resolve-Path -LiteralPath $JournalPath).Path
        $record = Get-Content -LiteralPath $journal -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($record.Schema -ne 3 -or $record.ComputerName -ine $env:COMPUTERNAME -or
            $record.MachineId -ne (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Cryptography' -Name MachineGuid).MachineGuid -or
            $record.Status -notin @('Installing', 'UpgradeFailed', 'Upgraded', 'UpgradePendingReboot', 'RecoveryPendingReboot', 'RollingBack', 'RollbackIncomplete', 'RollbackPendingReboot')) {
            throw 'Journal schema, computer or recovery state is invalid.'
        }
        $current = @(Get-Inventory)
        Assert-Inventory $current
        Assert-Idle $current
        foreach ($package in @($record.Backups)) { $null = Assert-Backup $package }
        if ($record.RestartRequired -and (Get-BootTime) -eq $record.OperationBootUtc) {
            throw 'Restart is required before attempting binding recovery; keep the journal and packages.'
        }
        Assert-NoPendingReboot
        $record.OperationBootUtc = Get-BootTime
        $record.RestartRequired = $false
        $restartRequired = $false
        if (Restore-Bindings) { if ($restartRequired) { exit 3010 }; exit 0 }
        exit 3
    }
    if (!$InfPath -or !$RecoveryDirectory) { throw 'Upgrade requires -InfPath and a new -RecoveryDirectory.' }
    $source = (Resolve-Path -LiteralPath $InfPath).Path
    if ([IO.Path]::GetFileName($source) -ine 'chmlink.inf') { throw 'Expected chmlink.inf.' }
    $version = Get-PackageVersion $source
    if ($version -lt [version]'0.3.0.0') { throw 'Target package predates configuration version 2.' }
    Assert-NoPendingReboot
    $devices = @(Get-Inventory)
    if (!$devices.Count) { throw 'No installed ChmLink devices; use create instead.' }
    Assert-TargetModes $source $devices
    if (@($devices.PublishedInf | Sort-Object -Unique).Count -ne 1) {
        throw 'Instances have inconsistent package bindings; reconcile them before a shared-service upgrade.'
    }
    foreach ($device in $devices) {
        if ([version]$device.Version -lt [version]'0.3.0.0') { throw 'Installed package predates configuration version 2.' }
        if (!$AllowDowngrade -and $version -le [version]$device.Version) {
            throw 'Target must be newer than every installed version; use -AllowDowngrade for explicit repair/downgrade.'
        }
    }
    Assert-Idle $devices
    if (Test-Path -LiteralPath $RecoveryDirectory) { throw 'RecoveryDirectory must not already exist.' }
    $directory = New-Item -ItemType Directory -Path $RecoveryDirectory
    $journal = Join-Path $directory.FullName 'journal.json'
    $record = [pscustomobject]@{ Schema = 3; ComputerName = $env:COMPUTERNAME; Status = 'BackingUp'
        MachineId = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Cryptography' -Name MachineGuid).MachineGuid
        OperationBootUtc = (Get-BootTime)
        TargetVersion = [string]$version; Devices = $devices; Backups = @(); Steps = @()
        RestartRequired = $false; Error = $null; CreatedUtc = [DateTime]::UtcNow.ToString('o') }
    Save-Journal
    foreach ($old in @($devices.PublishedInf | Sort-Object -Unique)) {
        $relative = "backup-$old"; $destination = Join-Path $directory.FullName $relative
        $exportedRoot = Export-Package $old $destination
        $relativeRoot = $exportedRoot.Substring($directory.FullName.Length + 1)
        $record.Backups += [pscustomobject]@{ PublishedInf = $old; Directory = $relativeRoot
            Files = @(Get-Hashes $exportedRoot) }
        Save-Journal
    }
    $payload = Join-Path $directory.FullName 'target'
    New-Item -ItemType Directory -Path $payload | Out-Null
    foreach ($name in @('chmlink.inf', 'chmlink.sys', 'chmlink.cat')) {
        Copy-Item -LiteralPath (Join-Path (Split-Path $source) $name) -Destination $payload
    }
    $record | Add-Member -NotePropertyName TargetFiles -NotePropertyValue @(Get-Hashes $payload)
    if ((Get-PackageVersion (Join-Path $payload 'chmlink.inf')) -ne $version) { throw 'Target package changed while copying.' }
    Assert-TargetModes (Join-Path $payload 'chmlink.inf') $devices
    foreach ($backup in @($record.Backups)) { $null = Assert-Backup $backup }
    $current = @(Get-Inventory)
    Assert-Inventory $current
    Assert-Idle $current
    Assert-NoPendingReboot
    $record.Status = 'Installing'; Save-Journal
    foreach ($device in $devices) {
        $record.Steps += [pscustomobject]@{ InstanceId = $device.InstanceId; OldInf = $device.PublishedInf
            State = 'Attempting'; Error = $null }
        Save-Journal # durable before calling into the installer, including the failing instance
        Invoke-DeviceTool -Arguments @('install', $device.InstanceId, (Join-Path $payload 'chmlink.inf'))
        $record.Steps[-1].State = 'Installed'; $record.RestartRequired = $restartRequired; Save-Journal
        if (!$restartRequired) { Assert-InstalledFiles $device.InstanceId $record.TargetFiles }
    }
    Assert-Inventory @(Get-Inventory)
    $record.Status = if ($restartRequired) { 'UpgradePendingReboot' } else { 'Upgraded' }; Save-Journal
    Write-Output "Upgrade installation finished. State: $($record.Status). Recovery journal: $journal"
    if ($restartRequired) { exit 3010 }
    exit 0
} catch {
    Write-Warning $_.Exception.Message
    if ($Action -eq 'Upgrade' -and $null -ne $record -and $deviceChangesStarted -and !$rollbackStarted) {
        $record.Error = $_.Exception.Message; $record.Status = 'UpgradeFailed'
        try {
            $record.RestartRequired = $restartRequired
            if ($restartRequired) {
                $record.Status = 'RecoveryPendingReboot'; Save-Journal
                Write-Warning "Recovery requires a restart before rollback. Journal: $journal"
                exit 3
            }
            Save-Journal
            Assert-NoPendingReboot
            $current = @(Get-Inventory)
            Assert-Inventory $current
            Assert-Idle $current
            if (Restore-Bindings) { exit 2 }
        } catch {
            Write-Warning "Recovery interrupted: $_"
            $record.Status = 'RollbackIncomplete'
            try { Save-Journal } catch { Write-Warning 'Could not persist final recovery state; retain all files.' }
        }
        exit 3
    }
    if ($deviceChangesStarted -or $rollbackStarted) { exit 3 }
    exit 1
} finally {
    if ($configLocked) { $configMutex.ReleaseMutex() }
    if ($null -ne $configMutex) { $configMutex.Dispose() }
    if ($locked) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
}
