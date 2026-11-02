#requires -Version 5.1
<#
Functional check of the ARM64 build on ARM64 Windows (an emulated machine is enough;
performance is not measured there). Run elevated from the unpacked kit that the CI's
full lane builds (build-arm64 stage, chmlink-arm64-kit.zip):

  signed\        the candidate package, test-signed
  signed-old\    the same driver under DriverVer 0.4.3.99, for the package test
  bin\           chmlink-device, chmlink-lifecycle-test, chmlink-ip-test, chmlink.dll
  embedded\      chmlink-quickstart and chmlink-package-test with the package embedded
  tools\         this script and the suites it runs
  chmlink-surprise.sys, test.cer

Test signing must be on. The certificate is added to Root and TrustedPublisher. Stages:
quickstart (create from the embedded package, ping, delete), lifecycle, ip, package
(with an anchor adapter, as in the CI). Adapters created here are removed again; the
driver package stays installed.
#>
[CmdletBinding()]
param(
    [string]$Kit,
    [string]$OutputDirectory,
    [string[]]$Stages = @('quickstart', 'lifecycle', 'ip', 'package'),
    # Emulated machines are slow; every suite gets this factor on its time limits.
    [int]$Slowdown = 4
)
if (-not $Kit) { $Kit = Join-Path $PSScriptRoot '..' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $Kit 'results' }
$ErrorActionPreference = 'Stop'
$Kit = (Resolve-Path -LiteralPath $Kit).Path
$output = New-Item -ItemType Directory -Force -Path $OutputDirectory
$owner = '{3C6B1F0E-8A42-4D7B-9E15-2F04A6D9C871}'
$anchor = $null
$results = [ordered]@{}
$exitCode = 1

function Log([string]$Text) {
    $line = '[{0:HH:mm:ss}] {1}' -f (Get-Date), $Text
    Write-Output $line
    Add-Content -LiteralPath (Join-Path $output.FullName 'arm64.log') -Value $line -Encoding UTF8
}
function Invoke-Logged([string]$Name, [string]$FilePath, [string]$Arguments, [int]$Seconds) {
    $Seconds *= $Slowdown
    Log "$Name ($FilePath $Arguments, limit $Seconds s)"
    $start = [Diagnostics.ProcessStartInfo]::new($FilePath, $Arguments)
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $errors = $process.StandardError.ReadToEndAsync(); $text = $process.StandardOutput.ReadToEndAsync()
    if (!$process.WaitForExit($Seconds * 1000)) { $process.Kill(); throw "$Name did not finish in $Seconds s" }
    $process.WaitForExit()
    $log = Join-Path $output.FullName (($Name -replace '[^A-Za-z0-9]+', '-') + '.log')
    Set-Content -LiteralPath $log -Value ($text.Result + "`n--- stderr`n" + $errors.Result) -Encoding UTF8
    if ($process.ExitCode -ne 0) { throw "$Name exited $($process.ExitCode); see $log" }
    $text.Result
}
function Invoke-Stage([string]$Name, [scriptblock]$Body) {
    if ($Stages -notcontains $Name) { return }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try { & $Body; $results[$Name] = 'PASS' } catch { $results[$Name] = "FAIL: $($_.Exception.Message)"; throw }
    finally { Log ("== {0}: {1} ({2:N0} s)" -f $Name, $results[$Name], $watch.Elapsed.TotalSeconds) }
}

try {
    if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw "This is a $env:PROCESSOR_ARCHITECTURE machine; run on ARM64 Windows." }
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated.' }
    if ((& bcdedit.exe /enum '{current}' | Out-String) -notmatch '(?im)^\s*testsigning\s+Yes') { throw 'Test signing is off: bcdedit /set testsigning on, then restart.' }
    $os = Get-CimInstance Win32_OperatingSystem
    Log "ARM64 kit $Kit on $($os.Caption) $($os.Version)"
    foreach ($store in 'Root', 'TrustedPublisher') {
        Import-Certificate -FilePath (Join-Path $Kit 'test.cer') -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }
    $bin = Join-Path $Kit 'bin'; $embedded = Join-Path $Kit 'embedded'; $tools = Join-Path $Kit 'tools'
    $inf = Join-Path $Kit 'signed\chmlink.inf'
    $ps = "-NoProfile -ExecutionPolicy Bypass -File"

    Invoke-Stage 'quickstart' {
        Invoke-Logged 'quickstart' (Join-Path $embedded 'chmlink-quickstart.exe') '--self-test' 180 | Out-Null
    }
    Invoke-Stage 'lifecycle' {
        Invoke-Logged 'lifecycle' 'powershell.exe' ("$ps `"$tools\test-lifecycle.ps1`" -InfPath `"$inf`" -BuildDirectory `"$bin`" -OutputDirectory `"$($output.FullName)\lifecycle`" -FuzzSeconds 5") 900 | Out-Null
    }
    Invoke-Stage 'ip' {
        Invoke-Logged 'ip' 'powershell.exe' ("$ps `"$tools\test-ip.ps1`" -InfPath `"$inf`" -BuildDirectory `"$bin`" -OutputDirectory `"$($output.FullName)\ip`"") 600 | Out-Null
    }
    Invoke-Stage 'package' {
        # The package test expects exactly one adapter on the candidate, as the CI leaves it.
        $created = Invoke-Logged 'anchor' (Join-Path $bin 'chmlink-device.exe') ("create `"$inf`" --owner $owner --name `"ChmLink ARM64 anchor`"") 180
        $script:anchor = ($created | ConvertFrom-Json).InstanceId
        Log "  anchor $($script:anchor)"
        Invoke-Logged 'package' 'powershell.exe' ("$ps `"$tools\test-package.ps1`" -OldInfPath `"$Kit\signed-old\chmlink.inf`" -EmbeddedDirectory `"$embedded`" -BuildDirectory `"$bin`" -OutputDirectory `"$($output.FullName)\package`" -FilterPath `"$Kit\chmlink-surprise.sys`"") 900 | Out-Null
    }
    Log 'PASS arm64'
    $exitCode = 0
} catch {
    Log "FAIL $($_.Exception.Message)"
} finally {
    if ($anchor) {
        try { Invoke-Logged 'remove anchor' (Join-Path $Kit 'bin\chmlink-device.exe') ("remove `"$anchor`" --owner $owner") 120 | Out-Null }
        catch { Log "cleanup: $($_.Exception.Message)"; $exitCode = 1 }
    }
    [pscustomobject]$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output.FullName 'summary.json') -Encoding UTF8
}
exit $exitCode
