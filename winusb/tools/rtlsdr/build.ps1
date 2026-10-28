param(
    [ValidateSet('x86','x64')][string[]]$Architecture = @('x86','x64'),
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143'
)
$ErrorActionPreference = 'Stop'
$winusbRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cache = Join-Path $winusbRoot 'build\rtlsdr-runtime'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

# 更新時は URL と SHA-256 を同時に変更し、取得物を再検証する。
$runtimeUrl = 'https://github.com/rtlsdrblog/rtl-sdr-blog/releases/download/V1.4.0/Release.zip'
$expectedHash = '7EF33F1304647F65E5E0FDE43637A73D54F076E91E651A3CECC4F55A17FD9815'
$archive = Join-Path $cache 'rtl-sdr-blog-V1.4.0.zip'
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $runtimeUrl -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw "RTL-SDR runtime hash mismatch: $archive"
}
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $cache 'V1.4.0') -Force

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
    [ordered]@{
        runtime = 'rtlsdrblog/rtl-sdr-blog V1.4.0'
        url = $runtimeUrl
        sha256 = $expectedHash
        source = 'https://github.com/rtlsdrblog/rtl-sdr-blog/tree/V1.4.0'
        license = 'https://github.com/rtlsdrblog/rtl-sdr-blog/blob/V1.4.0/COPYING'
        architecture = $arch
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'runtime.json') -Encoding UTF8
}
