param(
    [ValidateSet('x86','x64')][string[]]$Architecture = @('x86','x64'),
    [ValidatePattern('^v[0-9]+$')][string]$PlatformToolset = 'v143',
    [switch]$SkipNativeBuild,
    [switch]$SkipPackage
)
# RSP1 互換機 (MSi2500) の BonDriver 配布物を作る。
# helper (msi2500_frontend.exe) は外部の DLL を使わない静的リンクの単一実行ファイルで、
# USB ドライバーは SDRplay のものを使う前提のため INF は同梱しない。
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
$frontend=Join-Path $winusbRoot 'tools\msi2500_frontend'
# helper は RTL-SDR の helper に同梱した isdbt-dsp を誤り訂正の部品として使う (Cargo.toml の path 依存)
$dsp=Join-Path $winusbRoot 'tools\rtlsdr_frontend\vendor\isdbt-dsp'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if(-not $visualStudio){throw 'Visual Studio C++ tools and Windows SDK are required.'}
$msbuild=Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
$devCmd=Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
$work=Join-Path $winusbRoot 'build\msi2500-runtime'
New-Item -ItemType Directory -Force -Path $work | Out-Null
foreach($arch in $Architecture){
    $platform=if($arch -eq 'x86'){'Win32'}else{'x64'}
    $target=if($arch -eq 'x86'){'i686-pc-windows-msvc'}else{'x86_64-pc-windows-msvc'}
    $native=Join-Path $winusbRoot "build\$arch\Release-static"
    if(-not $SkipNativeBuild){
        foreach($project in @('DriverHost_PX4','BonDriver_PX4','WinSCard_PX4')){
            & $msbuild (Join-Path $winusbRoot "src\$project\$project.vcxproj") /nologo /v:minimal "/p:Configuration=Release-static;Platform=$platform;PlatformToolset=$PlatformToolset;SolutionDir=$winusbRoot\"
            if($LASTEXITCODE -ne 0){throw "Native MSi2500 build failed: $project ($arch)"}
        }
    }
    # rc.exe と対象アーキテクチャの SDK/LIB を Rust のリソースビルドにも渡す。
    $environmentScript=Join-Path $work "developer-env-$arch.cmd"
    [IO.File]::WriteAllText($environmentScript,"@echo off`r`ncall `"$devCmd`" -no_logo -arch=$arch -host_arch=x64`r`nif errorlevel 1 exit /b %errorlevel%`r`nset`r`n",[Text.Encoding]::ASCII)
    $developerEnvironment=& cmd.exe /d /c $environmentScript
    if($LASTEXITCODE -ne 0){throw 'Visual Studio environment initialization failed.'}
    foreach($line in $developerEnvironment){
        $separator=$line.IndexOf('=')
        if($separator -gt 0){[Environment]::SetEnvironmentVariable($line.Substring(0,$separator),$line.Substring($separator+1),'Process')}
    }
    # 録画 IQ を使う試験は MSI2500_TEST_IQ を指定したときだけ動くため、通常のビルドでは自動試験だけを実行する
    Invoke-Cargo -CargoArguments @('test','--locked','--release','--target',$target,'--manifest-path',(Join-Path $frontend 'Cargo.toml'))
    Invoke-Cargo -CargoArguments @('build','--locked','--release','--target',$target,'--manifest-path',(Join-Path $frontend 'Cargo.toml'))
    $helperOut=Join-Path $native 'msi2500'
    New-Item -ItemType Directory -Force -Path $helperOut | Out-Null
    Copy-Item -LiteralPath (Join-Path $frontend "target\$target\release\msi2500_frontend.exe") -Destination $helperOut -Force
    if(-not $SkipPackage){
        $bits=if($arch -eq 'x86'){'32bit'}else{'64bit'}
        $destination=Join-Path $winusbRoot "dist\BonDriver_MSI2500_$bits"
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $native 'BonDriver_PX4.dll') -Destination (Join-Path $destination 'BonDriver_MSI2500.dll') -Force
        foreach ($file in @('DriverHost_PX4.exe','WinSCard.dll')) {
            Copy-Item -LiteralPath (Join-Path $native $file) -Destination $destination -Force
        }
        # J.83B のチャンネル定義は PX-MLT 系と共通のファイルを使う
        foreach($file in @('BonDriver_MSI2500.ini','BonDriver_MSI2500.ChSet.txt','BonDriver_PX4-J83B.ChSet.txt')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\BonDriver_PX4\$file") -Destination $destination -Force
        }
        foreach($file in @('DriverHost_PX4.ini','it930x-firmware.bin')){
            Copy-Item -LiteralPath (Join-Path $winusbRoot "pkg\DriverHost_PX4\$file") -Destination $destination -Force
        }
        $runtime=Join-Path $destination 'msi2500'
        New-Item -ItemType Directory -Force -Path $runtime | Out-Null
        Copy-Item -LiteralPath (Join-Path $helperOut 'msi2500_frontend.exe') -Destination $runtime -Force
        Copy-Item -LiteralPath (Join-Path $frontend 'LICENSE') -Destination (Join-Path $runtime 'COPYING.frontend') -Force
        $guide=Get-Content -LiteralPath (Join-Path $winusbRoot '..\docs\msi2500-install.md') -Raw -Encoding UTF8
        $technical=Get-Content -LiteralPath (Join-Path $winusbRoot 'docs\msi2500-fullseg-design.md') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $destination 'README.md'),$guide.Replace('(../winusb/docs/msi2500-fullseg-design.md)','(TECHNICAL.md)'),[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $destination 'TECHNICAL.md'),$technical.Replace('(../../docs/msi2500-install.md)','(README.md)'),[Text.UTF8Encoding]::new($false))
        # J.83B の技術資料は README.md (導入手順) から TECHNICAL-J83B.md として参照する
        $guideText=[IO.File]::ReadAllText((Join-Path $destination 'README.md'),[Text.Encoding]::UTF8)
        [IO.File]::WriteAllText((Join-Path $destination 'README.md'),$guideText.Replace('(../winusb/docs/msi2500-j83b-design.md)','(TECHNICAL-J83B.md)'),[Text.UTF8Encoding]::new($false))
        $technicalJ83b=Get-Content -LiteralPath (Join-Path $winusbRoot 'docs\msi2500-j83b-design.md') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $destination 'TECHNICAL-J83B.md'),$technicalJ83b.Replace('(msi2500-fullseg-design.md)','(TECHNICAL.md)'),[Text.UTF8Encoding]::new($false))
        Copy-Item -LiteralPath (Join-Path $winusbRoot '..\LICENSE') -Destination (Join-Path $destination 'LICENSE') -Force
        # GPL の helper を再ビルドできるよう、helper と isdbt-dsp のソースと Cargo の依存をオフライン用に同梱する。
        # isdbt-dsp は Cargo.toml の相対パス (../rtlsdr_frontend/vendor/isdbt-dsp) と同じ位置へ置く。
        $sources=Join-Path $destination 'Sources'
        if(Test-Path -LiteralPath $sources){Remove-Item -LiteralPath $sources -Recurse -Force}
        $sourceFrontend=Join-Path $sources 'winusb\tools\msi2500_frontend'
        $sourceDsp=Join-Path $sources 'winusb\tools\rtlsdr_frontend\vendor\isdbt-dsp'
        New-Item -ItemType Directory -Force -Path $sourceFrontend,$sourceDsp,(Join-Path $sources 'winusb\src\common'),(Join-Path $sources '.cargo') | Out-Null
        foreach($file in @('Cargo.toml','Cargo.lock','cargo-config.toml','build.rs','frontend.rc','LICENSE','src','tests')){
            Copy-Item -LiteralPath (Join-Path $frontend $file) -Destination $sourceFrontend -Recurse -Force
        }
        foreach($file in @('Cargo.toml','src')){
            Copy-Item -LiteralPath (Join-Path $dsp $file) -Destination $sourceDsp -Recurse -Force
        }
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'tools\rtlsdr_frontend\LICENSE') -Destination (Join-Path $sources 'winusb\tools\rtlsdr_frontend\LICENSE') -Force
        # isdbt-dsp の出所 (上流のリポジトリとリビジョン) とライセンスの説明
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'tools\rtlsdr_frontend\vendor\README.md') -Destination (Join-Path $sources 'winusb\tools\rtlsdr_frontend\vendor\README.md') -Force
        Copy-Item -LiteralPath (Join-Path $winusbRoot 'src\common\version.h') -Destination (Join-Path $sources 'winusb\src\common') -Force
        Invoke-Cargo -CargoArguments @('vendor','--quiet','--locked','--manifest-path',(Join-Path $frontend 'Cargo.toml'),'--versioned-dirs',(Join-Path $sources 'vendor'))
        # --quiet は設定例も抑制するため、配布先を基準とする設定を明示して保存する。
        $configText = "[source.crates-io]`nreplace-with = `"vendored-sources`"`n`n[source.vendored-sources]`ndirectory = `"vendor`"`n"
        $configText += Get-Content -LiteralPath (Join-Path $frontend 'cargo-config.toml') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $sources '.cargo\config.toml'),$configText,[Text.UTF8Encoding]::new($false))
        Write-Host "MSi2500 BonDriver package: $destination"
    }
}
