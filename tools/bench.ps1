#requires -Version 5.1
# Opt-in data-path benchmark for the disposable test VM. Creates one owned
# IP-mode adapter, assigns 198.18.0.1/24 (benchmarking range), runs
# chmlink-bench against simulated peer 198.18.0.2 and removes the adapter.
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
$local = '198.18.0.1'; $peer = '198.18.0.2'
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
    foreach ($ip in @($local, $peer)) {
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
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $bench; $start.Arguments = "$index $local $peer $Seconds $Filter".Trim()
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    if (!$process.Start()) { throw 'Cannot start the benchmark.' }
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $deadline = [int](($Seconds * 20 + 60) * 1000)
    if (!$process.WaitForExit($deadline)) { $process.Kill(); throw 'Benchmark exceeded its deadline.' }
    $process.WaitForExit()
    $stdout.Result | Set-Content (Join-Path $output.FullName 'bench.jsonl') -Encoding UTF8
    $stderr.Result | Set-Content (Join-Path $output.FullName 'bench.err') -Encoding UTF8
    if ($process.ExitCode -ne 0) { throw "Benchmark exited $($process.ExitCode); inspect bench.err." }
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
