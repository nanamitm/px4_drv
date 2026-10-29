param(
    [ValidateSet('x86','x64')][string[]]$Architecture = @('x86','x64'),
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143',
    [switch]$SkipNativeBuild,
    [switch]$SkipPackage
)
$ErrorActionPreference='Stop'
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'Install Rust/Cargo, then run rustup target add i686-pc-windows-msvc x86_64-pc-windows-msvc.' }
function Invoke-Cargo([string[]]$CargoArguments) {
    # Windows PowerShell 5.1 ではログへのリダイレクト中の stderr もエラー扱いになる。
    # Cargo の進捗は文字列へ変換し、成否は終了コードで判定する。
    $previousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & cargo --config (Join-Path $frontend 'cargo-config.toml') @CargoArguments 2>&1 | ForEach-Object { $_.ToString() }
        $cargoExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previousPreference }
    if ($cargoExit -ne 0) { throw "Cargo failed (exit $cargoExit): $($CargoArguments -join ' ')" }
}
$winusbRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$frontend=Join-Path $winusbRoot 'tools\rtlsdr_frontend'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if(-not $visualStudio){throw 'Visual Studio C++ tools and Windows SDK are required.'}
$msbuild=Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
$devCmd=Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
& (Join-Path $PSScriptRoot 'build.ps1') -Architecture $Architecture -PlatformToolset $PlatformToolset
foreach($arch in $Architecture){
    $platform=if($arch -eq 'x86'){'Win32'}else{'x64'}
    $target=if($arch -eq 'x86'){'i686-pc-windows-msvc'}else{'x86_64-pc-windows-msvc'}
    $native=Join-Path $winusbRoot "build\$arch\Release-static"
    if(-not $SkipNativeBuild){
        foreach($project in @('DriverHost_PX4','BonDriver_PX4','WinSCard_PX4')){
            & $msbuild (Join-Path $winusbRoot "src\$project\$project.vcxproj") /nologo /v:minimal "/p:Configuration=Release-static;Platform=$platform;PlatformToolset=$PlatformToolset;SolutionDir=$winusbRoot\"
            if($LASTEXITCODE -ne 0){throw "Native RTL-SDR build failed: $project ($arch)"}
        }
    }
    # rc.exe と対象アーキテクチャの SDK/LIB を Rust のリソースビルドにも渡す。
    $environmentScript=Join-Path $winusbRoot "build\rtlsdr-runtime\developer-env-$arch.cmd"
    [IO.File]::WriteAllText($environmentScript,"@echo off`r`ncall `"$devCmd`" -no_logo -arch=$arch -host_arch=x64`r`nif errorlevel 1 exit /b %errorlevel%`r`nset`r`n",[Text.Encoding]::ASCII)
    $developerEnvironment=& cmd.exe /d /c $environmentScript
    if($LASTEXITCODE -ne 0){throw 'Visual Studio environment initialization failed.'}
    foreach($line in $developerEnvironment){
        $separator=$line.IndexOf('=')
        if($separator -gt 0){[Environment]::SetEnvironmentVariable($line.Substring(0,$separator),$line.Substring($separator+1),'Process')}
    }
    Invoke-Cargo -CargoArguments @('test','--locked','--target',$target,'--manifest-path',(Join-Path $frontend 'Cargo.toml'),'-p','rtlsdr_frontend','-p','isdbt-dsp')
    Invoke-Cargo -CargoArguments @('build','--locked','--release','--target',$target,'--manifest-path',(Join-Path $frontend 'Cargo.toml'))
    Copy-Item -LiteralPath (Join-Path $frontend "target\$target\release\rtlsdr_frontend.exe") -Destination (Join-Path $native 'rtlsdr') -Force
    if(-not $SkipPackage){
        $bits=if($arch -eq 'x86'){'32bit'}else{'64bit'}
        $destination=Join-Path $winusbRoot "dist\BonDriver_RTLSDR_$bits"
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $native 'BonDriver_PX4.dll') -Destination (Join-Path $destination 'BonDriver_RTLSDR.dll') -Force
        foreach ($file in @('DriverHost_PX4.exe','WinSCard.dll')) {
            Copy-Item -LiteralPath (Join-Path $native $file) -Destination $destination -Force
        }
        foreach($file in @('BonDriver_RTLSDR.ini','BonDriver_RTLSDR.ChSet.txt')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\BonDriver_PX4\$file") -Destination $destination -Force
        }
        foreach($file in @('DriverHost_PX4.ini','it930x-firmware.bin')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\DriverHost_PX4\$file") -Destination $destination -Force
        }
        $runtime=Join-Path $destination 'rtlsdr'
        New-Item -ItemType Directory -Force -Path $runtime | Out-Null
        foreach($file in @('rtlsdr_frontend.exe','rtlsdr.dll','pthreadVC2.dll','msvcr100.dll','vcruntime140.dll','runtime.json','COPYING.librtlsdr')){
            Copy-Item -LiteralPath (Join-Path $native "rtlsdr\$file") -Destination $runtime -Force
        }
        Copy-Item -LiteralPath (Join-Path $frontend 'LICENSE') -Destination (Join-Path $runtime 'COPYING.frontend') -Force
        $guide=Get-Content -LiteralPath (Join-Path $winusbRoot '..\docs\rtlsdr-install.md') -Raw -Encoding UTF8
        $technical=Get-Content -LiteralPath (Join-Path $winusbRoot 'docs\rtlsdr-bondriver.md') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $destination 'README.md'),$guide.Replace('(../winusb/docs/rtlsdr-bondriver.md)','(TECHNICAL.md)'),[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $destination 'TECHNICAL.md'),$technical.Replace('(../../docs/rtlsdr-install.md)','(README.md)'),[Text.UTF8Encoding]::new($false))
        Copy-Item -LiteralPath (Join-Path $winusbRoot '..\LICENSE') -Destination (Join-Path $destination 'LICENSE') -Force
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'pkg\rtlsdr\Install-Driver.ps1') -Destination $destination -Force
        $driver=Join-Path $destination 'Driver'
        New-Item -ItemType Directory -Force -Path $driver | Out-Null
        foreach($file in @('RTL2838UHIDIR.inf','px4_drv_winusb.cat','px4_drv_winusb.cer','cert-install.jse','cert-uninstall.jse')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\inf\$file") -Destination $driver -Force
        }
        foreach($file in @('verify.ps1','trustedpub.cer')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\signing-tools\$file") -Destination $driver -Force
        }
        & (Join-Path $driver 'verify.ps1') -DriverPath $driver
        $sources=Join-Path $destination 'Sources'
        New-Item -ItemType Directory -Force -Path $sources | Out-Null
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'build\rtlsdr-runtime\rtl-sdr-blog-V1.4.0-source.zip') -Destination (Join-Path $sources 'rtl-sdr-blog-V1.4.0.zip') -Force
        $sourceFrontend=Join-Path $sources 'winusb\tools\rtlsdr_frontend'
        New-Item -ItemType Directory -Force -Path $sourceFrontend,(Join-Path $sources 'winusb\src\common'),(Join-Path $sources '.cargo') | Out-Null
        foreach($file in @('Cargo.toml','Cargo.lock','cargo-config.toml','build.rs','frontend.rc','LICENSE','src','vendor')){
            Copy-Item -LiteralPath (Join-Path $frontend $file) -Destination $sourceFrontend -Recurse -Force
        }
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'src\common\version.h') -Destination (Join-Path $sources 'winusb\src\common') -Force
        Invoke-Cargo -CargoArguments @('vendor','--quiet','--locked','--manifest-path',(Join-Path $frontend 'Cargo.toml'),'--versioned-dirs',(Join-Path $sources 'vendor'))
        # --quiet は設定例も抑制するため、配布先を基準とする設定を明示して保存する。
        $configText = "[source.crates-io]`nreplace-with = `"vendored-sources`"`n`n[source.vendored-sources]`ndirectory = `"vendor`"`n"
        $configText += Get-Content -LiteralPath (Join-Path $frontend 'cargo-config.toml') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $sources '.cargo\config.toml'),$configText,[Text.UTF8Encoding]::new($false))
        Write-Host "RTL-SDR BonDriver package: $destination"
    }
}
