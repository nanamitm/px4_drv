param(
    [ValidateSet('x86','x64')][string[]]$Architecture = @('x86','x64'),
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143',
    [switch]$SkipNativeBuild,
    [switch]$SkipPackage
)
$ErrorActionPreference='Stop'
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
        foreach($project in @('DriverHost_PX4','BonDriver_PX4')){
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
    & cargo build --locked --release --target $target --manifest-path (Join-Path $frontend 'Cargo.toml')
    if($LASTEXITCODE -ne 0){throw "RTL-SDR frontend build failed: $arch (install rustup target $target if missing)"}
    Copy-Item -LiteralPath (Join-Path $frontend "target\$target\release\rtlsdr_frontend.exe") -Destination (Join-Path $native 'rtlsdr') -Force
    if(-not $SkipPackage){
        $bits=if($arch -eq 'x86'){'32bit'}else{'64bit'}
        $destination=Join-Path $winusbRoot "dist\BonDriver_RTLSDR_$bits"
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $native 'BonDriver_PX4.dll') -Destination (Join-Path $destination 'BonDriver_RTLSDR.dll') -Force
        Copy-Item -LiteralPath (Join-Path $native 'DriverHost_PX4.exe') -Destination $destination -Force
        foreach($file in @('BonDriver_RTLSDR.ini','BonDriver_RTLSDR.ChSet.txt')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\BonDriver_PX4\$file") -Destination $destination -Force
        }
        foreach($file in @('DriverHost_PX4.ini','it930x-firmware.bin')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\DriverHost_PX4\$file") -Destination $destination -Force
        }
        $runtime=Join-Path $destination 'rtlsdr'
        New-Item -ItemType Directory -Force -Path $runtime | Out-Null
        foreach($file in @('rtlsdr_frontend.exe','rtlsdr.dll','pthreadVC2.dll','msvcr100.dll','runtime.json')){
            Copy-Item -LiteralPath (Join-Path $native "rtlsdr\$file") -Destination $runtime -Force
        }
        Copy-Item -LiteralPath (Join-Path $frontend 'LICENSE') -Destination (Join-Path $runtime 'COPYING.frontend') -Force
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'docs\rtlsdr-bondriver.md') -Destination (Join-Path $destination 'README.md') -Force
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
        $sourceFrontend=Join-Path $sources 'winusb\tools\rtlsdr_frontend'
        New-Item -ItemType Directory -Force -Path $sourceFrontend,(Join-Path $sources 'winusb\src\common'),(Join-Path $sources '.cargo') | Out-Null
        foreach($file in @('Cargo.toml','Cargo.lock','build.rs','frontend.rc','LICENSE','src','vendor')){
            Copy-Item -LiteralPath (Join-Path $frontend $file) -Destination $sourceFrontend -Recurse -Force
        }
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'src\common\version.h') -Destination (Join-Path $sources 'winusb\src\common') -Force
        $vendorConfig=& cargo vendor --locked --manifest-path (Join-Path $frontend 'Cargo.toml') --versioned-dirs (Join-Path $sources 'vendor')
        if($LASTEXITCODE -ne 0){throw 'Frontend source vendoring failed.'}
        $configText=($vendorConfig -join "`n") -replace '(?m)^directory = .*$', 'directory = "vendor"'
        [IO.File]::WriteAllText((Join-Path $sources '.cargo\config.toml'),$configText+"`n",[Text.UTF8Encoding]::new($false))
        Write-Host "RTL-SDR BonDriver package: $destination"
    }
}
