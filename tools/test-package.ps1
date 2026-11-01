#requires -Version 5.1
<#
Adapter-management paths through chmlink.dll with its embedded driver, as
applications ship it. Run elevated in the disposable test VM, after the CI has
installed the candidate on its anchor adapter.

  package     the embedded package is newer than what runs: kept while an adapter has a
              data session open, upgraded once idle; a mix of packages (an upgrade that
              stopped halfway) is repaired, or refused with ERROR_BUSY while busy
  driver      chmlink_delete_driver removes unused packages and refuses the ones in use
  concurrent  several processes create adapters at once: unique names win, a shared
              name is created once, nothing is left half-made
  surprise    the adapter is surprise-removed under an open session (test filter driver)

-OldInfPath is the candidate re-signed with a lower DriverVer, at least the SDK's minimum.
-FilterPath is the signed chmlink-surprise.sys; without it the surprise part is skipped.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OldInfPath,
    [Parameter(Mandatory)][string]$EmbeddedDirectory,   # chmlink-package-test.exe with chmlink.dll
    [Parameter(Mandatory)][string]$BuildDirectory,      # chmlink-device.exe
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$FilterPath,
    [string[]]$Parts = @('package', 'driver', 'concurrent', 'surprise')
)
$ErrorActionPreference = 'Stop'
$ERROR_BUSY = 170; $ERROR_DUP_NAME = 52; $ERROR_INF_IN_USE = 3758096957   # 0xE000023D
$owner = '{5F1C2A7E-3B4D-4E8F-9A10-6C2D7E41883B}'
$filterService = 'chmlinksurprise'
$created = [Collections.Generic.List[string]]::new()
$holders = [Collections.Generic.List[object]]::new()
$exitCode = 1

function Log([string]$Text) {
    $line = '[{0:HH:mm:ss}] {1}' -f (Get-Date), $Text
    Write-Output $line
    Add-Content -LiteralPath (Join-Path $output.FullName 'package.log') -Value $line -Encoding UTF8
}
function Check([bool]$Condition, [string]$What) { if (!$Condition) { throw "FAILED: $What" } }

# One chmlink-package-test command: its JSON line, the SDK log and the exit code.
function Invoke-Test([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($packageTest, (($Arguments | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }) -join ' '))
    $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true; $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($start)
    $errors = $process.StandardError.ReadToEndAsync()
    $text = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit(); $sdkLog = $errors.Result; $code = $process.ExitCode; $process.Dispose()
    $json = $null
    try { $json = ($text.Trim() -split "`r?`n" | Select-Object -Last 1) | ConvertFrom-Json } catch { }
    if ($null -eq $json) { throw "chmlink-package-test $($Arguments -join ' ') printed no result: $text $sdkLog" }
    Add-Content -LiteralPath (Join-Path $output.FullName 'package.log') -Encoding UTF8 -Value (
        "  > $($Arguments -join ' ')`n  < $($text.Trim())`n" + (($sdkLog -split "`r?`n" | Where-Object { $_ } | ForEach-Object { "    $_" }) -join "`n"))
    [pscustomobject]@{ Json = $json; Log = [string]$sdkLog; Code = $code }
}
function New-Adapter([string]$Name) {
    $r = Invoke-Test @('create', $Name)
    Check ($r.Json.ok -eq 1) "creating '$Name' (error $($r.Json.error))"
    Check (($r.Json.flags -band 1) -eq 0) "'$Name' needs a restart"
    $created.Add($r.Json.instance)
    $r
}
function Remove-Adapter([string]$Instance) {
    $r = Invoke-Test @('delete', $Instance)
    Check ($r.Json.ok -eq 1) "deleting $Instance (error $($r.Json.error))"
    [void]$created.Remove($Instance)
}
function Get-Adapters { @(Get-PnpDevice -Class Net -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -match 'Root\\ChmLink' }) }
function Get-Version([string]$Instance) { [string](Get-PnpDeviceProperty -InstanceId $Instance -KeyName DEVPKEY_Device_DriverVersion).Data }
function Get-Index([string]$Instance) {
    $deadline = (Get-Date).AddSeconds(20)
    do {
        $adapter = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $Instance)
        if ($adapter.Count -eq 1 -and $adapter[0].InterfaceIndex) { return [uint32]$adapter[0].InterfaceIndex }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    throw "No interface for $Instance"
}
function Install-Old([string]$Instance) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $deviceTool install $Instance $OldInfPath | Out-Null; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
    Check ($code -eq 0) "installing the older package on $Instance (exit $code)"
    Check ((Get-Version $Instance) -eq $oldVersion) "$Instance runs $oldVersion"
}
function Assert-Versions([string[]]$Instances, [string]$Version, [string]$When) {
    foreach ($instance in $Instances) {
        $actual = Get-Version $instance
        Check ($actual -eq $Version) "$When`: $instance runs $actual, expected $Version"
    }
}
# A process that keeps a data session open and checks it every 100 ms.
function Start-Holder([string]$Instance) {
    $id = [guid]::NewGuid().ToString('N')
    $ready = [Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\ChmLinkHoldReady-$id")
    $stop = [Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\ChmLinkHoldStop-$id")
    $start = [Diagnostics.ProcessStartInfo]::new($packageTest, "hold $(Get-Index $Instance) Local\ChmLinkHoldReady-$id Local\ChmLinkHoldStop-$id")
    $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true; $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($start)
    $holder = [pscustomobject]@{ Process = $process; Ready = $ready; Stop = $stop; Instance = $Instance }
    $holders.Add($holder)
    if (!$ready.WaitOne(15000)) { throw "The session holder on $Instance did not start: $($process.StandardOutput.ReadToEnd())" }
    Check (!$process.HasExited) "the session holder on $Instance is running"
    $holder
}
function Stop-Holder($Holder) {
    [void]$Holder.Stop.Set()
    if (!$Holder.Process.WaitForExit(15000)) { $Holder.Process.Kill(); throw 'The session holder did not stop.' }
    $text = $Holder.Process.StandardOutput.ReadToEnd().Trim()
    [void]$holders.Remove($Holder); $Holder.Ready.Dispose(); $Holder.Stop.Dispose()
    $json = $text | ConvertFrom-Json
    Check ($json.ok -eq 1) "the data session on $($Holder.Instance) kept working ($text)"
    Log "  the session on $($Holder.Instance) kept working: $($json.queries) queries"
}
function Get-StorePackages {
    @(Get-ChildItem "$env:windir\INF\oem*.inf" | Where-Object { Select-String -LiteralPath $_.FullName -Pattern '^\s*chmlink\.sys\s*=' -Quiet } |
        ForEach-Object {
            $version = (Select-String -LiteralPath $_.FullName -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1).Matches[0].Groups[1].Value
            [pscustomobject]@{ Inf = $_.Name; Version = $version }
        })
}

function Test-Package {
    Log 'package: keep while busy, upgrade when idle, repair a mix'
    $a = (New-Adapter 'ChmLink Pkg A').Json.instance
    Assert-Versions @($anchor, $a) $candidateVersion 'after creating A'
    Install-Old $anchor; Install-Old $a
    Log "  anchor and A run the older $oldVersion"

    $holder = Start-Holder $a
    $b = New-Adapter 'ChmLink Pkg B'
    Check ($b.Log -match 'Keeping driver') 'creating B with a session open on A keeps the running package'
    Assert-Versions @($anchor, $a, $b.Json.instance) $oldVersion 'B created while A is busy'
    Stop-Holder $holder
    Log '  busy: B was created on the running package and the session survived'

    $c = New-Adapter 'ChmLink Pkg C'
    Check ($c.Log -match 'Moving an existing adapter') 'creating C when idle upgrades the others'
    Assert-Versions @($anchor, $a, $b.Json.instance, $c.Json.instance) $candidateVersion 'C created when idle'
    Log '  idle: every adapter was upgraded to the embedded package'

    Install-Old $b.Json.instance
    $d = New-Adapter 'ChmLink Pkg D'
    Assert-Versions @($anchor, $a, $b.Json.instance, $c.Json.instance, $d.Json.instance) $candidateVersion 'D created over a mix'
    Log '  mix: the adapter left on the older package was upgraded'

    Install-Old $b.Json.instance
    $holder = Start-Holder $b.Json.instance
    $before = @(Get-Adapters).Count
    $e = Invoke-Test @('create', 'ChmLink Pkg E')
    Check ($e.Json.ok -eq 0 -and $e.Json.error -eq $ERROR_BUSY) "creating over a busy mix fails with ERROR_BUSY (got ok=$($e.Json.ok) error=$($e.Json.error))"
    Check (@(Get-Adapters).Count -eq $before) 'the refused creation left no adapter behind'
    Assert-Versions @($b.Json.instance) $oldVersion 'after the refused creation'
    Stop-Holder $holder
    $e = New-Adapter 'ChmLink Pkg E'
    Assert-Versions @($b.Json.instance, $e.Json.instance) $candidateVersion 'E created once idle'
    Log '  busy mix: refused with ERROR_BUSY, nothing changed; created once idle'

    foreach ($instance in @($created)) { Remove-Adapter $instance }
}

function Test-Driver {
    Log 'driver: chmlink_delete_driver'
    $before = @(Get-StorePackages)
    $bound = @(Get-Adapters | ForEach-Object { [string](Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data })
    $r = Invoke-Test @('delete-driver')
    Check ($r.Json.ok -eq 0 -and $r.Json.error -eq $ERROR_INF_IN_USE) "with an adapter left it fails with ERROR_INF_IN_USE_BY_DEVICES (got ok=$($r.Json.ok) error=$($r.Json.error))"
    $after = @(Get-StorePackages)
    foreach ($package in $before) {
        $kept = @($after | Where-Object Inf -eq $package.Inf).Count -gt 0
        if ($bound -contains $package.Inf) { Check $kept "the package in use $($package.Inf) is kept" }
        else { Check (!$kept) "the unused package $($package.Inf) ($($package.Version)) is removed" }
    }
    Check ((Get-PnpDevice -InstanceId $anchor).Status -eq 'OK') 'the remaining adapter still works'
    Log ("  removed {0} unused package(s), kept {1} in use" -f ($before.Count - $after.Count), $after.Count)
}

function Test-Concurrent {
    Log 'concurrent: 8 processes create at once (6 names, and one name twice)'
    $names = @(1..6 | ForEach-Object { "ChmLink Par $_" }) + @('ChmLink Par Same', 'ChmLink Par Same')
    $before = @(Get-Adapters).Count
    $processes = foreach ($name in $names) {
        $start = [Diagnostics.ProcessStartInfo]::new($packageTest, "create `"$name`"")
        $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true; $start.CreateNoWindow = $true
        [pscustomobject]@{ Name = $name; Process = [Diagnostics.Process]::Start($start) }
    }
    $results = foreach ($p in $processes) {
        if (!$p.Process.WaitForExit(180000)) { $p.Process.Kill(); throw "creating '$($p.Name)' did not finish" }
        $text = $p.Process.StandardOutput.ReadToEnd().Trim(); $errText = $p.Process.StandardError.ReadToEnd().Trim()
        Add-Content -LiteralPath (Join-Path $output.FullName 'package.log') -Value "  $($p.Name): $text`n    $errText" -Encoding UTF8
        $json = $text | ConvertFrom-Json
        if ($json.ok -eq 1) { $created.Add($json.instance) }
        [pscustomobject]@{ Name = $p.Name; Ok = $json.ok; Error = $json.error; Instance = $json.instance }
    }
    foreach ($r in $results | Where-Object Name -ne 'ChmLink Par Same') { Check ($r.Ok -eq 1) "'$($r.Name)' was created (error $($r.Error))" }
    $same = @($results | Where-Object Name -eq 'ChmLink Par Same')
    Check (@($same | Where-Object Ok -eq 1).Count -eq 1) 'the shared name was created exactly once'
    Check (@($same | Where-Object { $_.Ok -eq 0 -and $_.Error -eq $ERROR_DUP_NAME }).Count -eq 1) 'the second creation of the shared name failed with ERROR_DUP_NAME'
    Check (@(Get-Adapters).Count -eq $before + 7) 'exactly 7 adapters were added'
    foreach ($r in $results | Where-Object Ok -eq 1) {
        $alias = (Get-NetAdapter -IncludeHidden | Where-Object PnPDeviceID -eq $r.Instance).Name
        Check ($alias -eq $r.Name) "$($r.Instance) is named '$($r.Name)' (is '$alias')"
        Check ((Get-PnpDevice -InstanceId $r.Instance).Status -eq 'OK') "$($r.Instance) started"
    }
    foreach ($instance in @($created)) { Remove-Adapter $instance }
    Check (@(Get-Adapters).Count -eq $before) 'all of them were deleted'
    Log '  7 adapters created with the right names, the duplicate refused, all deleted'
}

function Test-Surprise {
    if (!$FilterPath) { Log 'surprise: skipped (no -FilterPath)'; return }
    Log 'surprise: surprise removal under an open session'
    $f = (New-Adapter 'ChmLink Surprise').Json.instance
    & sc.exe create $filterService type= kernel start= demand binPath= "$FilterPath" | Out-Null
    Check ($LASTEXITCODE -eq 0) 'the test filter service was registered'
    $r = Invoke-Test @('filter', $f, $filterService)
    Check ($r.Json.ok -eq 1) "the test filter was attached (error $($r.Json.error))"
    $index = Get-Index $f
    $id = [guid]::NewGuid().ToString('N')
    $ready = [Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\ChmLinkSurprise-$id")
    try {
        $start = [Diagnostics.ProcessStartInfo]::new($packageTest, "surprise-wait $index Local\ChmLinkSurprise-$id")
        $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true; $start.CreateNoWindow = $true
        $waiter = [Diagnostics.Process]::Start($start)
        Check $ready.WaitOne(15000) 'the session waiter started'
        $r = Invoke-Test @('surprise-trigger')
        Check ($r.Json.ok -eq 1) "the surprise removal was triggered (error $($r.Json.error))"
        if (!$waiter.WaitForExit(90000)) { $waiter.Kill(); throw 'FAILED: the session did not end after the surprise removal' }
        $text = $waiter.StandardOutput.ReadToEnd().Trim()
        Add-Content -LiteralPath (Join-Path $output.FullName 'package.log') -Value "  waiter: $text" -Encoding UTF8
        Check (($text | ConvertFrom-Json).ok -eq 1) "the session failed cleanly ($text)"
    } finally { $ready.Dispose() }
    $deadline = (Get-Date).AddSeconds(30)
    do { $device = Get-PnpDevice -InstanceId $f; Start-Sleep -Milliseconds 250 } while ($device.Status -eq 'OK' -and (Get-Date) -lt $deadline)
    $problem = (Get-PnpDeviceProperty -InstanceId $f -KeyName DEVPKEY_Device_ProblemCode).Data
    Log "  after the removal: status $($device.Status), problem code $problem"
    Check ($device.Status -ne 'OK') 'Windows took the adapter down'
    Remove-Adapter $f
    & sc.exe delete $filterService | Out-Null
    $g = (New-Adapter 'ChmLink Surprise After').Json.instance
    Check ((Get-PnpDevice -InstanceId $g).Status -eq 'OK') 'a new adapter works after the surprise removal'
    Remove-Adapter $g
    Check ((Get-PnpDevice -InstanceId $anchor).Status -eq 'OK') 'the anchor adapter was not affected'
    Log '  the session failed promptly, the adapter was deleted, a new one works'
}

try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in an elevated disposable test VM.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    $packageTest = Join-Path (Resolve-Path $EmbeddedDirectory).Path 'chmlink-package-test.exe'
    $deviceTool = Join-Path (Resolve-Path $BuildDirectory).Path 'chmlink-device.exe'
    foreach ($file in $packageTest, $deviceTool, $OldInfPath) { if (!(Test-Path -LiteralPath $file)) { throw "Missing $file" } }
    $OldInfPath = (Resolve-Path $OldInfPath).Path
    $oldVersion = (Select-String -LiteralPath $OldInfPath -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1).Matches[0].Groups[1].Value
    $existing = @(Get-Adapters)
    if ($existing.Count -ne 1) { throw "Expected only the CI anchor adapter, found $($existing.Count) ChmLink adapters." }
    $anchor = $existing[0].InstanceId
    $candidateVersion = Get-Version $anchor
    Log "anchor $anchor runs $candidateVersion; older package $oldVersion"
    if ($Parts -contains 'driver') { Test-Driver }     # first: clears leftover packages of the older version
    if ($Parts -contains 'package') { Test-Package }
    if ($Parts -contains 'concurrent') { Test-Concurrent }
    if ($Parts -contains 'surprise') { Test-Surprise }
    if ($Parts -contains 'driver') { Test-Driver }     # again: the older package is unused now and goes
    Log 'PASS package tests'
    $exitCode = 0
} catch {
    Write-Warning $_.Exception.Message
    if ($output) { Log "FAIL $($_.Exception.Message)" }
} finally {
    foreach ($holder in @($holders)) { try { [void]$holder.Stop.Set(); if (!$holder.Process.WaitForExit(10000)) { $holder.Process.Kill() } } catch { } }
    foreach ($instance in @($created)) {
        if (Get-PnpDevice -InstanceId $instance -ErrorAction SilentlyContinue) {
            try { Remove-Adapter $instance } catch { Write-Warning "Cleanup failed for $instance`: $_"; $exitCode = 1 }
        }
    }
    if (Get-Service $filterService -ErrorAction SilentlyContinue) { & sc.exe delete $filterService | Out-Null }
    if ($anchor -and (Get-Version $anchor) -ne $candidateVersion) {
        Write-Warning "The anchor adapter was left on $(Get-Version $anchor); the CI restore puts the baseline back."
    }
}
exit $exitCode
