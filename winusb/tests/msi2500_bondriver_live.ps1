param(
    [ValidateSet('x86','x64')][string]$Architecture='x64',
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$FrontendPath,
    [ValidateRange(1,600)][int]$Seconds=30,
    [ValidateRange(1,5)][int]$Cycles=2,
    [ValidateRange(0,39)][int]$AlternateChannel=0,
    [switch]$IsolatedHost
)
# RSP1 互換機 (MSi2500) を BonDriver_PX4 と DriverHost_PX4 から受信する実機試験。
# BonDriver の取得側は RTL-SDR の試験と同じ rtlsdr_bondriver_live.exe を使う
# (TS の同期、TEI、CC、PAT の CRC、ワンセグ PMT、選局後の TSID を確かめる汎用の BonDriver クライアント)。
# 録画などで動作中の DriverHost と通信名を分けるため、通常は -IsolatedHost (PX4_ISOLATED_TEST_HOST でビルドした
# build\<arch>\rtlsdr-test の DriverHost と BonDriver) を指定する。
$ErrorActionPreference='Stop'
if(-not $IsolatedHost -and (Get-Process DriverHost_PX4 -ErrorAction SilentlyContinue)){throw 'An existing DriverHost is running. Use an isolated test build.'}
if($IsolatedHost){
    try{$mutex=[Threading.Mutex]::OpenExisting('Global\DriverHost_PX4_Mutex_RTLSDR_TEST');$mutex.Dispose();throw 'An isolated test host is already running.'}catch [Threading.WaitHandleCannotBeOpenedException]{}
}
if(Get-Process msi2500_frontend -ErrorAction SilentlyContinue){throw 'Another msi2500_frontend is running; the device can have only one owner.'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$native=Join-Path $root "build\$Architecture\Release-static"
$hostNative=if($IsolatedHost){Join-Path $root "build\$Architecture\rtlsdr-test"}else{$native}
$run=Join-Path $root ('build\msi2500-live\'+[guid]::NewGuid().ToString('N'))
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $run,(Join-Path $run 'msi2500'),$output | Out-Null
Copy-Item -LiteralPath (Join-Path $hostNative 'DriverHost_PX4.exe') -Destination $run
Copy-Item -LiteralPath (Join-Path $hostNative 'BonDriver_PX4.dll') -Destination (Join-Path $run 'BonDriver_MSI2500.dll')
Copy-Item -LiteralPath $FrontendPath -Destination (Join-Path $run 'msi2500\msi2500_frontend.exe')
foreach($file in @('BonDriver_MSI2500.ini','BonDriver_MSI2500.ChSet.txt')){
    Copy-Item -LiteralPath (Join-Path $root "pkg\BonDriver_PX4\$file") -Destination $run
}
# DriverHost_PX4.ini は日本語のコメントを含む Shift_JIS のため、その文字コードで読み、ASCII の定義部分だけを取り出す
$common=[IO.File]::ReadAllText((Join-Path $root 'pkg\DriverHost_PX4\DriverHost_PX4.ini'),[Text.Encoding]::GetEncoding(932))
$position=$common.IndexOf('[DeviceDefinition19]')
if($position -lt 0){throw 'MSi2500 device definition is missing.'}
$section=$common.Substring($position)
$next=$section.IndexOf('[DeviceDefinition20]')
if($next -ge 0){$section=$section.Substring(0,$next)}
$config="[DriverHost_PX4]`r`nPipeTimeout=30000`r`nDebugLog=true`r`n"+$section.Replace('DeviceDefinition19','DeviceDefinition0')
[IO.File]::WriteAllText((Join-Path $run 'DriverHost_PX4.ini'),$config,[Text.Encoding]::ASCII)
$hostProcess=$null
$client=$null
try{
    $hostProcess=Start-Process -FilePath (Join-Path $run 'DriverHost_PX4.exe') -WorkingDirectory $run -WindowStyle Hidden -PassThru
    $null=$hostProcess.Handle
    Start-Sleep -Milliseconds 1000
    $prefix=Join-Path $output "bon-$Architecture"
    $clientArgs=@(('"'+(Join-Path $run 'BonDriver_MSI2500.dll')+'"'),('"'+$prefix+'"'),$Seconds,$Cycles)
    if($AlternateChannel){if($Cycles -lt 2){throw 'Channel switch test needs at least two cycles.'};$clientArgs+=@($AlternateChannel)}
    # 配布設定は AsyncTune=true のため、選局要求の受付が 500 ms 以内で、その時点で未ロックであることも確かめる
    $env:RTLSDR_EXPECT_ASYNC_TUNE='1'
    $client=Start-Process -FilePath (Join-Path $native 'rtlsdr_bondriver_live.exe') -ArgumentList $clientArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.stderr.log"
    $null=$client.Handle
    $limit=$Cycles*($Seconds+55)+30
    if(-not $client.WaitForExit($limit*1000)){$client.Kill();$client.WaitForExit();throw 'BonDriver test timed out.'}
    $client.WaitForExit()
    Get-Content -LiteralPath "$prefix.log"
    if($client.ExitCode -ne 0){throw "BonDriver live test failed: $(Get-Content -LiteralPath "$prefix.stderr.log" -Raw)"}
    # helper は停止要求の後に USB の停止手順を踏んでから終わるため、少し待ってから残っていないことを確かめる
    Start-Sleep -Milliseconds 500
    if(Get-Process msi2500_frontend -ErrorAction SilentlyContinue){throw 'Frontend process remained after CloseTuner.'}
}finally{
    Remove-Item Env:\RTLSDR_EXPECT_ASYNC_TUNE -ErrorAction SilentlyContinue
    if($client){if(-not $client.HasExited){$client.Kill();$client.WaitForExit()};$client.Dispose()}
    if($hostProcess){
        if(-not $hostProcess.HasExited -and -not $hostProcess.WaitForExit(8000)){$hostProcess.Kill();$hostProcess.WaitForExit()}
        $hostProcess.Dispose()
    }
    foreach($log in @(Get-ChildItem -LiteralPath $run -Filter '*.log' -File)){
        Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $output ($Architecture+'-'+$log.Name)) -Force
    }
}
