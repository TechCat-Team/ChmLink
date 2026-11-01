#requires -Version 5.1
# The bench.ps1 IP-mode benchmarks over tap-windows6 in TUN mode, for
# comparison: tap-bench and tap-tcp-bench (bench/tap_shim.c) use the one
# installed TAP-Windows Adapter V9, assign 198.18.0.1/24 and remove the
# address on exit. Install the adapter once from the official release
# (dist.win10.zip: devcon install OemVista.inf tap0901). Run in the disposable
# test VM with no other adapter holding 198.18.0.0/24.
[CmdletBinding()]
param(
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
    $bench = Join-Path $build 'tap-bench.exe'; $tcpBench = Join-Path $build 'tap-tcp-bench.exe'
    if (!(Test-Path -LiteralPath $bench) -or !(Test-Path -LiteralPath $tcpBench)) { throw 'Build tap-bench and tap-tcp-bench first.' }
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    foreach ($ip in @($local, $peer, $alias)) {
        if (Get-NetIPAddress -IPAddress $ip -ErrorAction SilentlyContinue) { throw "Benchmark address already assigned: $ip" }
    }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    $tap = @(Get-NetAdapter -IncludeHidden | Where-Object InterfaceDescription -like 'TAP-Windows Adapter V9*')
    if ($tap.Count -ne 1) { throw 'Install exactly one TAP-Windows Adapter V9 first.' }
    $env:CHMLINK_TAP_ADDRESS = "$local/24"
    [ordered]@{
        Os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
        Cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        Logical = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
        Memory = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
        DriverVersion = 'tap-windows6 ' + $tap[0].DriverVersion
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
        Invoke-Bench $bench "0 $local $peer $Seconds $Filter".Trim() 'TAP benchmark' ([int]($Seconds * 20 + 90))
    }
    Invoke-Bench $tcpBench "0 $local $peer $alias $Seconds 4" 'TAP TCP benchmark' ([int]($Seconds * 4 + 90))
    'PASS tap benchmark complete'; $exitCode = 0
} catch { Write-Warning $_.Exception.Message } finally {
    # A benchmark that failed leaves its address on the TAP adapter.
    Get-NetIPAddress -IPAddress $local -ErrorAction SilentlyContinue |
        Where-Object { (Get-NetAdapter -InterfaceIndex $_.InterfaceIndex -IncludeHidden).InterfaceDescription -like 'TAP-Windows Adapter V9*' } |
        Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
}
exit $exitCode
