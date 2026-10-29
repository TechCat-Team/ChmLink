[CmdletBinding()]
param(
    [string]$KitRoot = "$PSScriptRoot/../build/wdk",
    [string]$Output = "$PSScriptRoot/../build/driver",
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$version = '10.0.26100.6584'
$kitVersion = '10.0.26100.0'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    Write-Host ("[{0:HH:mm:ss}] Running {1}" -f (Get-Date), $Program)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host 'Initializing Visual Studio x64 tools'
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ build tools were not found.' }
    $bootstrap = Join-Path ([IO.Path]::GetTempPath()) (([guid]::NewGuid().ToString()) + '.cmd')
    try {
        @("@call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 >nul",
          '@if errorlevel 1 exit /b %errorlevel%', '@set') | Set-Content $bootstrap -Encoding ASCII
        $environment = & cmd.exe /c $bootstrap
        if ($LASTEXITCODE -ne 0) { throw 'Unable to initialize the Visual Studio environment.' }
    } finally { Remove-Item $bootstrap -Force }
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
    if (-not (Test-Path "$destination/.restored-$version")) {
        if (-not $Restore) { throw "Missing $package; pass -Restore to download pinned Microsoft kits." }
        $archive = Join-Path $KitRoot "$package.zip"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        Write-Output "Restoring $package $version"
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$package/$version/$package.$version.nupkg" -OutFile $archive -UseBasicParsing -TimeoutSec 180
        Write-Host ("Downloaded {0:N1} MiB; extracting {1}" -f ((Get-Item $archive).Length / 1MB), $package)
        if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $destination)
        New-Item -ItemType File "$destination/.restored-$version" | Out-Null
        Write-Host ("Restored {0} in {1:N1}s" -f $package, $timer.Elapsed.TotalSeconds)
    } else {
        Write-Host "Using cached $package $version"
    }
}
$wdk = "$KitRoot/microsoft.windows.wdk.x64/c"
$sdk = "$KitRoot/microsoft.windows.sdk.cpp/c"
$sources = @('driver/entry.c', 'driver/adapter.c', 'driver/oid.c', 'driver/control.c',
             'src/packet/packet.c', 'src/protocol/batch.c')
$objects = @()
$compileFailed = $false
foreach ($source in $sources) {
    Write-Host "Compiling $source"
    $object = Join-Path $Output (([IO.Path]::GetFileNameWithoutExtension($source)) + '.obj')
    $objects += $object
    try {
        Invoke-Checked 'cl.exe' @('/nologo', '/c', '/kernel', '/Zl', '/GS', '/O2', '/W4', '/WX',
        '/wd4201', '/wd4324', '/wd4996', '/D_AMD64_', '/DNDIS660_MINIPORT=1',
        '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000002',
        "/I$repo/include", "/I$repo/driver", "/I$wdk/Include/$kitVersion/km",
        "/I$sdk/Include/$kitVersion/shared", "/I$sdk/Include/$kitVersion/ucrt",
            "/Fo$object", "$repo/$source")
    } catch {
        Write-Warning "Compilation failed for $source : $_"
        $compileFailed = $true
    }
}
if ($compileFailed) { throw 'Driver compilation failed; see compiler diagnostics above.' }
Invoke-Checked 'link.exe' (@('/nologo', '/driver', '/subsystem:native,10.00',
    '/entry:GsDriverEntry', '/machine:x64', '/nodefaultlib', '/integritycheck',
    "/out:$Output/chmlink.sys", "/libpath:$wdk/Lib/$kitVersion/km/x64",
    'ntoskrnl.lib', 'hal.lib', 'ndis.lib', 'ntstrsafe.lib', 'BufferOverflowK.lib') + $objects)
Copy-Item "$repo/driver/chmlink.inf" $Output -Force
Invoke-Checked "$wdk/tools/$kitVersion/x64/infverif.exe" @('/w', "$Output/chmlink.inf")
Invoke-Checked "$wdk/bin/$kitVersion/x86/Inf2Cat.exe" @("/driver:$Output", '/os:10_X64,Server10_X64', '/uselocaltime')
Write-Output "Unsigned test package: $Output"
