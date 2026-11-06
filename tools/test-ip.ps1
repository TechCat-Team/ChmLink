#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/verify/Release' }
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
    # Creation names the connection itself.
    if ($adapter[0].Name -cne $current.Name) { throw "The connection is named '$($adapter[0].Name)', not '$($current.Name)'." }
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
    # The IP adapter runs with a non-default MTU, so the configured value, not a
    # constant, has to reach NDIS, QUERY and the stack.
    # The Ethernet adapter runs below the default, as a tunnel would.
    $ipMtu = 9000; $ethernetMtu = 1400
    foreach ($mode in @('ethernet', 'ip')) {
        $create = @('create', $inf, '--owner', $owner.ToString('B'), '--mode', $mode, '--name', "ChmLink $mode Integration Test",
                    '--mtu', [string]$(if ($mode -eq 'ip') { $ipMtu } else { $ethernetMtu }))
        $device = Run-Device $create | ConvertFrom-Json
        $devices += $device
        $devices | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output.FullName 'devices.json') -Encoding UTF8
        if ($deviceRestart) { throw 'Device installation requires a restart; stopping before test.' }
        if (!$device.Managed -or $device.Mode -ne $(if ($mode -eq 'ip') { 2 } else { 1 }) -or
            [guid]$device.OwnerId -ne $owner) { throw 'Unexpected device identity.' }
        if ($device.Mtu -ne $(if ($mode -eq 'ip') { $ipMtu } else { $ethernetMtu })) { throw "Unexpected stored MTU $($device.Mtu)." }
    }
    $ethernet = Get-TestAdapter $devices[0]; $adapter = Get-TestAdapter $devices[1]
    Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv6 -Dhcp Disabled -PolicyStore ActiveStore
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress '192.0.2.1' -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress '2001:db8:2026::1' -PrefixLength 64 -PolicyStore ActiveStore | Out-Null
    foreach ($family in 'IPv4', 'IPv6') {
        $stackMtu = (Get-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily $family).NlMtu
        if ($stackMtu -ne $ipMtu) { throw "The $family interface MTU is $stackMtu, not the configured $ipMtu." }
    }
    # Windows sees the Ethernet MTU too, for both families.
    foreach ($family in 'IPv4', 'IPv6') {
        $stackMtu = (Get-NetIPInterface -InterfaceIndex $ethernet.ifIndex -AddressFamily $family).NlMtu
        if ($stackMtu -ne $ethernetMtu) { throw "The Ethernet adapter's $family MTU is $stackMtu, not the configured $ethernetMtu." }
    }
    $env:CHMLINK_TEST_MTU = [string]$ipMtu; $env:CHMLINK_TEST_ETHERNET_MTU = [string]$ethernetMtu
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
    $process.Dispose(); $process = $null

    # Changing the MTU of an existing adapter: stored, applied by a restart.
    $changed = Run-Device @('set-mtu', $devices[1].InstanceId, '1280', '--owner', $owner.ToString('B')) | ConvertFrom-Json
    if ($changed.Mtu -ne 1280 -or [guid]$changed.AdapterId -ne [guid]$devices[1].AdapterId) { throw 'set-mtu did not store 1280.' }
    $deadline = (Get-Date).AddSeconds(30); $stackMtu = 0
    while ((Get-Date) -lt $deadline) {
        $interface = Get-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv6 -ErrorAction SilentlyContinue
        if ($interface) { $stackMtu = $interface.NlMtu; if ($stackMtu -eq 1280) { break } }
        Start-Sleep -Milliseconds 250
    }
    if ($stackMtu -ne 1280) { throw "After set-mtu the IPv6 interface MTU is $stackMtu, not 1280." }
    # An Ethernet adapter goes up to jumbo frames; 9001 is refused and changes nothing.
    $changed = Run-Device @('set-mtu', $devices[0].InstanceId, '9000', '--owner', $owner.ToString('B')) | ConvertFrom-Json
    if ($changed.Mtu -ne 9000) { throw 'set-mtu did not store 9000 on the Ethernet adapter.' }
    $deadline = (Get-Date).AddSeconds(30); $stackMtu = 0
    while ((Get-Date) -lt $deadline) {
        $interface = Get-NetIPInterface -InterfaceIndex $ethernet.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
        if ($interface) { $stackMtu = $interface.NlMtu; if ($stackMtu -eq 9000) { break } }
        Start-Sleep -Milliseconds 250
    }
    if ($stackMtu -ne 9000) { throw "After set-mtu the Ethernet adapter's IPv4 MTU is $stackMtu, not 9000." }
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & $tool set-mtu $devices[0].InstanceId 9001 --owner $owner.ToString('B') 2>$null | Out-Null
    $refused = $LASTEXITCODE; $ErrorActionPreference = $previous
    if ($refused -eq 0) { throw 'set-mtu accepted 9001 on an Ethernet adapter.' }
    if ((Run-Device @('show', $devices[0].InstanceId) | ConvertFrom-Json).Mtu -ne 9000) { throw 'The refused Ethernet MTU changed the stored one.' }
    'PASS IPv4 and IPv6 IP adapter roundtrips (MTU 9000, full-size datagrams), Ethernet MTU 1400 and 9000, isolation and set-mtu'; $exitCode = 0
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
