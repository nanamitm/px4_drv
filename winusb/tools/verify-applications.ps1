# SPDX-License-Identifier: GPL-2.0-only
param([string]$DistributionPath='')
$ErrorActionPreference='Stop'
if(-not $DistributionPath){$DistributionPath=Join-Path $PSScriptRoot '..\dist'}
. (Join-Path $PSScriptRoot '..\pkg\application-signing\Application-Signatures.ps1')
$folders=@(Get-ChildItem -LiteralPath $DistributionPath -Directory -Filter 'BonDriver_*')
if(-not $folders.Count){throw 'No BonDriver distribution folders.'}
foreach($folder in $folders){Test-Px4ApplicationSignatures $folder.FullName}
$private=@(Get-ChildItem -LiteralPath $DistributionPath -Recurse -File | Where-Object {$_.Extension -in '.pfx','.pvk','.key' -or $_.Name -eq 'password.dpapi'})
if($private.Count){throw 'Private signing material in distribution.'}
Write-Host "PASS: signed application distributions ($($folders.Count) folders)"
