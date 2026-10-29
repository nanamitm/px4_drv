param(
    [ValidateSet('x86','x64')][string[]]$Architecture = @('x86','x64'),
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143'
)
$ErrorActionPreference = 'Stop'
$winusbRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cache = Join-Path $winusbRoot 'build\rtlsdr-runtime'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

# バイナリーと対応ソースを固定し、キャッシュも毎回ハッシュで検証する。
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime-lock.json') -Raw | ConvertFrom-Json
function Get-PinnedArchive([string]$Url, [string]$Hash, [string]$Name) {
    $path = Join-Path $cache $Name
    if (-not (Test-Path -LiteralPath $path)) { Invoke-WebRequest -Uri $Url -OutFile $path -TimeoutSec 120 }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $Hash) { throw "RTL-SDR archive hash mismatch: $path" }
    return $path
}
$archive = Get-PinnedArchive $lock.url $lock.sha256 'rtl-sdr-blog-V1.4.0.zip'
$sourceArchive = Get-PinnedArchive $lock.source_url $lock.source_sha256 'rtl-sdr-blog-V1.4.0-source.zip'
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $cache 'V1.4.0') -Force
Expand-Archive -LiteralPath $sourceArchive -DestinationPath (Join-Path $cache 'source-V1.4.0') -Force

$msbuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
if ($msbuild) { $msbuildPath = $msbuild.Source } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuildPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
}
if (-not $msbuildPath) { throw 'MSBuild and the v143 C++ toolset are required.' }

foreach ($arch in $Architecture) {
    $platform = if ($arch -eq 'x86') { 'Win32' } else { 'x64' }
    & $msbuildPath (Join-Path $PSScriptRoot 'rtlsdr_probe.vcxproj') /nologo /v:minimal "/p:Configuration=Release-static;Platform=$platform;PlatformToolset=$PlatformToolset"
    if ($LASTEXITCODE -ne 0) { throw "RTL-SDR probe build failed: $arch" }
    $destination = Join-Path $winusbRoot "build\$arch\Release-static\rtlsdr"
    foreach ($dll in @('rtlsdr.dll','pthreadVC2.dll','msvcr100.dll')) {
        Copy-Item -LiteralPath (Join-Path $cache "V1.4.0\$arch\$dll") -Destination $destination -Force
    }
    Copy-Item -LiteralPath (Join-Path $cache 'source-V1.4.0\rtl-sdr-blog-1.4.0\COPYING') -Destination (Join-Path $destination 'COPYING.librtlsdr') -Force
    foreach ($property in $lock.libraries.$arch.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $destination $property.Name) -Algorithm SHA256).Hash -cne $property.Value) {
            throw "RTL-SDR dependency hash mismatch: $arch/$($property.Name)"
        }
    }
    # 配布元の rtlsdr.dll 自体も vcruntime140.dll を必要とするため、VS の再頒布用 DLL を同じ場所へ置く。
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    $redist = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^14\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName "$arch\Microsoft.VC143.CRT\vcruntime140.dll")) } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $redist) { throw "Visual C++ v143 redistributable files not found: $arch" }
    $crt = Join-Path $redist.FullName "$arch\Microsoft.VC143.CRT\vcruntime140.dll"
    $signature = Get-AuthenticodeSignature -LiteralPath $crt
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw "Invalid Microsoft runtime signature: $crt" }
    Copy-Item -LiteralPath $crt -Destination (Join-Path $destination 'vcruntime140.dll') -Force
    [ordered]@{
        runtime = $lock.runtime; url = $lock.url; sha256 = $lock.sha256
        source = $lock.source; license = $lock.license; architecture = $arch
        redist = [ordered]@{version=$redist.Name; name='vcruntime140.dll'; sha256=(Get-FileHash -LiteralPath $crt -Algorithm SHA256).Hash}
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'runtime.json') -Encoding UTF8
}
