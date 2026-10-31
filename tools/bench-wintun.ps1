#requires -Version 5.1
# The bench.ps1 IP-mode benchmarks over Wintun, for comparison: wintun-bench
# and wintun-tcp-bench (bench/wintun_shim.c) create their own Wintun adapter
# with 198.18.0.1/24 and remove it on exit. Run in the disposable test VM with
# no other adapter holding 198.18.0.0/24 (bench.ps1 removes its adapters).
# -WintunDll is the amd64 wintun.dll of the official release to compare with.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WintunDll,
    [string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [double]$Seconds = 5,
    [string]$Filter = ''
)
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/verify/Release' }
$ErrorActionPreference = 'Stop'
$local = '198.18.0.1'; $peer = '198.18.0.2'; $alias = '198.18.0.3'
$exitCode = 1
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated in the disposable test VM.' }
    $build = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $bench = Join-Path $build 'wintun-bench.exe'; $tcpBench = Join-Path $build 'wintun-tcp-bench.exe'
    if (!(Test-Path -LiteralPath $bench) -or !(Test-Path -LiteralPath $tcpBench)) { throw 'Build wintun-bench and wintun-tcp-bench first.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    foreach ($ip in @($local, $peer, $alias)) {
        if (Get-NetIPAddress -IPAddress $ip -ErrorAction SilentlyContinue) { throw "Benchmark address already assigned: $ip" }
    }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    Copy-Item -LiteralPath $WintunDll -Destination (Join-Path $build 'wintun.dll') -Force
    $env:CHMLINK_WINTUN_ADDRESS = "$local/24"
    [ordered]@{
        Os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
        Cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        Logical = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
        Memory = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
        DriverVersion = 'wintun ' + (Get-Item -LiteralPath $WintunDll).VersionInfo.FileVersion
        Started = (Get-Date).ToString('s')
    } | ConvertTo-Json | Set-Content (Join-Path $output.FullName 'environment.json') -Encoding UTF8
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
    if ($Filter -ne 'tcp') {
        Invoke-Bench $bench "0 $local $peer $Seconds $Filter".Trim() 'Wintun benchmark' ([int]($Seconds * 20 + 90))
    }
    Invoke-Bench $tcpBench "0 $local $peer $alias $Seconds 4" 'Wintun TCP benchmark' ([int]($Seconds * 4 + 90))
    'PASS wintun benchmark complete'; $exitCode = 0
} catch { Write-Warning $_.Exception.Message }
exit $exitCode
