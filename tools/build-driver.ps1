[CmdletBinding()]
param(
    [string]$KitRoot,
    [string]$Output,
    # Target architecture. ARM64 is cross-compiled on an x64 host.
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    [switch]$Restore,
    # Also run PREfast (/analyze). Findings in our own sources fail the build.
    [switch]$Analyze
)
# Defaults are computed here, not in the param block: Windows PowerShell 5.1 evaluates $PSScriptRoot as empty there when the script is started with -File.
if (-not $KitRoot) { $KitRoot = Join-Path $PSScriptRoot '../build/wdk' }
if (-not $Output) { $Output = Join-Path $PSScriptRoot $(if ($Arch -eq 'x64') { '../build/driver' } else { "../build/driver-$Arch" }) }
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$version = '10.0.26100.6584'
$kitVersion = '10.0.26100.0'
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Per architecture: VsDevCmd target, compiler define, linker machine, Inf2Cat OS list, /GS library.
$target = @{
    x64   = @{ Vs = 'amd64'; Define = '/D_AMD64_'; Machine = 'x64';   Os = '10_X64,Server10_X64'; Gs = 'BufferOverflowK.lib'; Extra = @() }
    arm64 = @{ Vs = 'arm64'; Define = '/D_ARM64_'; Machine = 'arm64'; Os = '10_RS3_ARM64';        Gs = 'BufferOverflowFastFailK.lib'
              # Helpers the ARM64 compiler calls (interlocked fallbacks, /GS cookies), as the WDK's MSBuild
              # adds; from the compiler's own lib directory, known once the tools environment is set.
              Extra = @('arm64rt.lib') }
}[$Arch]

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    Write-Host ("[{0:HH:mm:ss}] Running {1}" -f (Get-Date), $Program)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Invoke-InfVerification([string[]]$Arguments) {
    Write-Host ("[{0:HH:mm:ss}] Validating INF: {1}" -f (Get-Date), ($Arguments -join ' '))
    $diagnostics = & $infverif @Arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    if ($diagnostics) { Write-Host $diagnostics }
    # This kit returns zero for some invalid command lines. Never treat its
    # usage/error diagnostics as successful verification solely on exit code.
    if ($exitCode -ne 0 -or $diagnostics -match '(?im)Invalid build string|^\s*Usage:|\bERROR\([0-9]+\)') {
        throw "INF verification failed (exit code $exitCode)."
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "Initializing Visual Studio $Arch tools"
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires $(if ($Arch -eq 'x64') { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' }) -property installationPath
    if (-not $vs) { throw "Visual Studio C++ build tools for $Arch were not found." }
    $bootstrap = Join-Path ([IO.Path]::GetTempPath()) (([guid]::NewGuid().ToString()) + '.cmd')
    try {
        @("@call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=$($target.Vs) -host_arch=amd64 >nul",
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
if ($env:VSCMD_ARG_TGT_ARCH -ne $Arch) { throw "Use a Visual Studio tools environment that targets $Arch (this one targets $env:VSCMD_ARG_TGT_ARCH)." }

if ($target.Extra) { $target.Extra = @("/libpath:$env:VCToolsInstallDir\lib\$Arch") + $target.Extra }
New-Item -ItemType Directory -Force $KitRoot, $Output | Out-Null
$KitRoot = (Resolve-Path $KitRoot).Path
$Output = (Resolve-Path $Output).Path
# The x64 kit carries the host tools (infverif, Inf2Cat); another target adds its headers and libraries.
foreach ($package in @(@('microsoft.windows.wdk.x64', "microsoft.windows.wdk.$Arch", 'microsoft.windows.sdk.cpp') | Select-Object -Unique)) {
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
$wdk = "$KitRoot/microsoft.windows.wdk.$Arch/c"
$wdkHost = "$KitRoot/microsoft.windows.wdk.x64/c"
$sdk = "$KitRoot/microsoft.windows.sdk.cpp/c"
$sources = @('driver/entry.c', 'driver/adapter.c', 'driver/config.c', 'driver/oid.c', 'driver/control.c',
             'driver/offload.c', 'driver/session.c',
             'src/packet/packet.c', 'src/packet/offload.c', 'src/protocol/batch.c', 'src/protocol/query.c',
             'src/protocol/session.c', 'src/ring/ring.c')
$analysis = @()
if ($Analyze) {
    # PREfast through the engine that ships with the Visual Studio C++ workload. Header
    # directories are "external" so only our own sources are reported on.
    #
    # Not used: the WDK's driver rule sets (DriverRecommendedRules.ruleset). On this toolchain the
    # engine rejects them (C6800) and loads no C28xxx driver rules either way, so they add noise and
    # no findings. Revisit with a full Visual Studio + WDK integration.
    $engine = Get-ChildItem "$env:VCToolsInstallDir\bin\Hostx64\$Arch\EspXEngine.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $engine) { throw 'Code analysis engine (EspXEngine.dll) not found in this Visual Studio installation.' }
    $analysis = @('/analyze', "/analyze:plugin$($engine.FullName)", '/analyze:external-', '/external:W0',
        "/external:I$wdk/Include/$kitVersion/km", "/external:I$wdk/Include/$kitVersion/shared",
        "/external:I$sdk/Include/$kitVersion/shared", "/external:I$sdk/Include/$kitVersion/ucrt")
}
$objects = @()
$compileFailed = $false
foreach ($source in $sources) {
    Write-Host "Compiling $source"
    $object = Join-Path $Output (($source -replace '[/\\]', '_' -replace '\.c$', '.obj'))
    $objects += $object
    try {
        Invoke-Checked 'cl.exe' (@('/nologo', '/c', '/kernel', '/Zl', '/GS', '/O2', '/Z7', '/W4', '/WX',
        '/wd4201', '/wd4324', '/wd4996', $target.Define, '/DNDIS_MINIPORT_DRIVER=1', '/DNDIS660_MINIPORT=1',
        '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000002',
        "/I$repo/include", "/I$repo/driver", "/I$wdk/Include/$kitVersion/km",
        "/I$sdk/Include/$kitVersion/shared", "/I$sdk/Include/$kitVersion/ucrt",
            "/Fo$object") + $analysis + @("$repo/$source"))
    } catch {
        Write-Warning "Compilation failed for $source : $_"
        $compileFailed = $true
    }
}
if ($compileFailed) { throw 'Driver compilation failed; see compiler diagnostics above.' }
Invoke-Checked 'link.exe' (@('/nologo', '/driver', '/subsystem:native,10.00',
    '/entry:GsDriverEntry', "/machine:$($target.Machine)", '/nodefaultlib', '/integritycheck',
    # Symbols for crash and live-dump analysis. /DEBUG turns off /OPT:REF,ICF; keep them on.
    '/debug', "/pdb:$Output/chmlink.pdb", '/opt:ref', '/opt:icf',
    "/out:$Output/chmlink.sys", "/libpath:$wdk/Lib/$kitVersion/km/$Arch",
    'ntoskrnl.lib', 'hal.lib', 'ndis.lib', 'ntstrsafe.lib', $target.Gs) + $target.Extra + $objects)
# Test-only upper filter for the surprise-removal test (tests/windows/surprise_filter.c).
# It goes next to the package, never into it.
$testOutput = Join-Path (Split-Path $Output -Parent) $(if ($Arch -eq 'x64') { 'driver-test' } else { "driver-test-$Arch" })
New-Item -ItemType Directory -Force $testOutput | Out-Null
Write-Host 'Compiling tests/windows/surprise_filter.c'
Invoke-Checked 'cl.exe' (@('/nologo', '/c', '/kernel', '/Zl', '/GS', '/O2', '/Z7', '/W4', '/WX',
    $target.Define, '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000002',
    "/I$wdk/Include/$kitVersion/km", "/I$sdk/Include/$kitVersion/shared", "/I$sdk/Include/$kitVersion/ucrt",
    "/Fo$testOutput/surprise_filter.obj") + $analysis + @("$repo/tests/windows/surprise_filter.c"))
Invoke-Checked 'link.exe' (@('/nologo', '/driver', '/subsystem:native,10.00', '/entry:GsDriverEntry', "/machine:$($target.Machine)",
    '/nodefaultlib', '/integritycheck', "/out:$testOutput/chmlink-surprise.sys", "/libpath:$wdk/Lib/$kitVersion/km/$Arch",
    'ntoskrnl.lib', 'hal.lib', 'wdmsec.lib', $target.Gs) + $target.Extra + @("$testOutput/surprise_filter.obj"))
# Inf2Cat refuses a postdated DriverVer by the build machine's clock, and hosted
# runners use UTC: a date taken in a later time zone fails there but not here.
$driverDate = [regex]::Match((Get-Content -Raw "$repo/driver/chmlink.inf"), '(?m)^\s*DriverVer\s*=\s*(\d{1,2}/\d{1,2}/\d{4})\s*,')
if (!$driverDate.Success) { throw 'driver/chmlink.inf has no DriverVer date.' }
$driverDate = [DateTime]::ParseExact($driverDate.Groups[1].Value, 'M/d/yyyy', [Globalization.CultureInfo]::InvariantCulture)
if ($driverDate -gt [DateTime]::UtcNow.Date) {
    throw "DriverVer date $($driverDate.ToString('MM/dd/yyyy')) is after today's UTC date $([DateTime]::UtcNow.ToString('MM/dd/yyyy')); use the UTC date."
}
Copy-Item "$repo/driver/chmlink.inf" $Output -Force
$infverif = "$wdkHost/tools/$kitVersion/x64/infverif.exe"
Invoke-InfVerification @('/u', "$Output/chmlink.inf")
# Net-class run-from-Driver-Store support starts at build 25319. Validate
# isolation on that path while retaining DIRID 12 for Windows 10 1607/Server 2016.
Invoke-InfVerification @('/w', '/wbuild', '10.0.25319', "$Output/chmlink.inf")
Invoke-Checked "$wdkHost/bin/$kitVersion/x86/Inf2Cat.exe" @("/driver:$Output", "/os:$($target.Os)", '/uselocaltime')
Write-Output "Unsigned $Arch test package: $Output"
