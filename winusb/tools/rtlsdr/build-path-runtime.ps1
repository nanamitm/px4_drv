param(
    [Parameter(Mandatory)][string]$LibusbSource,
    [Parameter(Mandatory)][string]$RtlSdrSource,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidateSet('Win32','x64')][string]$Platform,
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143'
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild and the Visual C++ toolset are required.' }

$LibusbSource = (Resolve-Path -LiteralPath $LibusbSource).Path
$RtlSdrSource = (Resolve-Path -LiteralPath $RtlSdrSource).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$settings = "/p:Configuration=Release;Platform=$Platform;PlatformToolset=$PlatformToolset"
& $msbuild (Join-Path $LibusbSource 'msvc\libusb_static.vcxproj') /nologo /v:minimal $settings
if ($LASTEXITCODE -ne 0) { throw "libusb build failed: $Platform" }
$libusb = Join-Path $LibusbSource "build\$PlatformToolset\$Platform\Release\lib\libusb-1.0.lib"

$paths = "/p:LibusbSource=$LibusbSource;LibusbLibrary=$libusb;RtlSdrSource=$RtlSdrSource;OutDir=$OutputDirectory\"
& $msbuild (Join-Path $PSScriptRoot 'rtlsdr_path.vcxproj') /nologo /v:minimal $settings $paths
if ($LASTEXITCODE -ne 0) { throw "RTL-SDR path runtime build failed: $Platform" }
