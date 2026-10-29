#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Create', 'Adopt', 'ApplyName')]
    [string]$Action,
    [string]$InfPath,
    [string]$InstanceId,
    [string]$Name,
    [string]$Mac,
    [guid]$OwnerId = '7C71F369-78C2-44F4-94CD-A2C14A29469B',
    [string]$DeviceTool = "$PSScriptRoot/../build/Release/chmlink-device.exe"
)
$ErrorActionPreference = 'Stop'
$changed = $false; $locked = $false; $mutex = $null
$nativeRestart = $false
try {
    if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'An elevated PowerShell is required.' }
    if ($OwnerId -eq [guid]::Empty) { throw 'OwnerId must not be empty.' }
    foreach ($field in @('Name', 'Mac')) {
        if ($PSBoundParameters.ContainsKey($field) -and [string]::IsNullOrWhiteSpace($PSBoundParameters[$field])) {
            throw "$field must not be empty."
        }
    }
    $DeviceTool = (Resolve-Path -LiteralPath $DeviceTool).Path
    $version = & $DeviceTool version
    if ($LASTEXITCODE -ne 0 -or ($version -join '').Trim() -ne '3') { throw 'Device tool version 3 is required.' }
    if ($Action -eq 'Create') {
        if (!$InfPath -or $InstanceId) { throw 'Create requires InfPath and does not accept InstanceId.' }
        $arguments = @('create', (Resolve-Path -LiteralPath $InfPath).Path, '--owner', $OwnerId.ToString('B'))
        if ($Name) { $arguments += @('--name', $Name) }
        if ($Mac) { $arguments += @('--mac', $Mac) }
        $changed = $true # native stderr can throw before LASTEXITCODE is inspected
        $output = & $DeviceTool @arguments
        $code = $LASTEXITCODE
        $changed = $code -eq 3
        if ($code -notin @(0, 3010)) { throw "Device creation failed (exit $code); inspect the device inventory before retrying." }
        $changed = $true; $nativeRestart = $code -eq 3010
        $created = $output | ConvertFrom-Json
        $InstanceId = $created.InstanceId
    } elseif ($Action -eq 'Adopt') {
        if (!$InstanceId -or $InfPath -or $Mac) { throw 'Adopt requires InstanceId and preserves the current MAC.' }
        $arguments = @('adopt', $InstanceId, '--owner', $OwnerId.ToString('B'))
        if ($Name) { $arguments += @('--name', $Name) }
        $changed = $true
        & $DeviceTool @arguments | Out-Host
        $code = $LASTEXITCODE
        $changed = $code -eq 3
        if ($code -ne 0) { throw 'Adoption failed; inspect the device inventory before retrying.' }
        $changed = $true
    } elseif (!$InstanceId -or $InfPath -or $Name -or $Mac) {
        throw 'ApplyName requires InstanceId and applies its stored name; creation parameters are not accepted.'
    }

    # Native create/adopt take this same lock themselves. Acquire it after they
    # return, then recheck identity before applying the connection alias.
    $mutex = [Threading.Mutex]::new($false, 'Global\ChmLink.AdapterConfiguration')
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] {
        $locked = $true; throw 'Configuration was interrupted; inspect the device inventory.'
    }
    if (!$locked) { throw 'Configuration is busy. ApplyName can be retried later.' }
    $configuration = & $DeviceTool show $InstanceId
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the device configuration.' }
    $configuration = $configuration | ConvertFrom-Json
    if (!$configuration.Managed -or [guid]$configuration.OwnerId -ne $OwnerId) { throw 'Device ownership does not match.' }
    $windows = @(Get-CimInstance Win32_NetworkAdapter | Where-Object PNPDeviceID -eq $InstanceId)
    if ($windows.Count -ne 1 -or !$windows[0].GUID) {
        throw "Device identity was saved, but the interface is not ready. Retry ApplyName for $InstanceId."
    }
    $adapter = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
    if ($adapter.Count -ne 1) { throw 'Cannot resolve a unique interface for the saved device.' }
    if ($adapter[0].MacAddress.Replace('-', ':') -ine $configuration.Mac) {
        throw 'The running MAC differs from the stored identity; inspect or restart the adapter before naming it.'
    }
    if ($adapter[0].Name -cne $configuration.Name) {
        $changed = $true
        Rename-NetAdapter -InputObject $adapter[0] -NewName $configuration.Name -Confirm:$false
    }
    $actual = @(Get-NetAdapter -IncludeHidden | Where-Object { [guid]$_.InterfaceGuid -eq [guid]$windows[0].GUID })
    if ($actual.Count -ne 1 -or $actual[0].Name -cne $configuration.Name) { throw 'Connection name verification failed.' }
    $configuration | ConvertTo-Json -Depth 4
    if ($nativeRestart) { exit 3010 }
    exit 0
} catch {
    Write-Warning $_.Exception.Message
    if ($nativeRestart) { Write-Warning 'Windows requested a manual restart before configuration can be completed.' }
    if ($changed) {
        Write-Warning "Configuration or naming may be partially applied. Retain the device and retry ApplyName with its owner. Instance: $InstanceId"
        exit 3
    }
    exit 1
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
}
