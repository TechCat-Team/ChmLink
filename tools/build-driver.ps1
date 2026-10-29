[CmdletBinding()]
param(
    [string]$KitRoot = "$PSScriptRoot/../build/wdk",
    [string]$Output = "$PSScriptRoot/../build/driver",
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$version = '10.0.26100.6584'
$kitVersion = '10.0.26100.0'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ build tools were not found.' }
    $environment = & cmd.exe /s /c "`"`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 >nul && set`""
    if ($LASTEXITCODE -ne 0) { throw 'Unable to initialize the Visual Studio environment.' }
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Use an x64 native tools environment.' }

New-Item -ItemType Directory -Force $KitRoot, $Output | Out-Null
$KitRoot = (Resolve-Path $KitRoot).Path
$Output = (Resolve-Path $Output).Path
foreach ($package in @('microsoft.windows.wdk.x64', 'microsoft.windows.sdk.cpp')) {
    $destination = Join-Path $KitRoot $package
    if (-not (Test-Path "$destination/c/Include/$kitVersion")) {
        if (-not $Restore) { throw "Missing $package; pass -Restore to download pinned Microsoft kits." }
        $archive = Join-Path $KitRoot "$package.zip"
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$package/$version/$package.$version.nupkg" -OutFile $archive
        Expand-Archive $archive -DestinationPath $destination -Force
    }
}
$wdk = "$KitRoot/microsoft.windows.wdk.x64/c"
$sdk = "$KitRoot/microsoft.windows.sdk.cpp/c"
$sources = @('driver/entry.c', 'driver/adapter.c', 'driver/oid.c', 'driver/control.c',
             'src/packet/packet.c', 'src/protocol/batch.c')
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $Output (([IO.Path]::GetFileNameWithoutExtension($source)) + '.obj')
    $objects += $object
    Invoke-Checked 'cl.exe' @('/nologo', '/c', '/kernel', '/Zl', '/GS', '/O2', '/W4', '/WX',
        '/wd4201', '/wd4324', '/wd4996', '/D_AMD64_', '/D_KERNEL_MODE', '/DNDIS660_MINIPORT=1',
        '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000002',
        "/I$repo/include", "/I$repo/driver", "/I$wdk/Include/$kitVersion/km",
        "/I$sdk/Include/$kitVersion/shared", "/I$sdk/Include/$kitVersion/ucrt",
        "/Fo$object", "$repo/$source")
}
Invoke-Checked 'link.exe' (@('/nologo', '/driver', '/subsystem:native,10.00',
    '/entry:GsDriverEntry', '/machine:x64', '/nodefaultlib', '/integritycheck',
    "/out:$Output/chmlink.sys", "/libpath:$wdk/Lib/$kitVersion/km/x64",
    'ntoskrnl.lib', 'hal.lib', 'ndis.lib', 'ntstrsafe.lib', 'BufferOverflowK.lib') + $objects)
Copy-Item "$repo/driver/chmlink.inf" $Output -Force
Invoke-Checked "$wdk/tools/$kitVersion/x64/infverif.exe" @('/w', "$Output/chmlink.inf")
Invoke-Checked "$wdk/bin/$kitVersion/x86/Inf2Cat.exe" @("/driver:$Output", '/os:10_X64,Server10_X64', '/uselocaltime')
Write-Output "Unsigned test package: $Output"
