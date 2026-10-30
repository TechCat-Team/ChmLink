#requires -Version 5.1
<#
Runtime CI for the disposable Windows test VM. Normally driven from another
machine by tools/ci/vm-run.sh; see tools/ci/README.md.

A run builds the commit under test, signs it with the VM's test certificate,
installs it over the baseline driver, runs the lifecycle and IP suites (and
the benchmark for the full lane), then ALWAYS restores the baseline driver.
Runs detach from the SSH session and report through files, so a dropped
connection loses nothing.

Actions: Init, Doctor, Launch, Run, Poll, Abort, Restore, Clean.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Init', 'Doctor', 'Launch', 'Run', 'Poll', 'Abort', 'Restore', 'Clean', 'RenewCert', 'Resume')]
    [string]$Action,
    [string]$RunId,
    [ValidateSet('smoke', 'full', 'nightly')][string]$Lane = 'smoke',
    [int]$StressMinutes = 0,
    # Minutes under Driver Verifier fault injection in the nightly lane; -1 = config or 10, 0 = skip.
    [int]$LowResMinutes = -1,
    [int]$Offset = 0,
    [string]$BaselineFrom,
    [string]$CertThumbprint,
    [int]$ValidDays = 730,
    [switch]$WhatIfRenew
)
$ErrorActionPreference = 'Stop'
$ciRoot = 'C:\ci'
$configPath = Join-Path $ciRoot 'config.json'
$lockPath = Join-Path $ciRoot 'lock.json'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# ---------------------------------------------------------------- utilities
function Get-Config {
    if (!(Test-Path -LiteralPath $configPath)) { throw "Missing $configPath; run vm-run.sh init first." }
    $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($default in @{ KitVersion = '10.0.26100.0'; KeepRuns = 5; BenchRepeats = 3; BenchSeconds = 5
                            MinFreeGB = 5; CertWarnDays = 14; CertFailDays = 3 }.GetEnumerator()) {
        if ($null -eq $config.($default.Key)) { $config | Add-Member -NotePropertyName $default.Key -NotePropertyValue $default.Value }
    }
    return $config
}

function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), $utf8)
}

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

function Get-InstalledImageHash {
    $service = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\chmlink' -ErrorAction SilentlyContinue
    if (!$service) { return $null }
    $path = $service.ImagePath -replace '^\\SystemRoot', $env:SystemRoot
    if (!(Test-Path -LiteralPath $path)) { return $null }
    return Get-Sha256 $path
}

function Get-ChmLinkDevices {
    @(Get-PnpDevice -Class Net -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -match 'Root\\ChmLink' })
}

function Get-BoundInf([string]$InstanceId) {
    [string](Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction Stop).Data
}

# Driver packages with this INF name in the driver store, bound or not. Property names are
# language independent, unlike pnputil's output.
function Get-ChmLinkPackages {
    @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object { $_.OriginalFileName -match '\\chmlink\.inf$' } |
        ForEach-Object { [pscustomobject]@{ Inf = [string]$_.Driver; Version = [string]$_.Version } })
}

function Get-InfVersion([string]$InfPath) {
    $match = Select-String -LiteralPath $InfPath -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1
    if (!$match) { throw "No DriverVer in $InfPath" }
    return $match.Matches[0].Groups[1].Value
}

# ---------------------------------------------------------------- run state
$script:runDir = $null; $script:logPath = $null; $script:stages = @(); $script:deadline = $null
$script:srcDir = $null; $script:manageScript = $null; $script:needsRestore = $false
$script:installStarted = $false; $script:candidateOem = ''; $script:driverVersion = ''
$script:leakReference = 0
$kitDir = Join-Path $ciRoot 'kit'   # manage-driver.ps1 + chmlink-device.exe of the last good build, for Restore

function Write-Log([string]$Message) {
    $line = '[{0:HH:mm:ss}] {1}' -f (Get-Date), $Message
    if ($script:logPath) { [IO.File]::AppendAllText($script:logPath, $line + "`r`n", $utf8) } else { Write-Output $line }
}

function Set-Status([string]$State, [string]$Stage = '') {
    if (!$script:runDir) { return }
    Write-Json (Join-Path $script:runDir 'status.json') ([ordered]@{
        State = $State; Stage = $Stage; Pid = $PID; Updated = (Get-Date).ToUniversalTime().ToString('s') })
}

# Run one external program with a timeout, appending its output to the run log.
function Invoke-Process([string]$Name, [string]$FilePath, [string]$Arguments, [int]$TimeoutSec = 600,
                        [string]$WorkDir = $script:runDir) {
    if ($script:deadline) {
        $remaining = [int](($script:deadline - (Get-Date)).TotalSeconds)
        if ($remaining -le 0) { throw "Lane deadline exceeded before '$Name'." }
        if ($remaining -lt $TimeoutSec) { $TimeoutSec = $remaining }
    }
    $out = Join-Path $script:runDir ('proc-{0}.out' -f ([guid]::NewGuid().ToString('N').Substring(0, 8)))
    $err = $out + '.err'
    Write-Log "run: $Name"
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WorkingDirectory $WorkDir -NoNewWindow `
        -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $handle = $process.Handle   # keeps ExitCode readable after exit
    if (!$process.WaitForExit($TimeoutSec * 1000)) {
        & taskkill.exe /T /F /PID $process.Id *> $null
        $timedOut = $true
    }
    $process.WaitForExit()
    foreach ($file in @($out, $err)) {
        if (Test-Path -LiteralPath $file) {
            Get-Content -LiteralPath $file -Encoding Oem | Where-Object { $_ -ne '' } | ForEach-Object { Write-Log "  $_" }
            Remove-Item -LiteralPath $file -Force
        }
    }
    if ($timedOut) { throw "'$Name' timed out after $TimeoutSec s." }
    if ($process.ExitCode -ne 0) { throw "'$Name' exited with code $($process.ExitCode)." }
}

function Invoke-Stage([string]$Name, [scriptblock]$Body) {
    $started = Get-Date
    Set-Status 'running' $Name
    Write-Log "== stage: $Name"
    $record = [ordered]@{ Name = $Name; Status = 'passed'; Seconds = 0; Error = '' }
    try { & $Body }
    catch { $record.Status = 'failed'; $record.Error = $_.Exception.Message; Write-Log "stage '$Name' FAILED: $($record.Error)" }
    $record.Seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $script:stages += [pscustomobject]$record
    if ($record.Status -eq 'failed') { throw "stage '$Name' failed: $($record.Error)" }
}

# ------------------------------------------------------------------ checks
function Get-Preflight($Config, [bool]$Quiet) {
    $problems = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    if (!(Test-Admin)) { $problems.Add('not running elevated') }
    $drive = Get-PSDrive -Name C
    $free = [math]::Round($drive.Free / 1GB, 1)
    if ($free -lt $Config.MinFreeGB) { $problems.Add("only $free GB free on C: (need $($Config.MinFreeGB))") }
    foreach ($tool in 'cmake', 'ctest', 'tar') {
        if (!(Get-Command $tool -ErrorAction SilentlyContinue)) { $problems.Add("$tool not on PATH") }
    }
    foreach ($file in 'chmlink.inf', 'chmlink.sys', 'chmlink.cat') {
        if (!(Test-Path -LiteralPath (Join-Path $Config.Baseline $file))) { $problems.Add("baseline package incomplete: $file") }
    }
    $sdk = Join-Path $Config.WdkRoot "microsoft.windows.sdk.cpp\c\bin\$($Config.KitVersion)\x64\signtool.exe"
    if (!(Test-Path -LiteralPath $sdk)) { $problems.Add("signtool missing: $sdk") }
    $cert = Get-ChildItem Cert:\LocalMachine\My -ErrorAction SilentlyContinue | Where-Object Thumbprint -eq $Config.CertThumbprint
    if (!$cert) { $problems.Add('signing certificate not found') }
    elseif (!$cert.HasPrivateKey) { $problems.Add('signing certificate has no private key') }
    else {
        $days = [int]($cert.NotAfter - (Get-Date)).TotalDays
        if ($days -lt $Config.CertFailDays) { $problems.Add("signing certificate expires in $days day(s)") }
        elseif ($days -lt $Config.CertWarnDays) { $warnings.Add("signing certificate expires in $days days ($($cert.NotAfter.ToString('yyyy-MM-dd')))") }
    }
    $pending = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending',
                 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') |
        Where-Object { Test-Path $_ }
    if ($pending) { $problems.Add('a reboot is pending') }
    $memory = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    if ($memory.VerifyDrivers -or $memory.VerifyDriverLevel) { $warnings.Add('Driver Verifier is enabled (persistent); timings are not comparable') }
    if (Get-ScheduledTask -TaskName $resumeTask -ErrorAction SilentlyContinue) { $problems.Add('the nightly resume task is registered: a nightly run is in progress or was abandoned (vm-run.sh restore clears it)') }
    $expected = @($Config.ExpectedDevices)
    $actual = @(Get-ChmLinkDevices | ForEach-Object InstanceId)
    $extra = @($actual | Where-Object { $expected -notcontains $_ })
    $missing = @($expected | Where-Object { $actual -notcontains $_ })
    if ($extra) { $problems.Add("unexpected ChmLink devices: $($extra -join ', ')") }
    if ($missing) { $problems.Add("missing ChmLink devices: $($missing -join ', ')") }
    return [pscustomobject]@{ Problems = @($problems); Warnings = @($warnings); FreeGB = $free
                              ImageMatchesBaseline = ((Get-InstalledImageHash) -eq (Get-Sha256 (Join-Path $Config.Baseline 'chmlink.sys'))) }
}

# Owners of the adapters the suites create. Anything else (the anchor device, a developer's own
# adapters) is never removed by CI.
$testOwners = @('9AB3CDB8-3693-4C1F-89B3-4062BB57BE83',   # tools/test-lifecycle.ps1
                'A9206A62-5C94-4CBC-B23E-FB3A891DE058',   # tools/test-ip.ps1
                '3F6B2D7E-91A4-4C58-B0E2-7A1D5C9E4B36')   # tools/bench.ps1

# Remove adapters left behind by a suite that was killed. Returns the instance ids removed.
function Remove-LeftoverTestDevices([string]$DeviceTool, $Config) {
    $removed = @()
    foreach ($device in @(Get-ChmLinkDevices | Where-Object { @($Config.ExpectedDevices) -notcontains $_.InstanceId })) {
        $identity = & $DeviceTool show $device.InstanceId | ConvertFrom-Json
        if (!$identity.Managed -or $testOwners -notcontains ([guid]$identity.OwnerId).ToString().ToUpper()) {
            Write-Log "leaving $($device.InstanceId): not owned by a CI suite"
            continue
        }
        Write-Log "removing leftover test adapter $($device.InstanceId) ($($identity.Name))"
        & $DeviceTool remove $device.InstanceId --owner $identity.OwnerId | ForEach-Object { Write-Log "  $_" }
        if ($LASTEXITCODE -notin @(0, 3010)) { throw "Removing $($device.InstanceId) failed with code $LASTEXITCODE." }
        $removed += $device.InstanceId
    }
    return $removed
}

# ------------------------------------------------------------ Driver Verifier
# The nightly lane runs the suites with Driver Verifier's standard rule classes (special pool, IRQL
# checks, pool tracking, I/O verification, deadlock detection, DMA, security and miscellaneous
# checks, DDI compliance) on chmlink.sys, set persistently so that it applies from boot, which also
# covers DriverEntry, and keeps applying to every image Windows loads afterwards, including the ones
# an upgrade or rollback installs.
#
# Why not "verifier /dif ... /now", which needs no reboot? Measured on this VM: it can be used once
# per boot, cannot be switched off (/reset reports "No settings were changed") until the next
# reboot, and it stops tracking the driver after an upgrade reloads it. A lane that must test the
# upgrade and rollback paths under verification cannot work with that.
#
# The text of "verifier /query" is localized (the same call prints English from a script and
# Chinese from a bare ssh command), so Get-VerifierState reads structure, not labels.
function Get-VerifierState {
    $query = (& verifier.exe /query 2>&1 | Out-String) -split "`r?`n"
    $listed = $false; $inModule = $false; $counters = @()
    foreach ($line in $query) {
        if ($line -match ':\s*chmlink\.sys\s*\(') { $listed = $true; $inModule = $true; continue }
        if ($line -match '^\s*\S[^:]*:\s*\S+\.sys\s*\(') { $inModule = $false; continue }   # the next module
        # Counter lines read "<label>: ( nonpaged / paged )", in this order: current allocations,
        # current bytes, peak allocations, peak bytes.
        if ($inModule -and $line -match ':\s*\(\s*(\d+)\s*/\s*(\d+)\s*\)') { $counters += ([int64]$matches[1] + [int64]$matches[2]) }
    }
    $at = { param($n) if ($counters.Count -gt $n) { $counters[$n] } else { -1 } }
    [pscustomobject]@{ Listed = $listed; CurrentAllocations = (& $at 0); CurrentBytes = (& $at 1); PeakAllocations = (& $at 2) }
}

# The global "Verifier Statistics Summary" counters, in output order. The labels are localised, the
# order is not: Raise IRQLs, Acquire Spin Locks, Synchronize Executions, Trims, Pool Allocations
# Attempted, Succeeded, Succeeded SpecialPool, With No Tag, Not Tracked, Failed, Failed Deliberately.
function Get-VerifierStatistics {
    $values = @()
    foreach ($line in ((& verifier.exe /query 2>&1 | Out-String) -split "`r?`n")) {
        if ($line -match '\.sys\s*\(') { break }                       # the per-driver list starts
        if ($line -match '^\s+[^:(]+:\s+(\d+)\s*$') { $values += [int64]$matches[1] }
    }
    [pscustomobject]@{
        Raw = ($values -join ',')
        Attempted = $(if ($values.Count -ge 11) { $values[4] } else { -1 })
        FailedDeliberately = $(if ($values.Count -ge 11) { $values[10] } else { -1 })
    }
}

# The persistent setting, as the registry has it. This is what survives a reboot.
function Get-VerifierPersistent {
    $memory = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    [pscustomobject]@{ Drivers = [string]$memory.VerifyDrivers; Level = $memory.VerifyDriverLevel }
}

# Set for the next boot. Takes effect only after a reboot.
function Set-VerifierPersistent {
    & verifier.exe /standard /driver chmlink.sys 2>&1 | ForEach-Object { Write-Log "  verifier: $_" }
    if ((Get-VerifierPersistent).Drivers -notmatch 'chmlink\.sys') { throw 'Could not set the persistent Driver Verifier configuration.' }
}

# Standard checks plus randomized low resources simulation (0x4) for the next boot. Verifier
# fails a share of the driver's allocations (probability in 1/10000) from DelayMinutes after boot
# on. 'verifier /faults' and '/flags' each reset the other's values, so the fault parameters are
# written after the flags.
function Set-VerifierLowResources([int]$Probability = 600, [int]$DelayMinutes = 1) {
    & verifier.exe /flags 0x209BF /driver chmlink.sys 2>&1 | ForEach-Object { Write-Log "  verifier: $_" }
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    Set-ItemProperty -LiteralPath $key -Name VerifierFaultProbability -Value $Probability -Type DWord
    Set-ItemProperty -LiteralPath $key -Name VerifierFaultBootMinutes -Value $DelayMinutes -Type DWord
    $now = Get-VerifierPersistent
    if ($now.Drivers -notmatch 'chmlink\.sys' -or !($now.Level -band 4)) { throw 'Could not set the low resources Driver Verifier configuration.' }
}

# Clear for the next boot. The current boot keeps tracking until it reboots.
function Clear-VerifierPersistent {
    & verifier.exe /reset 2>&1 | Out-Null
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    Remove-ItemProperty -LiteralPath $key -Name VerifierFaultProbability, VerifierFaultBootMinutes -ErrorAction SilentlyContinue
    if ((Get-VerifierPersistent).Drivers) { throw 'Could not clear the persistent Driver Verifier configuration.' }
}

# ----------------------------------------------------- driver install state
function Install-Package($Config, [string]$InfPath, [string]$DeviceTool, [string]$Label, [string]$ExpectedSysHash) {
    $recovery = Join-Path $script:runDir ('recovery-' + $Label + '-' + (Get-Date -Format 'HHmmss'))
    Invoke-Process "install $Label" 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -Action Upgrade -InfPath `"{1}`" -RecoveryDirectory `"{2}`" -MaintenanceWindow -AllowDowngrade -DeviceTool `"{3}`"" -f `
        $script:manageScript, $InfPath, $recovery, $DeviceTool) 300
    $actual = Get-InstalledImageHash
    if ($actual -ne $ExpectedSysHash) {
        $why = 'If DriverVer is unchanged, Windows reuses the published package; raise DriverVer.'
        throw "Installed driver image $actual is not the $Label package ($ExpectedSysHash). $why"
    }
}

function Restore-Baseline($Config, [string]$DeviceTool) {
    $baselineSys = Get-Sha256 (Join-Path $Config.Baseline 'chmlink.sys')
    if ((Get-InstalledImageHash) -eq $baselineSys) { Write-Log 'baseline driver already installed'; return }
    Install-Package $Config (Join-Path $Config.Baseline 'chmlink.inf') $DeviceTool 'baseline' $baselineSys
}

function Save-RestoreKit {
    New-Item -ItemType Directory -Force $kitDir | Out-Null
    Copy-Item (Join-Path $script:srcDir 'tools\manage-driver.ps1') $kitDir -Force
    Copy-Item (Join-Path $script:srcDir 'build\verify\Release\chmlink-device.exe') $kitDir -Force
}

# ------------------------------------------------------------------ actions
function Invoke-Init {
    if (!(Test-Admin)) { throw 'Run elevated.' }
    if (!$BaselineFrom) { throw 'Init needs -BaselineFrom <directory with signed chmlink.inf/.sys/.cat>.' }
    foreach ($dir in 'bin', 'runs', 'baseline') { New-Item -ItemType Directory -Force (Join-Path $ciRoot $dir) | Out-Null }
    foreach ($file in 'chmlink.inf', 'chmlink.sys', 'chmlink.cat') {
        Copy-Item -LiteralPath (Join-Path $BaselineFrom $file) (Join-Path $ciRoot "baseline\$file") -Force
    }
    $thumb = $CertThumbprint
    if (!$thumb) {
        $found = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.HasPrivateKey -and $_.Subject -match 'ChmLink' } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
        if (!$found) { throw 'No ChmLink signing certificate with a private key; pass -CertThumbprint.' }
        $thumb = $found.Thumbprint
    }
    $existing = if (Test-Path $configPath) { Get-Content $configPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    $devices = @(Get-ChmLinkDevices | ForEach-Object InstanceId)
    Write-Json $configPath ([ordered]@{
        CertThumbprint = $thumb; WdkRoot = $(if ($existing.WdkRoot) { $existing.WdkRoot } else { 'C:\build\wdk' })
        KitVersion = '10.0.26100.0'; Baseline = (Join-Path $ciRoot 'baseline')
        ExpectedDevices = $devices; KeepRuns = 5; BenchRepeats = 3; BenchSeconds = 5 })
    $config = Get-Config
    $baselineInf = Join-Path $config.Baseline 'chmlink.inf'
    $baselineSys = Get-Sha256 (Join-Path $config.Baseline 'chmlink.sys')
    Write-Output "baseline: $(Get-InfVersion $baselineInf), sys $($baselineSys.Substring(0, 12))"
    Write-Output "installed image matches baseline: $((Get-InstalledImageHash) -eq $baselineSys)"
    Write-Output "expected devices: $($devices -join ', ')"
    Write-Output "certificate: $thumb"
}

function Invoke-Doctor {
    $config = Get-Config
    $check = Get-Preflight $config $false
    Write-Output ("free disk: {0} GB; image matches baseline: {1}" -f $check.FreeGB, $check.ImageMatchesBaseline)
    foreach ($warning in $check.Warnings) { Write-Output "WARN: $warning" }
    # A run restores the baseline driver and removes CI-owned leftover adapters itself, so those
    # are reported but do not block it. Everything else needs a person.
    $blocking = 0
    foreach ($problem in $check.Problems) {
        if ($problem -like 'unexpected ChmLink devices*') { Write-Output "HEALABLE: $problem (the next run removes CI-owned leftovers)" }
        else { Write-Output "PROBLEM: $problem"; $blocking++ }
    }
    if (!$check.ImageMatchesBaseline) { Write-Output 'HEALABLE: resting driver is not the baseline (the next run restores it)' }
    if ((Get-VerifierPersistent).Drivers) { Write-Output "HEALABLE: persistent Driver Verifier setting ($((Get-VerifierPersistent).Drivers)); the next run or 'restore' clears it" }
    if (Get-ScheduledTask -TaskName $resumeTask -ErrorAction SilentlyContinue) { Write-Output 'HEALABLE: the nightly resume task is registered (a run is between reboots, or was abandoned: abort/restore removes it)' }
    $bound = @(Get-ChmLinkDevices | ForEach-Object { Get-BoundInf $_.InstanceId })
    $stale = @(Get-ChmLinkPackages | Where-Object { $bound -notcontains $_.Inf })
    Write-Output ("driver store: bound {0}; unbound {1}" -f ($bound -join ', '), $(if ($stale) { ($stale | ForEach-Object { "$($_.Inf) $($_.Version)" }) -join ', ' } else { 'none' }))
    $busy = Get-LockHolder
    if ($busy) { Write-Output "BUSY: run $($busy.RunId) (pid $($busy.Pid))" }
    $runs = @(Get-ChildItem (Join-Path $ciRoot 'runs') -Directory -ErrorAction SilentlyContinue)
    Write-Output "stored runs: $($runs.Count)"
    if ($blocking) { exit 1 }
    Write-Output 'OK'
}

function Get-LockHolder {
    if (!(Test-Path -LiteralPath $lockPath)) { return $null }
    try { $holder = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
    # A lock from an earlier boot is stale even if its pid now names another process.
    if ($holder.BootId -and $holder.BootId -ne (Get-BootId)) { return $null }
    if ($holder.Pid -and (Get-Process -Id $holder.Pid -ErrorAction SilentlyContinue)) { return $holder }
    return $null   # owner is gone: stale
}

function Enter-Lock {
    $holder = Get-LockHolder
    if ($holder) { throw "VM busy with run $($holder.RunId) (pid $($holder.Pid))." }
    Write-Json $lockPath ([ordered]@{ RunId = $RunId; Pid = $PID; BootId = (Get-BootId); Started = (Get-Date).ToUniversalTime().ToString('s') })
}

# Only the owner releases the lock, so a run that failed to get it cannot free another run's.
function Exit-Lock {
    if (!(Test-Path -LiteralPath $lockPath)) { return }
    try { $holder = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $holder = $null }
    if (!$holder -or $holder.Pid -eq $PID -or !(Get-Process -Id $holder.Pid -ErrorAction SilentlyContinue)) {
        Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-Launch {
    if (!$RunId) { throw 'Launch needs -RunId.' }
    $dir = Join-Path $ciRoot "runs\$RunId"
    if (!(Test-Path -LiteralPath $dir)) { throw "No such run directory: $dir" }
    $holder = Get-LockHolder
    if ($holder) { Write-Output "BUSY run=$($holder.RunId)"; exit 4 }
    $command = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Action Run -RunId {1} -Lane {2} -StressMinutes {3} -LowResMinutes {4}' -f $PSCommandPath, $RunId, $Lane, $StressMinutes, $LowResMinutes
    $created = ([wmiclass]'Win32_Process').Create($command, $dir, $null)
    if ($created.ReturnValue -ne 0) { throw "Cannot start the run (WMI code $($created.ReturnValue))." }
    Write-Json (Join-Path $dir 'status.json') ([ordered]@{ State = 'queued'; Stage = ''; Pid = $created.ProcessId
                                                           Updated = (Get-Date).ToUniversalTime().ToString('s') })
    Write-Output "LAUNCHED pid=$($created.ProcessId)"
}

function Invoke-Poll {
    [Console]::OutputEncoding = $utf8
    $dir = Join-Path $ciRoot "runs\$RunId"
    $log = Join-Path $dir 'log.txt'
    if (!(Test-Path -LiteralPath $dir)) { Write-Output '@@STATE missing  0'; return }
    $next = $Offset
    if (Test-Path -LiteralPath $log) {
        $lines = @(Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $Offset)
        $lines | ForEach-Object { Write-Output $_ }
        $next = $Offset + $lines.Count
    }
    $state = 'unknown'; $stage = ''
    $statusPath = Join-Path $dir 'status.json'
    if (Test-Path -LiteralPath $statusPath) {
        $status = Get-Content -LiteralPath $statusPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $state = $status.State; $stage = $status.Stage
        # "rebooting" is expected to have no process: the run continues after the boot (nightly lane).
        if (!(Test-Path (Join-Path $dir 'done')) -and $state -in @('queued', 'running') -and
            !(Get-Process -Id $status.Pid -ErrorAction SilentlyContinue)) { $state = 'crashed' }
    }
    if (Test-Path (Join-Path $dir 'done')) {
        $summary = Get-Content (Join-Path $dir 'out\summary.json') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue | ConvertFrom-Json
        if ($summary) { $state = $summary.Result }
    }
    Write-Output ('@@STATE {0} {1} {2}' -f $state, $(if ($stage) { $stage } else { '-' }), $next)
}

function Invoke-Abort {
    $dir = Join-Path $ciRoot "runs\$RunId"
    $status = Get-Content (Join-Path $dir 'status.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($status.Pid -and (Get-Process -Id $status.Pid -ErrorAction SilentlyContinue)) {
        & taskkill.exe /T /F /PID $status.Pid *> $null
        Write-Output "killed run process tree $($status.Pid)"
    }
    Exit-Lock
    Unregister-ResumeTask   # a run between reboots would otherwise wake up and continue
    $statePath = Join-Path $dir 'state.json'
    if (Test-Path -LiteralPath $statePath) {
        $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $state.Phase = 'done'; $state.ExpectedReboot = $false
        Write-Json $statePath $state
    }
    Write-Json (Join-Path $dir 'status.json') ([ordered]@{ State = 'aborted'; Stage = ''; Pid = 0; Updated = (Get-Date).ToUniversalTime().ToString('s') })
    Write-Output 'aborted; run "restore" to put the baseline driver back'
}

function Invoke-Restore {
    $config = Get-Config
    $script:runDir = Join-Path $ciRoot ('runs\restore-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $script:runDir | Out-Null
    $script:logPath = Join-Path $script:runDir 'log.txt'
    $script:manageScript = Join-Path $kitDir 'manage-driver.ps1'
    $tool = Join-Path $kitDir 'chmlink-device.exe'
    if (!(Test-Path $script:manageScript) -or !(Test-Path $tool)) { throw 'No restore kit yet; a run must build once first.' }
    $RunId = 'restore'
    Enter-Lock
    try {
        # A killed nightly run can leave the startup task and the persistent verifier setting behind.
        # Without the setting cleared the next boot would start under verification; without the task
        # removed it would try to continue a run that was abandoned.
        Unregister-ResumeTask
        if ((Get-VerifierPersistent).Drivers) {
            Clear-VerifierPersistent
            Write-Log 'persistent driver verifier setting cleared (takes effect at the next boot)'
        }
        if ((Get-VerifierState).Listed) { Write-Log 'driver verifier is still tracking chmlink.sys in this boot; reboot to stop it (timings are affected until then)' }
        # Devices next: a killed suite leaves adapters bound to the candidate package.
        Remove-LeftoverTestDevices $tool $config | Out-Null
        Restore-Baseline $config $tool
    } finally { Exit-Lock }
    [Console]::OutputEncoding = $utf8
    Get-Content $script:logPath -Encoding UTF8 | Write-Output
    Write-Output "baseline restored: $((Get-InstalledImageHash) -eq (Get-Sha256 (Join-Path $config.Baseline 'chmlink.sys')))"
}

function Invoke-Clean {
    $config = Get-Config
    $runs = @(Get-ChildItem (Join-Path $ciRoot 'runs') -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
    $runs = @($runs | Sort-Object LastWriteTime)
    $excess = $runs.Count - $config.KeepRuns
    if ($excess -gt 0) {
        $runs | Select-Object -First $excess | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force; Write-Output "removed $($_.Name)" }
    }
    Write-Output "kept $([math]::Min($runs.Count, $config.KeepRuns)) run(s)"
}

# ------------------------------------------------- runs that span reboots (nightly)
# The nightly lane reboots the VM twice, so a run is a sequence of phases and the process that
# started it does not survive. Everything that must outlive a reboot is written to state.json, and
# a startup task (ChmLinkCiResume, SYSTEM) continues the run:
#   A  before: build, sign, install the candidate, run the suites WITHOUT verifier, then set
#      verifier persistently and reboot. Failing here means the candidate is simply broken.
#   B  verifier on, from boot: suites, a short two-way benchmark, a stress loop, a leak check.
#      Then clear the persistent setting and reboot.
#   C  verifier gone: restore the baseline, remove the candidate package, write the summary.
# A boot that nobody asked for (a bugcheck, a power loss) is recognised because ExpectedReboot is
# only ever set by the code that requests the reboot; the run then skips to C and says why.
$resumeTask = 'ChmLinkCiResume'

function Get-BootId { (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o') }

function Read-RunState {
    $path = Join-Path $script:runDir 'state.json'
    if (!(Test-Path -LiteralPath $path)) { return $null }
    Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Save-RunState($State) { Write-Json (Join-Path $script:runDir 'state.json') $State }

function Register-ResumeTask {
    # A copy inside the run directory: the file in bin\ is replaced by the next vm-run.sh command,
    # and the run must continue with the code it started with.
    $frozen = Join-Path $script:runDir 'vm-ci.frozen.ps1'
    Copy-Item -LiteralPath $PSCommandPath $frozen -Force
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action Resume -RunId {1}' -f $frozen, $RunId)
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 3) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $resumeTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
}

function Unregister-ResumeTask {
    Unregister-ScheduledTask -TaskName $resumeTask -Confirm:$false -ErrorAction SilentlyContinue
}

# One runner for every suite, so phases A and B call the suites the same way. $Filter only matters
# for the benchmark.
# Returns a script block that runs one suite. It is not a closure on purpose: a closure gets its own
# script scope, where $script:srcDir and the helper functions are not the ones of this script.
function New-SuiteRunner([string]$InfPath, [string]$BuildDir) {
    $script:suiteInf = $InfPath; $script:suiteBuild = $BuildDir
    {
        param([string]$Kind, [string]$Directory, [int]$Timeout, [string]$Filter = '')
        $script = switch ($Kind) { 'lifecycle' { 'test-lifecycle.ps1' } 'ip' { 'test-ip.ps1' } 'bench' { 'bench.ps1' } 'lowres' { 'test-lowres.ps1' } }
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -InfPath "{1}" -BuildDirectory "{2}" -OutputDirectory "{3}"' -f `
            (Join-Path $script:srcDir "tools\$script"), $script:suiteInf, $script:suiteBuild, $Directory
        if ($Kind -eq 'bench') { $arguments += ' -Seconds 3'; if ($Filter) { $arguments += " -Filter $Filter" } }
        if ($Kind -eq 'lowres') { $arguments += " -Minutes $Filter" }
        Invoke-Process "$Kind suite" 'powershell.exe' $arguments $Timeout
    }
}

# Take the lock again after a reboot, for a run that already owns this VM.
function Enter-Lock-For([string]$Id) {
    $holder = Get-LockHolder
    if ($holder -and $holder.RunId -ne $Id) { throw "VM busy with run $($holder.RunId) (pid $($holder.Pid))." }
    Write-Json $lockPath ([ordered]@{ RunId = $Id; Pid = $PID; BootId = (Get-BootId); Started = (Get-Date).ToUniversalTime().ToString('s') })
}

# After an unexpected reboot, keep what explains it: the bugcheck event and the newest dump.
function Save-CrashEvidence([string]$OutDir) {
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $evidence = [ordered]@{ BugcheckEvents = @(); Dumps = @() }
    try {
        $events = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001, 41, 6008 } -MaxEvents 6 -ErrorAction Stop
        $evidence.BugcheckEvents = @($events | ForEach-Object { [ordered]@{ Id = $_.Id; Time = $_.TimeCreated.ToUniversalTime().ToString('s'); Message = (($_.Message -replace '\s+', ' ').Trim()) } })
    } catch { }
    $dump = Get-ChildItem 'C:\Windows\Minidump' -Filter *.dmp -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($dump) {
        Copy-Item -LiteralPath $dump.FullName (Join-Path $OutDir $dump.Name) -Force
        $evidence.Dumps = @([ordered]@{ Name = $dump.Name; Bytes = $dump.Length; Time = $dump.LastWriteTime.ToUniversalTime().ToString('s') })
    }
    Write-Json (Join-Path $OutDir 'crash.json') $evidence
    foreach ($e in $evidence.BugcheckEvents | Select-Object -First 2) { Write-Log "  event $($e.Id) at $($e.Time): $(([string]$e.Message).Substring(0, [math]::Min(160, ([string]$e.Message).Length)))" }
    if ($dump) { Write-Log "  minidump kept: $($dump.Name) ($($dump.Length) bytes)" }
}

function Write-RunSummary([string]$OutDir, $State, [string]$Result, [bool]$VmClean) {
    $config = Get-Config
    $summary = [ordered]@{
        RunId = $RunId; Lane = $State.Lane; Result = $Result; VmClean = $VmClean; Crashed = [bool]$State.Crashed
        Started = $State.StartedUtc; Finished = (Get-Date).ToUniversalTime().ToString('s')
        Seconds = [math]::Round(((Get-Date).ToUniversalTime() - [datetime]::Parse($State.StartedUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal)).TotalSeconds, 0)
        DriverVersion = $State.DriverVersion; Stages = @($script:stages)
        Computer = $env:COMPUTERNAME; BaselineVersion = (Get-InfVersion (Join-Path $config.Baseline 'chmlink.inf'))
    }
    Write-Json (Join-Path $OutDir 'summary.json') $summary
    Copy-Item -LiteralPath $script:logPath (Join-Path $OutDir 'log.txt') -Force
    Set-Status $Result ''
    Write-Log "result: $Result in $($summary.Seconds) s"
    Set-Content -LiteralPath (Join-Path $script:runDir 'done') -Value $Result
    try { Invoke-Clean | Out-Null } catch { }
}

# Mark the reboot as ours, release the lock (the next boot's process takes it again) and restart.
# Order matters when verifier is being armed (phase A): state first, then the startup task, then
# the verifier setting, then the reboot. Interrupted at any point, either verifier is not set yet or
# a task exists that will find the state and clear it.
# Windows signals running processes when it shuts down, and PowerShell then unwinds: catch and finally
# blocks run on the way out. Anything they did (clearing the verifier setting, restoring the baseline)
# would undo the handover or race the shutdown. Once a reboot is requested they only wait to be stopped.
$script:rebootPending = $false
function Stop-IfRebooting {
    if ($script:rebootPending) { Start-Sleep -Seconds 600 }
}

function Request-Reboot($State, [string]$NextPhase, [string]$Why) {
    $State.Phase = $NextPhase
    $State.ExpectedReboot = $true
    $State.BootIdBeforeReboot = Get-BootId
    Save-RunState $State
    Set-Status 'rebooting' $Why
    Write-Log "rebooting for: $Why (continues in phase $NextPhase)"
    Exit-Lock
    $script:rebootPending = $true
    & shutdown.exe /r /t 5 /c "ChmLink CI: $Why" | Out-Null
    # shutdown returns at once; stay alive so that the caller does not run on into the next stage.
    Start-Sleep -Seconds 120
    $script:rebootPending = $false
    throw 'The reboot did not happen within two minutes.'
}

# ----------------------------------------------------- certificate renewal
# The test certificate expires. Renewing means more than a new certificate: the baseline package
# is signed with the old one, and Windows reuses a published package whose INF text is unchanged,
# so the baseline is re-signed AND given a new DriverVer before it is installed again.
function Invoke-RenewCert {
    if (!(Test-Admin)) { throw 'Run elevated.' }
    $config = Get-Config
    $holder = Get-LockHolder
    if ($holder) { throw "VM busy with run $($holder.RunId)." }
    $old = Get-ChildItem Cert:\LocalMachine\My | Where-Object Thumbprint -eq $config.CertThumbprint
    if ($old) { Write-Output ("current certificate: {0} expires {1} ({2} days)" -f $config.CertThumbprint.Substring(0, 8), $old.NotAfter.ToString('yyyy-MM-dd'), [int]($old.NotAfter - (Get-Date)).TotalDays) }
    $kitTool = Join-Path $kitDir 'chmlink-device.exe'; $kitManage = Join-Path $kitDir 'manage-driver.ps1'
    if (!(Test-Path $kitTool) -or !(Test-Path $kitManage)) { throw 'No restore kit yet; a run must build once first.' }
    $sdk = Join-Path $config.WdkRoot "microsoft.windows.sdk.cpp\c\bin\$($config.KitVersion)\x64\signtool.exe"
    $inf2cat = Join-Path $config.WdkRoot "microsoft.windows.wdk.x64\c\bin\$($config.KitVersion)\x86\Inf2Cat.exe"

    $work = Join-Path $ciRoot ('renew-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $work | Out-Null
    $script:runDir = $work; $script:logPath = Join-Path $work 'log.txt'; $script:deadline = $null
    $stage = Join-Path $work 'package'
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($file in 'chmlink.inf', 'chmlink.sys') { Copy-Item (Join-Path $config.Baseline $file) $stage -Force }

    # DriverVer: raise the last component. The date part stays; only the version decides.
    $infPath = Join-Path $stage 'chmlink.inf'
    $text = [IO.File]::ReadAllText($infPath)
    $match = [regex]::Match($text, '(?m)^(\s*DriverVer\s*=\s*[^,]+,\s*)(\d+)\.(\d+)\.(\d+)\.(\d+)\s*$')
    if (!$match.Success) { throw 'No DriverVer in the baseline INF.' }
    $oldVersion = '{0}.{1}.{2}.{3}' -f $match.Groups[2].Value, $match.Groups[3].Value, $match.Groups[4].Value, $match.Groups[5].Value
    $newVersion = '{0}.{1}.{2}.{3}' -f $match.Groups[2].Value, $match.Groups[3].Value, $match.Groups[4].Value, ([int]$match.Groups[5].Value + 1)
    [IO.File]::WriteAllText($infPath, $text.Replace($match.Value, $match.Groups[1].Value + $newVersion), (New-Object Text.UTF8Encoding($false)))
    Write-Output "baseline DriverVer: $oldVersion -> $newVersion"
    if ($WhatIfRenew) { Write-Output "WHATIF: would create a $ValidDays-day certificate, re-sign and reinstall the baseline. Nothing changed."; Remove-Item $work -Recurse -Force; return }

    # 1) new certificate, trusted the same way as the current one
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=ChmLink VM Test Signing' -CertStoreLocation Cert:\LocalMachine\My `
        -NotAfter (Get-Date).AddDays($ValidDays) -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable
    $cer = Join-Path $work 'test-signing.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
    Write-Output ("new certificate: {0} expires {1}" -f $cert.Thumbprint.Substring(0, 8), $cert.NotAfter.ToString('yyyy-MM-dd'))

    # 2) re-sign. Everything is verified before any device is touched.
    $sign = '/fd sha256 /sm /s My /sha1 {0}' -f $cert.Thumbprint
    Invoke-Process 'sign sys' $sdk ("sign /q $sign `"$stage\chmlink.sys`"") 120
    Invoke-Process 'inf2cat' $inf2cat ("/driver:`"$stage`" /os:10_X64,Server10_X64 /uselocaltime") 300
    Invoke-Process 'sign cat' $sdk ("sign /q $sign `"$stage\chmlink.cat`"") 120
    Invoke-Process 'verify catalog' $sdk ("verify /q /pa /c `"$stage\chmlink.cat`" `"$stage\chmlink.sys`"") 120
    $signer = (Get-AuthenticodeSignature (Join-Path $stage 'chmlink.sys')).SignerCertificate.Thumbprint
    if ($signer -ne $cert.Thumbprint) { throw 'The re-signed driver is not signed by the new certificate.' }

    # 3) install it over the anchor device through the normal upgrade path (it rolls itself back on failure)
    $script:manageScript = $kitManage
    $newSys = Get-Sha256 (Join-Path $stage 'chmlink.sys')
    try { Install-Package $config (Join-Path $stage 'chmlink.inf') $kitTool 'renewed baseline' $newSys }
    catch {
        Write-Output "FAILED to install the renewed baseline: $($_.Exception.Message)"
        Write-Output 'The old baseline and certificate are unchanged. The new certificate stays in the stores; remove it if unwanted.'
        throw
    }

    # 4) only now switch the baseline and the configuration
    foreach ($file in 'chmlink.inf', 'chmlink.sys', 'chmlink.cat') { Copy-Item (Join-Path $stage $file) $config.Baseline -Force }
    $raw = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $raw.CertThumbprint = $cert.Thumbprint
    Write-Json $configPath $raw
    Write-Output ("baseline is now {0}, signed by {1}; config updated." -f $newVersion, $cert.Thumbprint.Substring(0, 8))
    Write-Output "The old certificate $($config.CertThumbprint.Substring(0, 8)) is left in place (existing signatures keep chaining to it)."
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

# Continues a nightly run after a reboot. Started by the startup task, so it runs as SYSTEM.
function Invoke-Resume {
    if (!$RunId) { throw 'Resume needs -RunId.' }
    $config = Get-Config
    $script:runDir = Join-Path $ciRoot "runs\$RunId"
    $script:srcDir = Join-Path $script:runDir 'src'
    $script:manageScript = Join-Path $script:srcDir 'tools\manage-driver.ps1'
    $script:logPath = Join-Path $script:runDir 'log.txt'
    $out = Join-Path $script:runDir 'out'
    $state = Read-RunState
    if (!$state) { return }                                   # not a run of ours: nothing to do
    if ($state.Phase -eq 'done') { Unregister-ResumeTask; return }
    $build = Join-Path $script:srcDir 'build\verify\Release'
    $signed = Join-Path $script:srcDir 'build\signed'
    $candidateInf = Join-Path $signed 'chmlink.inf'
    $tool = Join-Path $build 'chmlink-device.exe'
    $script:deadline = [datetime]::Parse($state.DeadlineUtc).ToUniversalTime()
    $script:deadline = [TimeZoneInfo]::ConvertTimeFromUtc($script:deadline, [TimeZoneInfo]::Local)
    $script:stages = @($state.Stages)
    $script:candidateOem = [string]$state.CandidateOem
    $script:driverVersion = [string]$state.DriverVersion
    $script:installStarted = $true
    $suite = New-SuiteRunner $candidateInf $build
    $unexpected = $false

    # What kind of boot is this?
    $bootNow = Get-BootId
    if ($state.ExpectedReboot -and $bootNow -ne $state.BootIdBeforeReboot) {
        Write-Log "boot after a requested reboot (phase $($state.Phase))"
    } elseif ($bootNow -eq $state.BootIdBeforeReboot) {
        Write-Log 'resume task fired again within the same boot; nothing to do'
        return
    } else {
        $unexpected = $true
        Write-Log "UNEXPECTED REBOOT: the system restarted while phase $($state.Phase) was not asking for it"
        Save-CrashEvidence $out
    }
    $state.ExpectedReboot = $false
    Enter-Lock-For $RunId
    Set-Status 'running' "resume-$($state.Phase)"
    $result = [string]$state.Result
    if ($unexpected) { $result = 'failed'; $state.Phase = 'C'; $state.Crashed = $true }
    Save-RunState $state

    try {
        if ($state.Phase -eq 'B') {
            try {
                Invoke-Stage 'verifier-active' {
                    # The startup task runs seconds after boot, before PnP has started the root-enumerated
                    # adapter; the driver is not loaded (so not listed) until then.
                    $waitEnd = (Get-Date).AddSeconds(180)
                    $now = Get-VerifierState
                    while (!$now.Listed -and (Get-Date) -lt $waitEnd) { Start-Sleep -Seconds 5; $now = Get-VerifierState }
                    if (!$now.Listed) { throw 'Driver Verifier is not tracking chmlink.sys after the reboot.' }
                    $state.LeakCurrent = $now.CurrentAllocations; $state.LeakBytes = $now.CurrentBytes
                    Write-Log ("verifier on from boot. Idle driver: {0} live allocations, {1} bytes" -f $now.CurrentAllocations, $now.CurrentBytes)
                    Save-RunState $state
                }
                Invoke-Stage 'verifier-lifecycle' { & $suite 'lifecycle' (Join-Path $out 'verifier-lifecycle') 1500 }
                Invoke-Stage 'verifier-ip' { & $suite 'ip' (Join-Path $out 'verifier-ip') 900 }
                # Verifier slows the data path several times over, so the benchmark here proves behaviour
                # under concurrency, not speed. Its numbers are never compared with the baseline.
                Invoke-Stage 'verifier-duplex' { & $suite 'bench' (Join-Path $out 'verifier-duplex') 900 'duplex' }
                $stressEnd = (Get-Date).AddMinutes([int]$state.StressMinutes)
                $iteration = 0
                while ((Get-Date) -lt $stressEnd) {
                    $iteration++
                    Invoke-Stage "stress-$iteration" {
                        & $suite 'lifecycle' (Join-Path $out "stress-$iteration-lifecycle") 1500
                        & $suite 'ip' (Join-Path $out "stress-$iteration-ip") 900
                        & $suite 'bench' (Join-Path $out "stress-$iteration-bench") 900 '2injectors'
                    }
                }
                Invoke-Stage 'verifier-leaks' {
                    # Every suite adapter is gone. What the driver holds must be back at the level measured
                    # at boot; the reference is measured, not assumed.
                    $after = Get-VerifierState
                    Write-Log ("after {0} stress iteration(s): {1} live allocations, {2} bytes, peak {3} (idle at boot: {4}, {5} bytes)" -f `
                        $iteration, $after.CurrentAllocations, $after.CurrentBytes, $after.PeakAllocations, $state.LeakCurrent, $state.LeakBytes)
                    if ($after.CurrentAllocations -gt $state.LeakCurrent -or $after.CurrentBytes -gt $state.LeakBytes) {
                        throw ("chmlink.sys holds {0} allocations / {1} bytes after the suites, {2} / {3} when idle: possible leak." -f `
                            $after.CurrentAllocations, $after.CurrentBytes, $state.LeakCurrent, $state.LeakBytes)
                    }
                }
                $result = 'passed'
            } catch {
                Write-Log "phase B failed: $($_.Exception.Message)"
                $result = 'failed'
            }
            $state.Result = $result; $state.Stages = @($script:stages)
            if ($result -eq 'passed' -and [int]$state.LowResMinutes -gt 0) {
                Set-VerifierLowResources
                Request-Reboot $state 'L' 'turning on low resources simulation'
            }
            Clear-VerifierPersistent
            Request-Reboot $state 'C' 'switching driver verifier off'
        }
        if ($state.Phase -eq 'L') {
            try {
                Invoke-Stage 'lowres-active' {
                    $waitEnd = (Get-Date).AddSeconds(180)
                    $now = Get-VerifierState
                    while (!$now.Listed -and (Get-Date) -lt $waitEnd) { Start-Sleep -Seconds 5; $now = Get-VerifierState }
                    if (!$now.Listed) { throw 'Driver Verifier is not tracking chmlink.sys after the reboot.' }
                    $state.LowIdleAllocations = $now.CurrentAllocations; $state.LowIdleBytes = $now.CurrentBytes
                    Write-Log ("low resources simulation armed. Idle driver: {0} live allocations, {1} bytes" -f $now.CurrentAllocations, $now.CurrentBytes)
                    Save-RunState $state
                    # Faults start one minute after boot; the suite must run inside that window.
                    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
                    $armed = $boot.AddSeconds(75)
                    if ((Get-Date) -lt $armed) { Start-Sleep -Seconds ([int][math]::Ceiling(($armed - (Get-Date)).TotalSeconds)) }
                }
                Invoke-Stage 'lowres-stress' { & $suite 'lowres' (Join-Path $out 'lowres') ([int]$state.LowResMinutes * 60 + 900) ([string][int]$state.LowResMinutes) }
                Invoke-Stage 'lowres-leaks' {
                    $stats = Get-VerifierStatistics
                    Write-Log ("fault injection: {0} of {1} pool allocations failed deliberately (counters {2})" -f $stats.FailedDeliberately, $stats.Attempted, $stats.Raw)
                    # A run in which nothing was failed proves nothing about the failure paths.
                    if ($stats.FailedDeliberately -eq 0) { throw 'Driver Verifier injected no allocation failures; the low resources suite did not exercise any failure path.' }
                    $after = Get-VerifierState
                    Write-Log ("after fault injection: {0} live allocations, {1} bytes (idle at boot: {2}, {3} bytes)" -f `
                        $after.CurrentAllocations, $after.CurrentBytes, $state.LowIdleAllocations, $state.LowIdleBytes)
                    if ($after.CurrentAllocations -gt $state.LowIdleAllocations -or $after.CurrentBytes -gt $state.LowIdleBytes) {
                        throw ("chmlink.sys holds {0} allocations / {1} bytes after the fault-injection suite, {2} / {3} when idle: a failure path leaks." -f `
                            $after.CurrentAllocations, $after.CurrentBytes, $state.LowIdleAllocations, $state.LowIdleBytes)
                    }
                }
            } catch {
                Write-Log "phase L failed: $($_.Exception.Message)"
                $result = 'failed'
            }
            $state.Result = $result; $state.Stages = @($script:stages)
            Clear-VerifierPersistent
            Request-Reboot $state 'C' 'switching driver verifier off'
        }
    } catch {
        Stop-IfRebooting
        Write-Log "resume failed: $($_.Exception.Message)"
        if ($result -ne 'failed') { $result = 'infra-failed' }
    }
    Stop-IfRebooting

    # Phase C: verifier is off (or the run is being abandoned): put the VM back and report.
    $vmClean = $true
    try {
        if ((Get-VerifierPersistent).Drivers) { Clear-VerifierPersistent }
        if ((Get-VerifierState).Listed) {
            # Still tracked: this boot began with verifier on. Restoring now would reload the driver under
            # verification, which is harmless but slow; one more reboot makes the end state clean.
            if (!$state.ExtraRebootDone) {
                $state.ExtraRebootDone = $true; $state.Result = $result; $state.Stages = @($script:stages)
                Request-Reboot $state 'C' 'verifier still tracking after a crash; clearing it'
            }
        }
        $script:deadline = (Get-Date).AddMinutes(15)
        Invoke-Stage 'restore' { Restore-Baseline $config $tool }
        $bound = Get-BoundInf (Get-ChmLinkDevices | Select-Object -First 1).InstanceId
        if ($script:candidateOem -and $script:candidateOem -ne $bound) {
            try {
                Invoke-Process 'remove candidate package' 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -Action RemovePackage -PublishedInf {1} -MaintenanceWindow -DeviceTool `"{2}`"" -f `
                    $script:manageScript, $script:candidateOem, $tool) 120
            } catch { Write-Log "warning: could not remove $($script:candidateOem): $($_.Exception.Message)" }
        }
    } catch {
        $vmClean = $false; $result = 'infra-failed'
        Write-Log "RESTORE FAILED; the VM needs attention: $($_.Exception.Message)"
    }
    Unregister-ResumeTask
    $state.Phase = 'done'; Save-RunState $state
    Exit-Lock
    Write-RunSummary $out $state $result $vmClean
}

# --------------------------------------------------------------------- run
function Invoke-Run {
    if (!$RunId) { throw 'Run needs -RunId.' }
    $config = Get-Config
    $script:runDir = Join-Path $ciRoot "runs\$RunId"
    $script:srcDir = Join-Path $script:runDir 'src'
    $script:manageScript = Join-Path $script:srcDir 'tools\manage-driver.ps1'
    $script:logPath = Join-Path $script:runDir 'log.txt'
    $out = Join-Path $script:runDir 'out'
    New-Item -ItemType Directory -Force $out | Out-Null
    $stressMinutes = if ($StressMinutes -gt 0) { $StressMinutes } elseif ($config.NightlyStressMinutes) { [int]$config.NightlyStressMinutes } else { 20 }
    $lowResMinutes = if ($LowResMinutes -ge 0) { $LowResMinutes } elseif ($null -ne $config.NightlyLowResMinutes) { [int]$config.NightlyLowResMinutes } else { 10 }
    $laneMinutes = switch ($Lane) { 'full' { 75 } 'nightly' { 40 + $stressMinutes + $(if ($lowResMinutes) { $lowResMinutes + 15 } else { 0 }) } default { 30 } }
    $script:deadline = (Get-Date).AddMinutes($laneMinutes)
    $started = Get-Date
    $result = 'failed'; $vmClean = $true; $handedOver = $false
    $build = Join-Path $script:srcDir 'build\verify\Release'
    $signed = Join-Path $script:srcDir 'build\signed'
    $tool = $null
    Write-Log "run $RunId lane=$Lane on $env:COMPUTERNAME, deadline in $laneMinutes min"
    try {
        Enter-Lock
        Invoke-Stage 'preflight' {
            $check = Get-Preflight $config $false
            if ($check.Problems | Where-Object { $_ -like 'unexpected ChmLink devices*' }) {
                # Probably a killed earlier run. Its device tool is not built yet, so use the saved kit.
                $kitTool = Join-Path $kitDir 'chmlink-device.exe'
                if (Test-Path $kitTool) {
                    Write-Log 'unexpected devices found; removing those owned by CI suites'
                    Remove-LeftoverTestDevices $kitTool $config | Out-Null
                    $check = Get-Preflight $config $false
                }
            }
            $check.Warnings | ForEach-Object { Write-Log "warning: $_" }
            if (!$check.ImageMatchesBaseline) {
                # The restore needs this run's device tool, so it happens right after the user-mode build.
                Write-Log 'resting driver is not the baseline (previous run?); it will be restored first'
                $script:needsRestore = $true
            }
            if ($check.Problems.Count) { throw ($check.Problems -join '; ') }
        }
        Invoke-Stage 'build-user' {
            Invoke-Process 'cmake configure' 'cmake.exe' '-S . -B build/verify -G "Visual Studio 17 2022" -A x64 -DCMAKE_COMPILE_WARNING_AS_ERROR=ON' 300 $script:srcDir
            Invoke-Process 'cmake build' 'cmake.exe' '--build build/verify --config Release -- /m /v:minimal /nologo' 900 $script:srcDir
        }
        Invoke-Stage 'ctest' {
            Invoke-Process 'ctest' 'ctest.exe' '--test-dir build/verify -C Release --output-on-failure' 300 $script:srcDir
        }
        $tool = Join-Path $build 'chmlink-device.exe'
        Save-RestoreKit
        if ($script:needsRestore) { Invoke-Stage 'restore-leftover' { Restore-Baseline $config $tool } }
        Invoke-Stage 'build-driver' {
            Invoke-Process 'build driver' 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -KitRoot `"{1}`" -Output `"{2}`" -Analyze" -f `
                (Join-Path $script:srcDir 'tools\build-driver.ps1'), $config.WdkRoot, (Join-Path $script:srcDir 'build\driver')) 900 $script:srcDir
            # Kept with the results: a live dump or minidump of this run is only readable with it.
            $pdb = Join-Path $script:srcDir 'build\driver\chmlink.pdb'
            if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb (Join-Path $out 'chmlink.pdb') -Force }
        }
        Invoke-Stage 'sign' {
            New-Item -ItemType Directory -Force $signed | Out-Null
            $driverOut = Join-Path $script:srcDir 'build\driver'
            Copy-Item (Join-Path $driverOut 'chmlink.sys'), (Join-Path $driverOut 'chmlink.inf') $signed -Force
            $signtool = Join-Path $config.WdkRoot "microsoft.windows.sdk.cpp\c\bin\$($config.KitVersion)\x64\signtool.exe"
            $inf2cat = Join-Path $config.WdkRoot "microsoft.windows.wdk.x64\c\bin\$($config.KitVersion)\x86\Inf2Cat.exe"
            $sign = '/fd sha256 /sm /s My /sha1 {0}' -f $config.CertThumbprint
            Invoke-Process 'sign sys' $signtool ("sign /q $sign `"$signed\chmlink.sys`"") 120
            Invoke-Process 'inf2cat' $inf2cat ("/driver:`"$signed`" /os:10_X64,Server10_X64 /uselocaltime") 300
            Invoke-Process 'sign cat' $signtool ("sign /q $sign `"$signed\chmlink.cat`"") 120
            Invoke-Process 'verify catalog' $signtool ("verify /q /pa /c `"$signed\chmlink.cat`" `"$signed\chmlink.sys`"") 120
        }
        $candidateInf = Join-Path $signed 'chmlink.inf'
        $candidateSys = Get-Sha256 (Join-Path $signed 'chmlink.sys')
        $script:driverVersion = Get-InfVersion $candidateInf
        Write-Log "candidate driver $($script:driverVersion), sys $($candidateSys.Substring(0, 12))"
        Invoke-Stage 'clean-store' {
            # Windows reuses a published package whose INF is unchanged, and every build differs in its
            # link timestamp. A leftover package of the candidate's version would therefore win over the
            # new binary, and a candidate with the baseline's version would silently be the baseline.
            $baselineVersion = Get-InfVersion (Join-Path $config.Baseline 'chmlink.inf')
            if ($script:driverVersion -eq $baselineVersion) {
                throw "Candidate DriverVer $($script:driverVersion) equals the baseline's; Windows would keep the baseline package. Raise DriverVer."
            }
            $bound = @(Get-ChmLinkDevices | ForEach-Object { Get-BoundInf $_.InstanceId })
            foreach ($package in @(Get-ChmLinkPackages | Where-Object { $_.Version -eq $script:driverVersion -and $bound -notcontains $_.Inf })) {
                Write-Log "removing leftover package $($package.Inf) ($($package.Version))"
                Invoke-Process "remove $($package.Inf)" 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -Action RemovePackage -PublishedInf {1} -MaintenanceWindow -DeviceTool `"{2}`"" -f `
                    $script:manageScript, $package.Inf, $tool) 120
            }
        }
        Invoke-Stage 'install' {
            $script:installStarted = $true
            Install-Package $config $candidateInf $tool 'candidate' $candidateSys
            $script:candidateOem = Get-BoundInf (Get-ChmLinkDevices | Select-Object -First 1).InstanceId
            Write-Log "candidate bound as $($script:candidateOem)"
        }
        $suite = {
            param([string]$Kind, [string]$Directory, [int]$Timeout)
            $script = if ($Kind -eq 'lifecycle') { 'test-lifecycle.ps1' } else { 'test-ip.ps1' }
            Invoke-Process "$Kind suite" 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -InfPath `"{1}`" -BuildDirectory `"{2}`" -OutputDirectory `"{3}`"" -f `
                (Join-Path $script:srcDir "tools\$script"), $candidateInf, $build, $Directory) $Timeout
        }
        Invoke-Stage 'lifecycle' { & $suite 'lifecycle' (Join-Path $out 'lifecycle') 900 }
        Invoke-Stage 'ip' { & $suite 'ip' (Join-Path $out 'ip') 600 }
        if ($Lane -eq 'full') {
            for ($i = 1; $i -le $config.BenchRepeats; $i++) {
                Invoke-Stage "bench-$i" {
                    Invoke-Process "benchmark $i" 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -InfPath `"{1}`" -BuildDirectory `"{2}`" -OutputDirectory `"{3}`" -Seconds {4}" -f `
                        (Join-Path $script:srcDir 'tools\bench.ps1'), $candidateInf, $build, (Join-Path $out "bench-$i"), $config.BenchSeconds) 900
                }
            }
        }
        if ($Lane -eq 'nightly') {
            # Candidate installed and healthy without verifier. Hand the rest to the phase machine, which
            # survives the reboot. Order matters (see Request-Reboot): state, task, verifier, reboot.
            $state = [pscustomobject]@{
                Lane = $Lane; Phase = 'B'; ExpectedReboot = $false; BootIdBeforeReboot = ''; Result = ''
                StressMinutes = $stressMinutes; LowResMinutes = $lowResMinutes; LowIdleAllocations = -1; LowIdleBytes = -1
                StartedUtc = $started.ToUniversalTime().ToString('s')
                DeadlineUtc = $script:deadline.ToUniversalTime().ToString('o')
                CandidateOem = $script:candidateOem; DriverVersion = $script:driverVersion
                Stages = @($script:stages); Crashed = $false; ExtraRebootDone = $false
                LeakCurrent = -1; LeakBytes = -1
            }
            Save-RunState $state
            Register-ResumeTask
            Set-VerifierPersistent
            $handedOver = $true
            Request-Reboot $state 'B' 'turning driver verifier on'
        }
        $result = 'passed'
    }
    catch {
        Stop-IfRebooting
        Write-Log "run failed: $($_.Exception.Message)"
        $failedStage = @($script:stages | Where-Object Status -eq 'failed' | Select-Object -First 1)
        $result = if ($failedStage.Count -and $failedStage[0].Name -eq 'preflight') { 'infra-failed' } else { 'failed' }
    }
    finally {
        Stop-IfRebooting
        # Reached with $handedOver only when the reboot was requested but did not happen (the process
        # would otherwise have been stopped by it). Undo the arming so that the VM is left as found.
        if ($handedOver) {
            try { Clear-VerifierPersistent } catch { Write-Log "warning: $($_.Exception.Message)" }
            Unregister-ResumeTask
        }
        # The VM must come back to the baseline whatever happened above.
        if ($script:installStarted -or ((Get-InstalledImageHash) -ne (Get-Sha256 (Join-Path $config.Baseline 'chmlink.sys')) -and $tool)) {
            try {
                $script:deadline = (Get-Date).AddMinutes(15)
                Invoke-Stage 'restore' { Restore-Baseline $config $tool }
                $bound = Get-BoundInf (Get-ChmLinkDevices | Select-Object -First 1).InstanceId
                if ($script:candidateOem -and $script:candidateOem -ne $bound) {
                    try {
                        Invoke-Process 'remove candidate package' 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -Action RemovePackage -PublishedInf {1} -MaintenanceWindow -DeviceTool `"{2}`"" -f `
                            $script:manageScript, $script:candidateOem, $tool) 120
                    } catch { Write-Log "warning: could not remove $($script:candidateOem): $($_.Exception.Message)" }
                }
            } catch {
                $vmClean = $false; $result = 'infra-failed'
                Write-Log "RESTORE FAILED; the VM needs attention: $($_.Exception.Message)"
            }
        }
        Exit-Lock
        $summary = [ordered]@{
            RunId = $RunId; Lane = $Lane; Result = $result; VmClean = $vmClean
            Started = $started.ToUniversalTime().ToString('s'); Finished = (Get-Date).ToUniversalTime().ToString('s')
            Seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 0)
            DriverVersion = $script:driverVersion; Stages = @($script:stages)
            Computer = $env:COMPUTERNAME; BaselineVersion = (Get-InfVersion (Join-Path $config.Baseline 'chmlink.inf'))
        }
        Write-Json (Join-Path $out 'summary.json') $summary
        Copy-Item -LiteralPath $script:logPath (Join-Path $out 'log.txt') -Force
        Set-Status $result ''
        Write-Log "result: $result in $($summary.Seconds) s"
        Set-Content -LiteralPath (Join-Path $script:runDir 'done') -Value $result
        try { Invoke-Clean | Out-Null } catch { }
    }
}

switch ($Action) {
    'Init'    { Invoke-Init }
    'Doctor'  { Invoke-Doctor }
    'Launch'  { Invoke-Launch }
    'Run'     { Invoke-Run }
    'Poll'    { Invoke-Poll }
    'Abort'   { Invoke-Abort }
    'Restore' { Invoke-Restore }
    'Clean'   { Invoke-Clean }
    'RenewCert' { Invoke-RenewCert }
    'Resume'  { Invoke-Resume }
}
