#requires -Version 5.1
<#
Resource use of ChmLink and Wintun side by side (bench/resource_bench.c): CPU
at 0, 100, 1000, 10000 and 50000 packets per second each way, memory and
threads per session, and optionally a soak of the ChmLink SDK. Opt-in, for the
disposable test VM with the ChmLink candidate installed. Each driver gets
198.18.0.1/24 in turn; the soak uses 198.19.0.1/24.

Wintun runs twice: with the same poll-before-sleep as the ChmLink SDK, and
waiting on its event at once as Wintun's example does.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InfPath,
    [string]$WintunDll,
    [string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [int]$Seconds = 10,
    [int[]]$Rates = @(0, 100, 1000, 10000, 50000),
    [double]$SoakMinutes = 0,
    [switch]$SkipRates
)
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/verify/Release' }
$ErrorActionPreference = 'Stop'
$owner = [guid]'3F6B2D7E-91A4-4C58-B0E2-7A1D5C9E4B36'   # tools/bench.ps1's owner: CI cleans it up
$local = '198.18.0.1'; $peer = '198.18.0.2'
$devices = @(); $exitCode = 1
function Run-Device([string[]]$Arguments) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & $tool @Arguments; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $previous }
    if ($code -ne 0) { throw "Device tool exited $code." }
    return $out
}
function Invoke-Bench([string]$Program, [string]$Arguments, [string]$Label, [int]$DeadlineSeconds) {
    Write-Output ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $Label)
    $start = [Diagnostics.ProcessStartInfo]::new($Program, $Arguments)
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $process.PriorityClass = 'Normal'
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit($DeadlineSeconds * 1000)) { $process.Kill(); throw "$Label exceeded its deadline." }
    $process.WaitForExit()
    foreach ($line in ($stdout.Result -split "`r?`n" | Where-Object { $_ })) {
        $json = $line | ConvertFrom-Json
        $json | Add-Member -NotePropertyName driver -NotePropertyValue $Label
        $json | ConvertTo-Json -Compress | Add-Content (Join-Path $output.FullName 'resources.jsonl') -Encoding UTF8
        Write-Output "  $line"
    }
    if ($stderr.Result) { "== $Label`n$($stderr.Result)" | Add-Content (Join-Path $output.FullName 'resources.err') -Encoding UTF8 }
    if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode); inspect resources.err." }
}
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated in the disposable test VM.' }
    $build = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $tool = Join-Path $build 'chmlink-device.exe'
    $bench = Join-Path $build 'chmlink-resource-bench.exe'; $wintunBench = Join-Path $build 'wintun-resource-bench.exe'
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory.' }
    $output = New-Item -ItemType Directory -Path $OutputDirectory
    $inf = (Resolve-Path -LiteralPath $InfPath).Path
    $rateArgs = $Rates -join ' '
    $rateDeadline = [int]($Seconds * 2 * $Rates.Count + 120) + 60
    [ordered]@{
        Os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
        Cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        Logical = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
        Memory = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
        Started = (Get-Date).ToString('s')
    } | ConvertTo-Json | Set-Content (Join-Path $output.FullName 'environment.json') -Encoding UTF8

    if (!$SkipRates) {
        $device = Run-Device @('create', $inf, '--owner', $owner.ToString('B'), '--mode', 'ip', '--name', 'ChmLink Resources') | ConvertFrom-Json
        $devices += $device
        $index = (Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $device.InstanceId).InterfaceIndex
        Set-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
        New-NetIPAddress -InterfaceIndex $index -IPAddress $local -PrefixLength 24 -PolicyStore ActiveStore | Out-Null
        Invoke-Bench $bench "rate $index $local $peer $Seconds $rateArgs" 'chmlink' $rateDeadline
        Invoke-Bench $bench "memory $index" 'chmlink' 120
        Run-Device @('remove', $device.InstanceId, '--owner', $owner.ToString('B')) | Out-Null
        $devices = @()

        if ($WintunDll) {
            Copy-Item -LiteralPath $WintunDll -Destination (Join-Path $build 'wintun.dll') -Force
            $env:CHMLINK_WINTUN_ADDRESS = "$local/24"
            $env:CHMLINK_WINTUN_SPIN = '2048'
            Invoke-Bench $wintunBench "rate 0 $local $peer $Seconds $rateArgs" 'wintun-poll' $rateDeadline
            $env:CHMLINK_WINTUN_SPIN = '0'
            Invoke-Bench $wintunBench "rate 0 $local $peer $Seconds $rateArgs" 'wintun' $rateDeadline
            Invoke-Bench $wintunBench "memory 0" 'wintun' 120
        }
    }
    if ($SoakMinutes -gt 0) {
        Invoke-Bench $bench "soak `"$inf`" $SoakMinutes" 'chmlink-soak' ([int]($SoakMinutes * 60 + 600))
    }
    'PASS resource benchmark complete'; $exitCode = 0
} catch { Write-Warning $_.Exception.Message }
finally {
    foreach ($d in $devices) { try { & $tool remove $d.InstanceId --owner $owner.ToString('B') | Out-Null } catch { } }
}
exit $exitCode
