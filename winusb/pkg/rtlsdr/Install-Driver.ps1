$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Run this script in an administrator PowerShell.'
}
& (Join-Path $PSScriptRoot 'Driver\verify.ps1') -DriverPath (Join-Path $PSScriptRoot 'Driver')
& "$env:SystemRoot\System32\pnputil.exe" '/add-driver' (Join-Path $PSScriptRoot 'Driver\RTL2838UHIDIR.inf') '/install'
if($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010){throw "Driver installation failed: $LASTEXITCODE"}
$devices=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'USB\VID_0BDA&PID_2838&MI_00\*'})
if(-not $devices.Count){throw 'Driver was staged; connect the RTL-SDR receive interface and check its status.'}
foreach($device in $devices){
    $service=Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service'
    $problem=Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
    if($service.Data -ne 'WinUSB' -or $problem.Data -ne 0){throw "WinUSB is not active on $($device.InstanceId)"}
}
Write-Host 'RTL2838UHIDIR receive interface is using WinUSB.'
