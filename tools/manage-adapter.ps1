#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Create', 'ApplyName', 'SetMtu')]
    [string]$Action,
    [string]$InfPath,
    [string]$InstanceId,
    [string]$Name,
    [string]$Mac,
    [ValidateSet('ethernet', 'ip')][string]$Mode = 'ethernet',
    # IP adapters only: 1280 to 65535, 1500 by default. SetMtu restarts the
    # adapter, which ends an open data session.
    [ValidateRange(1280, 65535)][int]$Mtu,
    [guid]$OwnerId = '7C71F369-78C2-44F4-94CD-A2C14A29469B',
    [string]$DeviceTool
)
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $DeviceTool) { $DeviceTool = Join-Path $PSScriptRoot '../build/Release/chmlink-device.exe' }
$ErrorActionPreference = 'Stop'
# The SDK's configuration lock lives in a private namespace bounded by
# BUILTIN\Administrators, so unprivileged processes cannot squat it. Once this
# process holds the namespace open, the 'ChmLink\' prefix resolves to it.
function Get-ConfigurationMutex {
    if (!('ChmLink.ConfigLock' -as [type])) {
        Add-Type -Namespace ChmLink -Name ConfigLock -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateBoundaryDescriptorW(string name, uint flags);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool AddSIDToBoundaryDescriptor(ref IntPtr boundary, byte[] sid);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreatePrivateNamespaceW(ref SECURITY_ATTRIBUTES attributes, IntPtr boundary, string alias);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr OpenPrivateNamespaceW(IntPtr boundary, string alias);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out IntPtr descriptor, IntPtr size);
[StructLayout(LayoutKind.Sequential)] public struct SECURITY_ATTRIBUTES { public int Length; public IntPtr Descriptor; public bool Inherit; }
public static IntPtr Namespace;
'@
    }
    if ([ChmLink.ConfigLock]::Namespace -eq [IntPtr]::Zero) {
        $sid = [Security.Principal.SecurityIdentifier]::new([Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)
        $bytes = [byte[]]::new($sid.BinaryLength); $sid.GetBinaryForm($bytes, 0)
        $boundary = [ChmLink.ConfigLock]::CreateBoundaryDescriptorW('ChmLink', 0)
        if ($boundary -eq [IntPtr]::Zero -or ![ChmLink.ConfigLock]::AddSIDToBoundaryDescriptor([ref]$boundary, $bytes)) {
            throw "Cannot build the configuration lock boundary ($([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        }
        $attributes = [ChmLink.ConfigLock+SECURITY_ATTRIBUTES]::new()
        $attributes.Length = [Runtime.InteropServices.Marshal]::SizeOf($attributes)
        $descriptor = [IntPtr]::Zero
        if (![ChmLink.ConfigLock]::ConvertStringSecurityDescriptorToSecurityDescriptorW('D:(A;;GA;;;SY)(A;;GA;;;BA)', 1, [ref]$descriptor, [IntPtr]::Zero)) {
            throw "Cannot build the configuration lock descriptor ($([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        }
        $attributes.Descriptor = $descriptor
        for ($attempt = 0; $attempt -lt 8 -and [ChmLink.ConfigLock]::Namespace -eq [IntPtr]::Zero; $attempt++) {
            $handle = [ChmLink.ConfigLock]::CreatePrivateNamespaceW([ref]$attributes, $boundary, 'ChmLink')
            if ($handle -eq [IntPtr]::Zero) {
                $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                if ($errorCode -ne 183) { break }
                $handle = [ChmLink.ConfigLock]::OpenPrivateNamespaceW($boundary, 'ChmLink')
                if ($handle -eq [IntPtr]::Zero) { $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
            }
            [ChmLink.ConfigLock]::Namespace = $handle
        }
        if ([ChmLink.ConfigLock]::Namespace -eq [IntPtr]::Zero) { throw "Cannot open the configuration lock namespace ($errorCode)." }
    }
    [Threading.Mutex]::new($false, 'ChmLink\AdapterConfiguration')
}

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
    if ($LASTEXITCODE -ne 0 -or ($version -join '').Trim() -ne '5') { throw 'Device tool version 5 is required.' }
    if ($Action -eq 'SetMtu') {
        if (!$InstanceId -or !$Mtu -or $InfPath -or $Name -or $Mac -or $PSBoundParameters.ContainsKey('Mode')) {
            throw 'SetMtu requires InstanceId and Mtu only.'
        }
        $output = & $DeviceTool set-mtu $InstanceId $Mtu --owner $OwnerId.ToString('B')
        $code = $LASTEXITCODE
        if ($code -notin @(0, 3010)) { throw "Setting the MTU failed (exit $code)." }
        $output
        exit $code
    }
    if ($Action -eq 'Create') {
        if (!$InfPath -or $InstanceId) { throw 'Create requires InfPath and does not accept InstanceId.' }
        if ($Mode -eq 'ip' -and $Mac) { throw 'IP adapters do not have a MAC address.' }
        if ($Mtu -and $Mode -ne 'ip') { throw 'Only IP adapters have a configurable MTU.' }
        $arguments = @('create', (Resolve-Path -LiteralPath $InfPath).Path, '--owner', $OwnerId.ToString('B'))
        if ($Name) { $arguments += @('--name', $Name) }
        if ($Mac) { $arguments += @('--mac', $Mac) }
        $arguments += @('--mode', $Mode.ToLowerInvariant())
        if ($Mtu) { $arguments += @('--mtu', [string]$Mtu) }
        $changed = $true # native stderr can throw before LASTEXITCODE is inspected
        $output = & $DeviceTool @arguments
        $code = $LASTEXITCODE
        $changed = $code -eq 3
        if ($code -notin @(0, 3010)) { throw "Device creation failed (exit $code); inspect the device inventory before retrying." }
        $changed = $true; $nativeRestart = $code -eq 3010
        $created = $output | ConvertFrom-Json
        $InstanceId = $created.InstanceId
    } elseif (!$InstanceId -or $InfPath -or $Name -or $Mac -or $Mtu -or $PSBoundParameters.ContainsKey('Mode')) {
        throw 'ApplyName requires InstanceId and applies its stored name; creation parameters are not accepted.'
    }

    # Native create takes this same lock itself. Acquire it after it
    # return, then recheck identity before applying the connection alias.
    $mutex = Get-ConfigurationMutex
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
    if ($configuration.Mode -eq 1 -and $adapter[0].MacAddress.Replace('-', ':') -ine $configuration.Mac) {
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
