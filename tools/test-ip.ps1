#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$BuildDirectory = "$PSScriptRoot/../build/verify/Release",
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$owner = [guid]'A9206A62-5C94-4CBC-B23E-FB3A891DE058'
$devices = @(); $process = $null; $exitCode = 1
function Run-Device([string[]]$Arguments) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $output = & $tool @Arguments; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    if ($code -notin @(0, 3010)) { throw "Device tool exited $code; inspect the inventory before retrying." }
    $script:deviceRestart = $code -eq 3010
    return $output
}
function Get-TestAdapter($Device) {
    $current = Run-Device @('show', $Device.InstanceId) | ConvertFrom-Json
    if (!$current.Managed -or [guid]$current.AdapterId -ne [guid]$Device.AdapterId -or
        [guid]$current.OwnerId -ne $owner -or $current.Mode -ne $Device.Mode) { throw 'Test device identity changed.' }
    $windows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $Device.InstanceId)
    if ($windows.Count -ne 1 -or !$windows[0].GUID) { throw 'Test interface is not ready.' }
    $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
    if ($adapter.Count -ne 1) { throw 'Cannot resolve the unique test adapter.' }
    return $adapter[0]
}
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in an elevated disposable test VM.' }
    $build = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $tool = Join-Path $build 'chmlink-device.exe'; $test = Join-Path $build 'chmlink-ip-test.exe'
    if (!(Test-Path -LiteralPath $tool) -or !(Test-Path -LiteralPath $test)) { throw 'Build the test and device tool first.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    foreach ($ip in @('192.0.2.1', '192.0.2.2', '2001:db8:2026::1', '2001:db8:2026::2')) {
        if (Get-NetIPAddress -IPAddress $ip -ErrorAction SilentlyContinue) { throw "Test address already assigned: $ip" }
    }
    $inf = (Resolve-Path -LiteralPath $InfPath).Path
    foreach ($mode in @('ethernet', 'ip')) {
        $device = Run-Device @('create', $inf, '--owner', $owner.ToString('B'),
            '--mode', $mode, '--name', "ChmLink $mode Integration Test") | ConvertFrom-Json
        $devices += $device
        $devices | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output.FullName 'devices.json') -Encoding UTF8
        if ($deviceRestart) { throw 'Device installation requires a restart; stopping before test.' }
        if (!$device.Managed -or $device.Mode -ne $(if ($mode -eq 'ip') { 2 } else { 1 }) -or
            [guid]$device.OwnerId -ne $owner) { throw 'Unexpected device identity.' }
    }
    $ethernet = Get-TestAdapter $devices[0]; $adapter = Get-TestAdapter $devices[1]
    Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv6 -Dhcp Disabled -PolicyStore ActiveStore
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress '192.0.2.1' -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress '2001:db8:2026::1' -PrefixLength 64 -PolicyStore ActiveStore | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $test
    $start.Arguments = "$($adapter.ifIndex) 192.0.2.1 192.0.2.2 2001:db8:2026::1 2001:db8:2026::2 $($ethernet.ifIndex)"
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    if (!$process.Start()) { throw 'Cannot start IP test.' }
    $completed = $process.WaitForExit(60000)
    if (!$completed) {
        $process.Kill()
        if (!$process.WaitForExit(10000)) { throw 'Test process did not terminate; inspect the VM.' }
    }
    $process.StandardOutput.ReadToEnd() | Set-Content (Join-Path $output.FullName 'ip.out')
    $process.StandardError.ReadToEnd() | Set-Content (Join-Path $output.FullName 'ip.err')
    if (!$completed -or $process.ExitCode -ne 0) { throw 'IP roundtrip did not pass; inspect the logs.' }
    'PASS IPv4 and IPv6 IP adapter roundtrips and Ethernet isolation'; $exitCode = 0
} catch { Write-Warning $_.Exception.Message }
finally {
    if ($null -ne $process) {
        try { if (!$process.HasExited) { $process.Kill(); $process.WaitForExit(10000) | Out-Null } }
        catch { Write-Warning "Test process cleanup failed: $_"; $exitCode = 1 }
        finally { $process.Dispose() }
    }
    [array]::Reverse($devices)
    foreach ($device in $devices) {
        if (!(Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue)) { continue }
        try {
            $current = Run-Device @('show', $device.InstanceId) | ConvertFrom-Json
            if ([guid]$current.AdapterId -ne [guid]$device.AdapterId -or [guid]$current.OwnerId -ne $owner -or $current.Mode -ne $device.Mode) {
                throw 'Device identity changed; refusing cleanup.'
            }
            Run-Device @('remove', $device.InstanceId, '--owner', $owner.ToString('B')) | Out-Host
            if ($deviceRestart) { throw 'Device removal requires a restart.' }
        } catch { Write-Warning "Cleanup failed for $($device.InstanceId): $_"; $exitCode = 1 }
    }
}
exit $exitCode
