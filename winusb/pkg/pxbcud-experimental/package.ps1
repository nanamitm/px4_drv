param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
$winusbRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$destination=[IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path $destination | Out-Null
& (Join-Path $PSScriptRoot '..\signing-tools\verify.ps1') -DriverPath (Join-Path $winusbRoot 'pkg\inf')
New-Item -ItemType Directory -Force -Path (Join-Path $destination 'Driver') | Out-Null
Copy-Item -Path (Join-Path $winusbRoot 'pkg\inf\*') -Destination (Join-Path $destination 'Driver') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-Driver.ps1') -Destination $destination -Force
foreach ($architecture in @('x86','x64')) {
    $target=Join-Path $destination $architecture
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $binary=Join-Path $winusbRoot "build\$architecture\Release-static"
    foreach ($file in @('DriverHost_PX4.exe','pxbcud_live_test.exe','pxbcud_usb_probe.exe','channel_switch_stress.exe')) {
        Copy-Item -LiteralPath (Join-Path $binary $file) -Destination $target -Force
    }
    Copy-Item -LiteralPath (Join-Path $binary 'BonDriver_PX4.dll') -Destination (Join-Path $target 'BonDriver_PX-BCUD.dll') -Force
    foreach ($file in @('DriverHost_PX4.ini','BonDriver_PX-BCUD.ini')) {
        $configurationFolder=if ($file -eq 'DriverHost_PX4.ini') { 'pkg\DriverHost_PX4' } else { 'pkg\BonDriver_PX4' }
        Copy-Item -LiteralPath (Join-Path (Join-Path $winusbRoot $configurationFolder) $file) -Destination $target -Force
    }
    Copy-Item -LiteralPath (Join-Path $winusbRoot 'pkg\BonDriver_PX4\BonDriver_PX4-S.ChSet.txt') -Destination $target -Force
}
Copy-Item -LiteralPath (Join-Path $winusbRoot '..\docs\px-bcud-winusb.md') -Destination (Join-Path $destination 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $winusbRoot '..\LICENSE') -Destination $destination -Force
& (Join-Path $PSScriptRoot '..\signing-tools\verify.ps1') -DriverPath (Join-Path $destination 'Driver')
Write-Host "PX-BCUD experimental package: $destination"
