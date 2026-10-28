param(
    [ValidateSet('x86','x64')][string]$Architecture='x64',
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,120)][int]$Seconds=15,
    [ValidateRange(1,5)][int]$Cycles=2,
    [switch]$IsolatedHost
)
$ErrorActionPreference='Stop'
if(-not $IsolatedHost -and (Get-Process DriverHost_PX4 -ErrorAction SilentlyContinue)){throw 'An existing DriverHost is running. Use an isolated test build.'}
if($IsolatedHost){
    try{$mutex=[Threading.Mutex]::OpenExisting('Global\DriverHost_PX4_Mutex_RTLSDR_TEST');$mutex.Dispose();throw 'An isolated RTL-SDR test host is already running.'}catch [Threading.WaitHandleCannotBeOpenedException]{}
}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$native=Join-Path $root "build\$Architecture\Release-static"
$hostNative=if($IsolatedHost){Join-Path $root "build\$Architecture\rtlsdr-test"}else{$native}
$run=Join-Path $root ('build\rtlsdr-live\'+[guid]::NewGuid().ToString('N'))
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $run,(Join-Path $run 'rtlsdr'),$output | Out-Null
Copy-Item -LiteralPath (Join-Path $hostNative 'DriverHost_PX4.exe') -Destination $run
Copy-Item -LiteralPath (Join-Path $hostNative 'BonDriver_PX4.dll') -Destination (Join-Path $run 'BonDriver_RTLSDR.dll')
foreach($file in @('rtlsdr_frontend.exe','rtlsdr.dll','pthreadVC2.dll','msvcr100.dll')){
    Copy-Item -LiteralPath (Join-Path $native "rtlsdr\$file") -Destination (Join-Path $run 'rtlsdr')
}
foreach($file in @('BonDriver_RTLSDR.ini','BonDriver_RTLSDR.ChSet.txt')){
    Copy-Item -LiteralPath (Join-Path $root "pkg\BonDriver_PX4\$file") -Destination $run
}
$common=[IO.File]::ReadAllText((Join-Path $root 'pkg\DriverHost_PX4\DriverHost_PX4.ini'))
$position=$common.IndexOf('[DeviceDefinition17]')
if($position -lt 0){throw 'RTL-SDR device definition is missing.'}
$config="[DriverHost_PX4]`r`nPipeTimeout=30000`r`nDebugLog=true`r`n"+$common.Substring($position).Replace('DeviceDefinition17','DeviceDefinition0')
[IO.File]::WriteAllText((Join-Path $run 'DriverHost_PX4.ini'),$config,[Text.Encoding]::ASCII)
$hostProcess=$null
$client=$null
try{
    $hostProcess=Start-Process -FilePath (Join-Path $run 'DriverHost_PX4.exe') -WorkingDirectory $run -WindowStyle Hidden -PassThru
    $null=$hostProcess.Handle
    Start-Sleep -Milliseconds 1000
    $prefix=Join-Path $output "bon-$Architecture"
    $client=Start-Process -FilePath (Join-Path $native 'rtlsdr_bondriver_live.exe') -ArgumentList @(('"'+(Join-Path $run 'BonDriver_RTLSDR.dll')+'"'),('"'+$prefix+'"'),$Seconds,$Cycles) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.stderr.log"
    $null=$client.Handle
    $limit=$Cycles*($Seconds+55)+30
    if(-not $client.WaitForExit($limit*1000)){$client.Kill();$client.WaitForExit();throw 'BonDriver test timed out.'}
    $client.WaitForExit()
    Get-Content -LiteralPath "$prefix.log"
    if($client.ExitCode -ne 0){throw "BonDriver live test failed: $(Get-Content -LiteralPath "$prefix.stderr.log" -Raw)"}
    if(Get-Process rtlsdr_frontend -ErrorAction SilentlyContinue){throw 'Frontend process remained after CloseTuner.'}
}finally{
    if($client){if(-not $client.HasExited){$client.Kill();$client.WaitForExit()};$client.Dispose()}
    if($hostProcess){
        if(-not $hostProcess.HasExited -and -not $hostProcess.WaitForExit(3000)){$hostProcess.Kill();$hostProcess.WaitForExit()}
        $hostProcess.Dispose()
    }
    foreach($log in @(Get-ChildItem -LiteralPath $run -Filter '*.log' -File)){
        Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $output ($Architecture+'-'+$log.Name)) -Force
    }
}
