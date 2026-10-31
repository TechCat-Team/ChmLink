#requires -Version 5.1
# Opt-in data-path benchmark for the disposable test VM. Creates one owned
# IP-mode adapter, assigns 198.18.0.1/24 (benchmarking range), runs
# chmlink-bench against simulated peer 198.18.0.2, then chmlink-tcp-bench
# (TCP through the stack, with 198.18.0.3 as the reflected alias). Then an
# Ethernet-mode adapter on 198.18.1.0/24 runs chmlink-tcp-bench without and
# with task offloads (Windows uses them in Ethernet mode only). Adapters are
# removed at the end. -Filter tcp runs the TCP part only.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [double]$Seconds = 5,
    [string]$Filter = ''
)
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/verify/Release' }
$ErrorActionPreference = 'Stop'
$owner = [guid]'3F6B2D7E-91A4-4C58-B0E2-7A1D5C9E4B36'
$local = '198.18.0.1'; $peer = '198.18.0.2'; $alias = '198.18.0.3'
# The Ethernet-mode adapter for the offload TCP benchmark gets its own subnet.
$ethLocal = '198.18.1.1'; $ethPeer = '198.18.1.2'; $ethAlias = '198.18.1.3'
# An IP adapter with the largest MTU, on a third subnet.
$bigLocal = '198.18.2.1'; $bigPeer = '198.18.2.2'; $bigAlias = '198.18.2.3'
$devices = @(); $exitCode = 1
function Run-Device([string[]]$Arguments) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $output = & $tool @Arguments; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    if ($code -notin @(0, 3010)) { throw "Device tool exited $code; inspect the inventory before retrying." }
    $script:deviceRestart = $code -eq 3010
    return $output
}
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in an elevated disposable test VM.' }
    $build = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $tool = Join-Path $build 'chmlink-device.exe'; $bench = Join-Path $build 'chmlink-bench.exe'
    if (!(Test-Path -LiteralPath $tool) -or !(Test-Path -LiteralPath $bench)) { throw 'Build chmlink-bench and the device tool first.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    foreach ($ip in @($local, $peer, $alias, $ethLocal, $ethPeer, $ethAlias, $bigLocal, $bigPeer, $bigAlias)) {
        if (Get-NetIPAddress -IPAddress $ip -ErrorAction SilentlyContinue) { throw "Benchmark address already assigned: $ip" }
    }
    $inf = (Resolve-Path -LiteralPath $InfPath).Path
    $device = Run-Device @('create', $inf, '--owner', $owner.ToString('B'), '--mode', 'ip', '--name', 'ChmLink Bench') | ConvertFrom-Json
    $devices += $device
    if ($deviceRestart) { throw 'Device installation requires a restart; stopping before the benchmark.' }
    $windows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $device.InstanceId)
    if ($windows.Count -ne 1 -or !$windows[0].GUID) { throw 'Test interface is not ready.' }
    $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
    if ($adapter.Count -ne 1) { throw 'Cannot resolve the unique test adapter.' }
    $index = $adapter[0].ifIndex
    Set-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    New-NetIPAddress -InterfaceIndex $index -IPAddress $local -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
    [ordered]@{
        Os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
        Cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        Logical = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
        Memory = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
        DriverVersion = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
        Verifier = ((verifier /querysettings 2>&1 | Select-Object -First 1) -join '')
        Started = (Get-Date).ToString('s')
    } | ConvertTo-Json | Set-Content (Join-Path $output.FullName 'environment.json') -Encoding UTF8
    Start-Sleep -Seconds 3   # let address registration and IPv6 neighbor chatter settle
    function Invoke-Bench([string]$Program, [string]$Arguments, [string]$Label, [int]$DeadlineSeconds) {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $Program; $start.Arguments = $Arguments
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
        if (!$process.Start()) { throw "Cannot start $Label." }
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($DeadlineSeconds * 1000)) { $process.Kill(); throw "$Label exceeded its deadline." }
        $process.WaitForExit()
        $stdout.Result | Add-Content (Join-Path $output.FullName 'bench.jsonl') -Encoding UTF8
        $stderr.Result | Add-Content (Join-Path $output.FullName 'bench.err') -Encoding UTF8
        if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode); inspect bench.err." }
    }
    $tcpOnly = $Filter -eq 'tcp'
    if (!$tcpOnly) {
        Invoke-Bench $bench "$index $local $peer $Seconds $Filter".Trim() 'benchmark' ([int]($Seconds * 20 + 60))
    }
    if ($Filter -eq '' -or $tcpOnly) {
        $tcpBench = Join-Path $build 'chmlink-tcp-bench.exe'
        if (!(Test-Path -LiteralPath $tcpBench)) { throw 'Build chmlink-tcp-bench first.' }
        # IP mode: Windows uses no task offloads there, so one plain run.
        Invoke-Bench $tcpBench "$index $local $peer $alias $Seconds 4" 'TCP benchmark (IP mode)' ([int]($Seconds * 4 + 60))

        # IP mode at MTU 65535: the stack sends packets up to 64 KiB, and the
        # reflector cuts them to 1500 bytes (chmlink_ip_split) before injecting,
        # as an application tunnelling over a 1500-byte path would.
        $big = Run-Device @('create', $inf, '--owner', $owner.ToString('B'), '--mode', 'ip', '--mtu', '65535', '--name', 'ChmLink Bench IP 64K') | ConvertFrom-Json
        $devices += $big
        if ($deviceRestart) { throw 'Device installation requires a restart; stopping before the benchmark.' }
        $bigWindows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $big.InstanceId)
        if ($bigWindows.Count -ne 1 -or !$bigWindows[0].GUID) { throw 'Large-MTU test interface is not ready.' }
        $bigAdapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$bigWindows[0].GUID })
        if ($bigAdapter.Count -ne 1) { throw 'Cannot resolve the unique large-MTU test adapter.' }
        $bigIndex = $bigAdapter[0].ifIndex
        Set-NetIPInterface -InterfaceIndex $bigIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
        New-NetIPAddress -InterfaceIndex $bigIndex -IPAddress $bigLocal -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
        Start-Sleep -Seconds 3
        Invoke-Bench $tcpBench "$bigIndex $bigLocal $bigPeer $bigAlias $Seconds 4" 'TCP benchmark (IP mode, MTU 65535)' ([int]($Seconds * 4 + 60))
        # The same packets injected whole: the bound for a tunnel that carries
        # large packets end to end (tcp_mtu65535_whole_*).
        $env:CHMLINK_BENCH_NO_SPLIT = '1'
        try { Invoke-Bench $tcpBench "$bigIndex $bigLocal $bigPeer $bigAlias $Seconds 4" 'TCP benchmark (IP mode, MTU 65535, unsplit)' ([int]($Seconds * 4 + 60)) }
        finally { Remove-Item Env:CHMLINK_BENCH_NO_SPLIT }

        # Ethernet mode: without and with offloads. The reflector answers ARP itself.
        $eth = Run-Device @('create', $inf, '--owner', $owner.ToString('B'), '--mode', 'ethernet', '--name', 'ChmLink Bench Ethernet') | ConvertFrom-Json
        $devices += $eth
        if ($deviceRestart) { throw 'Device installation requires a restart; stopping before the benchmark.' }
        $ethWindows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $eth.InstanceId)
        if ($ethWindows.Count -ne 1 -or !$ethWindows[0].GUID) { throw 'Ethernet test interface is not ready.' }
        $ethAdapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$ethWindows[0].GUID })
        if ($ethAdapter.Count -ne 1) { throw 'Cannot resolve the unique Ethernet test adapter.' }
        $ethIndex = $ethAdapter[0].ifIndex
        Set-NetIPInterface -InterfaceIndex $ethIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
        New-NetIPAddress -InterfaceIndex $ethIndex -IPAddress $ethLocal -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
        Start-Sleep -Seconds 3

        # What Windows believes about the adapter's offloads, sampled in each half.
        $phase = 2 * ($Seconds + 0.2) + 1.5
        $state = Start-Job -ArgumentList $ethIndex, $phase, (Join-Path $output.FullName 'offload-state.txt') -ScriptBlock {
            param($ifIndex, $phase, $path)
            [Console]::OutputEncoding = [Text.Encoding]::UTF8; chcp 65001 | Out-Null
            foreach ($label in 'without offloads', 'with offloads') {
                Start-Sleep -Milliseconds ([int]($phase * 500 + 1000))
                $name = (Get-NetAdapter -IncludeHidden | Where-Object ifIndex -eq $ifIndex).Name
                "== $label ($(Get-Date -Format s))" | Add-Content $path -Encoding UTF8
                foreach ($probe in { Get-NetAdapterLso -Name $name -IncludeHidden | Format-List * },
                                   { Get-NetAdapterChecksumOffload -Name $name -IncludeHidden | Format-List * },
                                   { Get-NetAdapterRsc -Name $name -IncludeHidden | Format-List * },
                                   { netsh int ipv4 show offload $ifIndex }) {
                    try { & $probe 2>&1 | Out-String -Width 200 | Add-Content $path -Encoding UTF8 }
                    catch { "probe failed: $_" | Add-Content $path -Encoding UTF8 }
                }
                Start-Sleep -Milliseconds ([int]($phase * 500))
            }
        }
        try {
            Invoke-Bench $tcpBench "$ethIndex $ethLocal $ethPeer $ethAlias $Seconds 4" 'TCP benchmark (Ethernet mode)' ([int]($Seconds * 8 + 90))
        } finally {
            $state | Wait-Job -Timeout 30 | Out-Null; $state | Remove-Job -Force
        }
    }
    'PASS benchmark complete'; $exitCode = 0
} catch { Write-Warning $_.Exception.Message }
finally {
    [array]::Reverse($devices)
    foreach ($device in $devices) {
        if (!(Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue)) { continue }
        try {
            $current = Run-Device @('show', $device.InstanceId) | ConvertFrom-Json
            if ([guid]$current.AdapterId -ne [guid]$device.AdapterId -or [guid]$current.OwnerId -ne $owner) {
                throw 'Device identity changed; refusing cleanup.'
            }
            Run-Device @('remove', $device.InstanceId, '--owner', $owner.ToString('B')) | Out-Host
        } catch { Write-Warning "Cleanup failed for $($device.InstanceId): $_"; $exitCode = 1 }
    }
}
exit $exitCode
