$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Run this script in an administrator PowerShell.'
}
& (Join-Path $PSScriptRoot 'Driver\verify.ps1') -DriverPath (Join-Path $PSScriptRoot 'Driver')
& "$env:SystemRoot\System32\pnputil.exe" '/add-driver' (Join-Path $PSScriptRoot 'Driver\MSI2500.inf') '/install'
if($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010){throw "Driver installation failed: $LASTEXITCODE"}
$devices=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'USB\VID_1DF7&PID_2500\*'})
if(-not $devices.Count){throw 'Driver was staged; connect the RSP1 compatible receiver and check its status.'}
foreach($device in $devices){
    $service=Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service'
    $problem=Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
    if($service.Data -ne 'WinUSB' -or $problem.Data -ne 0){throw "WinUSB is not active on $($device.InstanceId)"}
    # SDRplay passes its WHQL-signed package through Windows ranking first. Both packages bind the same
    # winusb.sys with the same interface GUID, so the provider is reported rather than treated as an error.
    $provider=Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverProvider'
    Write-Host "$($device.InstanceId): WinUSB (driver package provider: $($provider.Data))"
}
Write-Host 'RSP1 compatible receiver is using WinUSB.'
