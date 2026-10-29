param([string]$SigningPfxPath='', [string]$SigningPasswordEnvironment='', [string]$SigningPasswordFile='', [switch]$RequireSignedApplications)


# カレントディレクトリに移動
if ($MyInvocation.MyCommand.Path -ne $null) {
    $CurrentPath = (Split-Path $MyInvocation.MyCommand.Path -Parent)
} else {
    $CurrentPath = $PSScriptRoot
}
Set-Location $CurrentPath

# コンソールウインドウのサイズを変更
$ErrorActionPreference = 'Stop'  # エラーを確実に catch する
try { (Get-Host).UI.RawUI.BufferSize = New-Object System.Management.Automation.Host.Size(120,25) } catch {}
try { (Get-Host).UI.RawUI.WindowSize = New-Object System.Management.Automation.Host.Size(120,25) } catch {}

# MSBuild のパスを環境変数 PATH に追加
# GitHub Actions では microsoft/setup-msbuild が PATH を設定するため、その設定を優先する
# ローカル環境では Visual Studio 2022 のエディション差を吸収するため、vswhere で MSBuild を探索する
if ((Get-Command msbuild -ErrorAction SilentlyContinue) -eq $null) {
    $vswhere_path = Join-Path -Path ${env:ProgramFiles(x86)} -ChildPath 'Microsoft Visual Studio\Installer\vswhere.exe'

    if ((Test-Path $vswhere_path) -eq $True) {
        $msbuild_file_path = & $vswhere_path -latest -version '[17.0,18.0)' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1

        if ($msbuild_file_path -ne $null) {
            $msbuild_dir_path = Split-Path $msbuild_file_path -Parent
            $env:PATH = "$env:PATH;$msbuild_dir_path"
        }
    }
}

if ((Get-Command msbuild -ErrorAction SilentlyContinue) -eq $null) {
    throw 'MSBuild was not found. Install Visual Studio 2022 with MSBuild.'
}

# カタログファイルと INF の不一致をビルド前に検出し、署名対象と異なるドライバーパッケージの生成を防ぐ
& 'pkg/signing-tools/verify.ps1' -DriverPath 'pkg/inf'

# MSBuild を使用してソリューションをビルド
$build_platforms = @('x86', 'x64')
foreach ($build_platform in $build_platforms) {
    # PX-BCUD の USB 分割境界・I2C 故障注入は実機接続なしで検証する。
    foreach ($bcud_test in @('pxbcud_frontend_test', 'pxbcud_usb_probe', 'pxbcud_live_test', 'pxbcud_usb_diagnostic', 'channel_switch_stress')) {
        msbuild "tests/$bcud_test.vcxproj" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
        if ($LASTEXITCODE -ne 0) { throw "PX-BCUD test build failed: $bcud_test ($build_platform)" }
    }
    & "build/$build_platform/Release-static/pxbcud_frontend_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "PX-BCUD frontend test failed ($build_platform)" }
    msbuild px4_winusb.sln /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed. platform: $build_platform"
    }

    # Linux 版と WinUSB 版が共有する TS 同期判定を実際の入力値で検証
    msbuild tests/ts_sync_condition_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "TS sync condition test build failed. platform: $build_platform"
    }
    & 'tests/ts_sync_condition_test.ps1' -Platform $build_platform

    # 実機に依存しないカード抜去・再挿入・接触不良の状態遷移を毎回検証
    & "build/$build_platform/Release-static/smart_card_state_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Smart card state test failed. platform: $build_platform"
    }

    # 無入力・送信待機中もストリーム停止要求を回収できることを検証
    msbuild tests/stream_stop_test.vcxproj /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Stream stop test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/stream_stop_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Stream stop test failed. platform: $build_platform" }

    msbuild tests/stream_epoch_test.vcxproj /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Stream epoch test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/stream_epoch_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Stream epoch test failed. platform: $build_platform" }

    msbuild tests/pipe_shutdown_test.vcxproj /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Pipe shutdown test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/pipe_shutdown_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Pipe shutdown test failed. platform: $build_platform" }

    msbuild tests/card_status_wait_test.vcxproj /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Card status wait test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/card_status_wait_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Card status wait test failed. platform: $build_platform" }

    msbuild tests/card_status_lock_test.vcxproj /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Card status lock test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/card_status_lock_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Card status lock test failed. platform: $build_platform" }

    # 選局時の TS バッファ初期化が読み書きと競合しても止まらないことを毎回検証
    & "build/$build_platform/Release-static/ringbuffer_purge_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Ring buffer purge test failed. platform: $build_platform"
    }
    # torneの復調器はホスト内のC++実装。Go不要の模擬試験で状態遷移と境界を検証する。
    msbuild tests/torne_native_frontend_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) { throw "Native torne frontend test build failed. platform: $build_platform" }
    & "build/$build_platform/Release-static/torne_native_frontend_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Native torne frontend test failed. platform: $build_platform" }

    foreach ($torne_test in @('torne_foundation_test', 'torne_bootstrap_test')) {
        msbuild "tests/$torne_test.vcxproj" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
        if ($LASTEXITCODE -ne 0) { throw "Torne test build failed: $torne_test ($build_platform)" }
        & "build/$build_platform/Release-static/$torne_test.exe"
        if ($LASTEXITCODE -ne 0) { throw "Torne test failed: $torne_test ($build_platform)" }
    }

    # 共通受信機の変更が RTL-SDR の非同期選局と helper の通信形式を壊していないことを検証
    foreach ($rtl_test in @('receiver_tune_completion_test', 'rtlsdr_protocol_test')) {
        msbuild "tests/$rtl_test.vcxproj" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
        if ($LASTEXITCODE -ne 0) { throw "RTL-SDR test build failed: $rtl_test ($build_platform)" }
        & "build/$build_platform/Release-static/$rtl_test.exe"
        if ($LASTEXITCODE -ne 0) { throw "RTL-SDR test failed: $rtl_test ($build_platform)" }
    }

    # 模擬 I2C で ISDB-C・J.83B の設定と失敗時の復帰を検証
    & "build/$build_platform/Release-static/j83b_driver_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "J83B driver test failed. platform: $build_platform"
    }

    # 作った TS で、TSMF の TS 番号による選択・ヘッダーのない搬送波の素通し・分割 TLV の自動判定を検証
    & "build/$build_platform/Release-static/tsmf_filter_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "TSMF filter test failed. platform: $build_platform"
    }

    # ISDB-C と J.83B の ChSet の列の解釈と不正な行の扱い、配布の ChSet がすべて読めることを検証
    & "build/$build_platform/Release-static/chset_isdbc_test.exe" "pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt" "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt"
    if ($LASTEXITCODE -ne 0) {
        throw "ISDB-C ChSet test failed. platform: $build_platform"
    }
}

# dist/ フォルダにビルドされたファイルをコピー
# フォルダの作成
$distribution_path = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'dist'))
if ($distribution_path -ne (Join-Path ([IO.Path]::GetFullPath($PSScriptRoot)) 'dist')) {
    throw 'Unexpected distribution path.'
}
if (Test-Path -LiteralPath $distribution_path) {
    if ((Get-Item -LiteralPath $distribution_path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing to replace a redirected distribution directory.'
    }
    Remove-Item -LiteralPath $distribution_path -Recurse -Force
}
New-Item -ItemType Directory dist/
New-Item -ItemType Directory dist/BonDriver_PX4_32bit
New-Item -ItemType Directory dist/BonDriver_PX4_64bit
New-Item -ItemType Directory dist/BonDriver_PX-MLT_32bit
New-Item -ItemType Directory dist/BonDriver_PX-MLT_64bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056_32bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056_64bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056N_32bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056N_64bit
New-Item -ItemType Directory dist/BonDriver_ISDBT2071_32bit
New-Item -ItemType Directory dist/BonDriver_ISDBT2071_64bit
New-Item -ItemType Directory dist/BonDriver_PX-M1UR_32bit
New-Item -ItemType Directory dist/BonDriver_PX-M1UR_64bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UR_32bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UR_64bit

# BonDriver_PX4_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_32bit/BonDriver_PX4-T.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ini dist/BonDriver_PX4_32bit/BonDriver_PX4-T.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX4_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_32bit/BonDriver_PX4-S.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ini dist/BonDriver_PX4_32bit/BonDriver_PX4-S.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX4_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX4_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX4_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX4_32bit/it930x-firmware.bin

# BonDriver_PX4_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_64bit/BonDriver_PX4-T.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ini dist/BonDriver_PX4_64bit/BonDriver_PX4-T.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX4_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_64bit/BonDriver_PX4-S.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ini dist/BonDriver_PX4_64bit/BonDriver_PX4-S.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX4_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX4_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX4_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX4_64bit/it930x-firmware.bin

# BonDriver_PX-MLT_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-MLT_32bit/BonDriver_PX-MLT.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-MLT.ini dist/BonDriver_PX-MLT_32bit/BonDriver_PX-MLT.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-C.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-J83B.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-MLT_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-MLT_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-MLT_32bit/it930x-firmware.bin

# BonDriver_PX-MLT_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-MLT_64bit/BonDriver_PX-MLT.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-MLT.ini dist/BonDriver_PX-MLT_64bit/BonDriver_PX-MLT.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-C.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-J83B.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-MLT_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-MLT_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-MLT_64bit/it930x-firmware.bin

# BonDriver_ISDB2056_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056_32bit/BonDriver_ISDB2056.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056.ini dist/BonDriver_ISDB2056_32bit/BonDriver_ISDB2056.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056_32bit/it930x-firmware.bin

# BonDriver_ISDB2056_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056_64bit/BonDriver_ISDB2056.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056.ini dist/BonDriver_ISDB2056_64bit/BonDriver_ISDB2056.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056_64bit/it930x-firmware.bin

# BonDriver_ISDB2056N_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056N_32bit/BonDriver_ISDB2056N.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056N.ini dist/BonDriver_ISDB2056N_32bit/BonDriver_ISDB2056N.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056N_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056N_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056N_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056N_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056N_32bit/it930x-firmware.bin

# BonDriver_ISDB2056N_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056N_64bit/BonDriver_ISDB2056N.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056N.ini dist/BonDriver_ISDB2056N_64bit/BonDriver_ISDB2056N.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056N_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056N_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056N_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056N_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056N_64bit/it930x-firmware.bin

# BonDriver_PX-M1UR_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-M1UR_32bit/BonDriver_PX-M1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-M1UR.ini dist/BonDriver_PX-M1UR_32bit/BonDriver_PX-M1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-M1UR_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-M1UR_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-M1UR_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-M1UR_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-M1UR_32bit/it930x-firmware.bin

# BonDriver_PX-M1UR_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-M1UR_64bit/BonDriver_PX-M1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-M1UR.ini dist/BonDriver_PX-M1UR_64bit/BonDriver_PX-M1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-M1UR_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-M1UR_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-M1UR_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-M1UR_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-M1UR_64bit/it930x-firmware.bin

# BonDriver_ISDBT2071_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDBT2071_32bit/BonDriver_ISDBT2071.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDBT2071.ini dist/BonDriver_ISDBT2071_32bit/BonDriver_ISDBT2071.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDBT2071_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDBT2071_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDBT2071_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDBT2071_32bit/it930x-firmware.bin

# BonDriver_ISDBT2071_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDBT2071_64bit/BonDriver_ISDBT2071.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDBT2071.ini dist/BonDriver_ISDBT2071_64bit/BonDriver_ISDBT2071.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDBT2071_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDBT2071_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDBT2071_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDBT2071_64bit/it930x-firmware.bin

# BonDriver_PX-S1UR_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UR_32bit/BonDriver_PX-S1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UR.ini dist/BonDriver_PX-S1UR_32bit/BonDriver_PX-S1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UR_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UR_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UR_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UR_32bit/it930x-firmware.bin

# BonDriver_PX-S1UR_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UR_64bit/BonDriver_PX-S1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UR.ini dist/BonDriver_PX-S1UR_64bit/BonDriver_PX-S1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UR_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UR_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UR_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UR_64bit/it930x-firmware.bin

# PX-BCUD も通常配布へ含める。給電非対応と世代付き TS 配信の設定を同梱する。
foreach ($bcud_arch in @(@('x86', '32bit'), @('x64', '64bit'))) {
    $bcud_platform = $bcud_arch[0]
    $bcud_folder = "dist/BonDriver_PX-BCUD_$($bcud_arch[1])"
    New-Item -ItemType Directory -Path $bcud_folder | Out-Null
    Copy-Item "build/$bcud_platform/Release-static/BonDriver_PX4.dll" "$bcud_folder/BonDriver_PX-BCUD.dll"
    Copy-Item "build/$bcud_platform/Release-static/DriverHost_PX4.exe" $bcud_folder
    Copy-Item 'pkg/BonDriver_PX4/BonDriver_PX-BCUD.ini' $bcud_folder
    Copy-Item 'pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt' $bcud_folder
    Copy-Item 'pkg/DriverHost_PX4/DriverHost_PX4.ini' $bcud_folder
    Copy-Item '../docs/px-bcud-winusb.md' "$bcud_folder/README.md"
    Copy-Item '../LICENSE' $bcud_folder
}

# 各 BonDriver と同じビット数の WinSCard.dll を配置
# WinUSB 版の全対応機種で内蔵カードリーダーを利用できる
Get-ChildItem dist/ -Directory -Filter 'BonDriver_*' | ForEach-Object {
    $win_scard_platform = if ($_.Name.EndsWith('_32bit')) { 'x86' } else { 'x64' }
    Copy-Item "build/$win_scard_platform/Release-static/WinSCard.dll" $_.FullName
}

# CECH-ZD1J用配布物と共通ライセンス文書を配置する。
python tools/package_torne.py
if ($LASTEXITCODE -ne 0) {
    throw 'Torne packaging failed.'
}

# inf ファイルをコピー
Copy-Item -Recurse pkg/inf/ dist/Driver

# コピー後の最終配布物も再度検査
& 'pkg/signing-tools/verify.ps1' -DriverPath 'dist/Driver'

python tools/check_pxbcud_package.py
if ($LASTEXITCODE -ne 0) { throw 'PX-BCUD package check failed.' }
python tests/pxbcud_package_test.py
if ($LASTEXITCODE -ne 0) { throw 'PX-BCUD package validation regression failed.' }

# アプリ署名は CAT と別の公開証明書へ固定する。通常の開発ビルドは従来どおり
# 署名キー不要とし、配布用途では検証に成功した成果物だけを渡す。
if ($SigningPfxPath) {
    $sign_arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'tools/sign-applications.ps1'),'-PfxPath',$SigningPfxPath)
    if ($SigningPasswordEnvironment) { $sign_arguments += @('-PasswordEnvironment',$SigningPasswordEnvironment) }
    if ($SigningPasswordFile) { $sign_arguments += @('-PasswordFile',$SigningPasswordFile) }
    & powershell.exe @sign_arguments
    if ($LASTEXITCODE -ne 0) { throw 'Application signing failed.' }
}
if ($RequireSignedApplications -or $SigningPfxPath) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/verify-applications.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Application signature verification failed.' }
}

Write-Host '    '
Write-Host '    '
Write-Host '    BonDriver のビルドとパッケージングを完了しました。'
Write-Host '    ビルドしたファイルは dist/ フォルダに配置されています。'
Write-Host '    '
Start-Sleep -Seconds 5
