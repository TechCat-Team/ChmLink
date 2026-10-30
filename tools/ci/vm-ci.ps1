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
    [ValidateSet('Init', 'Doctor', 'Launch', 'Run', 'Poll', 'Abort', 'Restore', 'Clean')]
    [string]$Action,
    [string]$RunId,
    [ValidateSet('smoke', 'full')][string]$Lane = 'smoke',
    [int]$Offset = 0,
    [string]$BaselineFrom,
    [string]$CertThumbprint
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
    if ($memory.VerifyDrivers -or $memory.VerifyDriverLevel) { $warnings.Add('Driver Verifier is enabled; timings are not comparable') }
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
    if ($holder.Pid -and (Get-Process -Id $holder.Pid -ErrorAction SilentlyContinue)) { return $holder }
    return $null   # owner is gone: stale
}

function Enter-Lock {
    $holder = Get-LockHolder
    if ($holder) { throw "VM busy with run $($holder.RunId) (pid $($holder.Pid))." }
    Write-Json $lockPath ([ordered]@{ RunId = $RunId; Pid = $PID; Started = (Get-Date).ToUniversalTime().ToString('s') })
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
    $command = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Action Run -RunId {1} -Lane {2}' -f $PSCommandPath, $RunId, $Lane
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
        # Devices first: a killed suite leaves adapters bound to the candidate package.
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
    $laneMinutes = if ($Lane -eq 'full') { 75 } else { 30 }
    $script:deadline = (Get-Date).AddMinutes($laneMinutes)
    $started = Get-Date
    $result = 'failed'; $vmClean = $true
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
        Invoke-Stage 'lifecycle' {
            Invoke-Process 'lifecycle suite' 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -InfPath `"{1}`" -BuildDirectory `"{2}`" -OutputDirectory `"{3}`"" -f `
                (Join-Path $script:srcDir 'tools\test-lifecycle.ps1'), $candidateInf, $build, (Join-Path $out 'lifecycle')) 900
        }
        Invoke-Stage 'ip' {
            Invoke-Process 'ip suite' 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -InfPath `"{1}`" -BuildDirectory `"{2}`" -OutputDirectory `"{3}`"" -f `
                (Join-Path $script:srcDir 'tools\test-ip.ps1'), $candidateInf, $build, (Join-Path $out 'ip')) 600
        }
        if ($Lane -eq 'full') {
            for ($i = 1; $i -le $config.BenchRepeats; $i++) {
                Invoke-Stage "bench-$i" {
                    Invoke-Process "benchmark $i" 'powershell.exe' ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`" -InfPath `"{1}`" -BuildDirectory `"{2}`" -OutputDirectory `"{3}`" -Seconds {4}" -f `
                        (Join-Path $script:srcDir 'tools\bench.ps1'), $candidateInf, $build, (Join-Path $out "bench-$i"), $config.BenchSeconds) 900
                }
            }
        }
        $result = 'passed'
    }
    catch {
        Write-Log "run failed: $($_.Exception.Message)"
        $failedStage = @($script:stages | Where-Object Status -eq 'failed' | Select-Object -First 1)
        $result = if ($failedStage.Count -and $failedStage[0].Name -eq 'preflight') { 'infra-failed' } else { 'failed' }
    }
    finally {
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
}
