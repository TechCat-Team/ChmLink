#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [int]$Minutes = 10
)
# Run under Driver Verifier fault injection (randomized low resources). Allocation failures in the
# driver are expected and individual operations may fail; what must hold is that nothing crashes or
# hangs, every adapter can be removed, and nothing the driver allocated is left behind (the caller
# compares Verifier's live allocation counters afterwards).
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/verify/Release' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '../build/lowres-results' }
$ErrorActionPreference = 'Stop'
$owner = [guid]'9AB3CDB8-3693-4C1F-89B3-4062BB57BE83'
$devices = [Collections.Generic.List[object]]::new()
$exitCode = 1
$stats = [ordered]@{ Cycles = 0; Created = 0; NeverStarted = 0; FuzzRuns = 0; FuzzSkipped = 0; Toggles = 0; ToggleFailures = 0 }

function Log([string]$Message) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Add-Content -LiteralPath (Join-Path $output.FullName 'lowres.log') -Value $line -Encoding UTF8
    $line
}
function Run-Device([string[]]$Arguments) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $text = & $tool @Arguments | Out-String; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    [pscustomobject]@{ Code = $code; Text = $text }
}
# Every PnP-level step runs in a job with a deadline: a hang is the failure this suite looks for.
function Invoke-Bounded([scriptblock]$Block, [object[]]$Arguments, [int]$Seconds, [string]$What) {
    $job = Start-Job -ScriptBlock $Block -ArgumentList $Arguments
    try {
        if (!(Wait-Job $job -Timeout $Seconds)) { throw "HANG: $What did not finish within $Seconds s" }
        $null = Receive-Job $job -ErrorAction SilentlyContinue
        return $job.State -eq 'Completed' -and !$job.ChildJobs[0].Error.Count
    } finally { Remove-Job $job -Force -ErrorAction SilentlyContinue }
}
function Find-Interface($Device, [int]$Seconds) {
    $end = (Get-Date).AddSeconds($Seconds)
    do {
        $windows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $Device.InstanceId)
        if ($windows.Count -eq 1 -and $windows[0].GUID) {
            $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
            if ($adapter.Count -eq 1) { return $adapter[0] }
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $end)
    return $null
}
function Remove-TestDevice($Device) {
    $removed = Invoke-Bounded { param($t, $id, $o) & $t remove $id --owner $o | Out-Null; if ($LASTEXITCODE) { throw "remove exited $LASTEXITCODE" } } `
        @($tool, $Device.InstanceId, $owner.ToString('B')) 120 "removing $($Device.InstanceId)"
    if (!$removed) { throw "Could not remove $($Device.InstanceId) under fault injection." }
    [void]$devices.Remove($Device)
}

try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in an elevated disposable test VM.' }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $tool = Join-Path $BuildDirectory 'chmlink-device.exe'
    $fuzz = Join-Path $BuildDirectory 'chmlink-ioctl-fuzz.exe'
    foreach ($file in $tool, $fuzz) { if (!(Test-Path -LiteralPath $file)) { throw "Missing $file" } }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    $InfPath = (Resolve-Path -LiteralPath $InfPath).Path
    $deadline = (Get-Date).AddMinutes($Minutes)
    while ((Get-Date) -lt $deadline) {
        $stats.Cycles++
        $created = Run-Device @('create', $InfPath, '--owner', $owner.ToString('B'), '--name', 'ChmLink LowRes')
        if ($created.Code -ne 0 -and $created.Code -ne 3010) { Log "create exited $($created.Code); retrying"; Start-Sleep 2; continue }
        $device = $created.Text | ConvertFrom-Json
        $devices.Add($device); $stats.Created++
        $adapter = Find-Interface $device 20
        if ($null -eq $adapter) {
            # ClInitialize failed under injection: the device exists without an interface.
            $stats.NeverStarted++; Log "cycle $($stats.Cycles): adapter never started"
        } else {
            $fuzzOut = Join-Path $output.FullName "fuzz-$($stats.Cycles).out"
            $process = Start-Process $fuzz -ArgumentList @($adapter.ifIndex, 15, '--tolerant') -PassThru -NoNewWindow `
                -RedirectStandardOutput $fuzzOut -RedirectStandardError ($fuzzOut -replace '\.out$', '.err')
            $null = $process.Handle   # PowerShell 5.1 reports no ExitCode unless the handle was taken while running
            $toggleEnd = (Get-Date).AddSeconds(12)
            while ((Get-Date) -lt $toggleEnd -and !$process.HasExited) {
                foreach ($verb in 'Disable', 'Enable') {
                    $stats.Toggles++
                    $ok = Invoke-Bounded { param($index, $v)
                        $a = Get-NetAdapter -IncludeHidden | Where-Object ifIndex -eq $index
                        if ($a) { if ($v -eq 'Disable') { Disable-NetAdapter -InputObject $a -Confirm:$false } else { Enable-NetAdapter -InputObject $a -Confirm:$false } }
                    } @($adapter.ifIndex, $verb) 90 "$verb of interface $($adapter.ifIndex)"
                    if (!$ok) { $stats.ToggleFailures++ }
                }
                Start-Sleep -Milliseconds 500
            }
            if (!$process.WaitForExit(120000)) { $process.Kill(); throw "HANG: fuzz on interface $($adapter.ifIndex) did not finish" }
            $text = Get-Content -LiteralPath $fuzzOut -Raw -ErrorAction SilentlyContinue
            if ($process.ExitCode -ne 0) {
                $err = Get-Content -LiteralPath ($fuzzOut -replace '\.out$', '.err') -Raw -ErrorAction SilentlyContinue
                throw "fuzz exited $($process.ExitCode): $err"
            }
            if ($text -match 'SKIP') { $stats.FuzzSkipped++ } else { $stats.FuzzRuns++ }
        }
        Remove-TestDevice $device
    }
    $stats | ConvertTo-Json | Set-Content (Join-Path $output.FullName 'stats.json') -Encoding UTF8
    Log ("PASS low resources: {0} cycles, {1} adapters never started, {2} fuzz runs ({3} skipped), {4} toggles ({5} failed)" -f `
        $stats.Cycles, $stats.NeverStarted, $stats.FuzzRuns, $stats.FuzzSkipped, $stats.Toggles, $stats.ToggleFailures)
    $exitCode = 0
} catch { Write-Warning $_.Exception.Message; if ($output) { Log "FAIL $($_.Exception.Message)" | Out-Null } }
finally {
    foreach ($device in @($devices)) {
        try { Remove-TestDevice $device } catch { Write-Warning "Cleanup failed for $($device.InstanceId): $_"; $exitCode = 1 }
    }
}
exit $exitCode
