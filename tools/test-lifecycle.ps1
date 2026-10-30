#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$BuildDirectory = "$PSScriptRoot/../build/verify/Release",
    [string]$OutputDirectory = "$PSScriptRoot/../build/lifecycle-results"
)
$ErrorActionPreference = 'Stop'
$owner = [guid]'9AB3CDB8-3693-4C1F-89B3-4062BB57BE83'
$devices = @(); $process = $null; $ready = $null
$exitCode = 1
$createdNeedsRestart = $false
function Run-Device([string[]]$Arguments) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $output = & $tool @Arguments; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    if ($code -eq 3010 -and $Arguments[0] -eq 'create') {
        $script:createdNeedsRestart = $true
    } elseif ($code -ne 0) { throw "Device tool exited $code; inspect the inventory before retrying." }
    return $output
}
function Get-TestAdapter($Device) {
    $identity = Run-Device @('show', $Device.InstanceId) | ConvertFrom-Json
    if (!$identity.Managed -or [guid]$identity.AdapterId -ne [guid]$Device.AdapterId -or
        [guid]$identity.OwnerId -ne $owner) { throw 'Test device identity changed.' }
    $windows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $Device.InstanceId)
    if ($windows.Count -ne 1) { throw 'Cannot identify test interface.' }
    $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
    if ($adapter.Count -ne 1) { throw 'Cannot resolve test interface.' }
    return $adapter[0]
}
function Start-Test([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $test; $start.Arguments = $Arguments -join ' '
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $child = [Diagnostics.Process]::new(); $child.StartInfo = $start
    if (!$child.Start()) { $child.Dispose(); throw 'Cannot start test process.' }
    return $child
}
function Wait-Test($Process, [string]$Label) {
    $completed = $Process.WaitForExit(120000)
    if (!$completed) {
        $Process.Kill()
        if (!$Process.WaitForExit(10000)) { throw 'Test process did not terminate; inspect the VM.' }
    }
    $Process.StandardOutput.ReadToEnd() | Set-Content (Join-Path $output.FullName "$Label.out")
    $Process.StandardError.ReadToEnd() | Set-Content (Join-Path $output.FullName "$Label.err")
    if (!$completed) { throw 'Lifecycle test exceeded its deadline.' }
    if ($Process.ExitCode -ne 0) { throw "Lifecycle test exited $($Process.ExitCode); inspect its logs." }
}
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in an elevated disposable test VM.' }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $tool = Join-Path $BuildDirectory 'chmlink-device.exe'
    $test = Join-Path $BuildDirectory 'chmlink-lifecycle-test.exe'
    if (!(Test-Path -LiteralPath $test)) { throw 'Build chmlink-lifecycle-test first.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    $InfPath = (Resolve-Path -LiteralPath $InfPath).Path
    foreach ($suffix in @('A', 'B')) {
        # Native creation returns the identity before any network alias work.
        $device = Run-Device @('create', $InfPath, '--owner', $owner.ToString('B'),
            '--name', "ChmLink Lifecycle $suffix") | ConvertFrom-Json
        $devices += $device
        $devices | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output.FullName 'devices.json') -Encoding UTF8
        if ($createdNeedsRestart) { throw 'Device installation requires a restart; stopping before lifecycle checks.' }
    }
    $first = Get-TestAdapter $devices[0]; $second = Get-TestAdapter $devices[1]
    foreach ($ip in @('169.254.250.1', '169.254.251.1')) {
        if (Get-NetIPAddress -IPAddress $ip -ErrorAction SilentlyContinue) { throw 'Test IP already exists.' }
    }
    Set-NetIPInterface -InterfaceIndex $first.ifIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    Set-NetIPInterface -InterfaceIndex $second.ifIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    New-NetIPAddress -InterfaceIndex $first.ifIndex -IPAddress '169.254.250.1' -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
    New-NetIPAddress -InterfaceIndex $second.ifIndex -IPAddress '169.254.251.1' -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
    $process = Start-Test @($first.ifIndex, $second.ifIndex)
    Wait-Test $process 'basic'; $process.Dispose(); $process = $null
    foreach ($transition in @('disable', 'remove')) {
        $eventName = 'Local\ChmLinkTransition-' + [guid]::NewGuid().ToString('N')
        $ready = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $eventName)
        $first = Get-TestAdapter $devices[0]
        $process = Start-Test @('--transition', $first.ifIndex, $eventName)
        if (!$ready.WaitOne(15000)) { throw 'Test waiter did not become ready.' }
        $first = Get-TestAdapter $devices[0] # recheck before modifying this exact instance
        if ($transition -eq 'disable') {
            Disable-NetAdapter -InputObject $first -Confirm:$false
        } else {
            Run-Device @('remove', $devices[0].InstanceId, '--owner', $owner.ToString('B')) | Out-Host
        }
        Wait-Test $process $transition; $process.Dispose(); $process = $null
        $ready.Dispose(); $ready = $null
        if ($transition -eq 'disable') { Enable-NetAdapter -InputObject $first -Confirm:$false }
        $null = Get-TestAdapter $devices[1]
    }
    'PASS lifecycle tests; results: ' + $output.FullName
    $exitCode = 0
} catch { Write-Warning $_.Exception.Message }
finally {
    if ($null -ne $process) {
        try {
            if (!$process.HasExited) { $process.Kill(); $process.WaitForExit(10000) | Out-Null }
        } catch { Write-Warning "Test process cleanup failed: $_"; $exitCode = 1 }
        finally { $process.Dispose() }
    }
    if ($null -ne $ready) { $ready.Dispose() }
    foreach ($device in $devices) {
        if (Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue) {
            try {
                Run-Device @('remove', $device.InstanceId, '--owner', $owner.ToString('B')) | Out-Host
            } catch { Write-Warning "Cleanup failed for $($device.InstanceId): $_"; $exitCode = 1 }
        }
    }
}
exit $exitCode
