# SPDX-License-Identifier: GPL-2.0-only
param([string]$DistributionPath='')
$ErrorActionPreference='Stop'
if(-not $DistributionPath){$DistributionPath=Join-Path $PSScriptRoot '..\dist'}
$winusb=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$source=Join-Path (Resolve-Path -LiteralPath $DistributionPath).Path 'BonDriver_Torne_64bit'
$area=Join-Path $winusb ('build\application-signature-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $area | Out-Null
. (Join-Path $winusb 'pkg\application-signing\Application-Signatures.ps1')
function Assert([bool]$ok,[string]$message){if(-not $ok){throw $message}}
function Rejected([scriptblock]$call,[string]$message){$failed=$false;try{& $call | Out-Null}catch{$failed=$true};Assert $failed $message}
function Snapshot {return (@(Get-ChildItem Cert:\CurrentUser\Root,Cert:\CurrentUser\TrustedPublisher,Cert:\CurrentUser\My,Cert:\LocalMachine\Root,Cert:\LocalMachine\TrustedPublisher | ForEach-Object {$_.PSParentPath+'|'+$_.Thumbprint} | Sort-Object) -join "`n")}
function Fixture([string]$name){$path=Join-Path $area $name;Copy-Item -LiteralPath $source -Destination $path -Recurse;return $path}
function RunTrust([string]$path,[string]$action,[switch]$WhatIf){
    $script=Join-Path $path 'Trust-Applications.ps1'
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName='powershell.exe';$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$script+'" -Directory "'+$path+'" -Action '+$action
    if($WhatIf){$info.Arguments+=' -WhatIf'}
    $process=[Diagnostics.Process]::Start($info)
    try {
        $output=$process.StandardOutput.ReadToEndAsync();$errors=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(30000)){$process.Kill();throw 'Trust verification entry point timed out.'}
        ($output.Result+$errors.Result) | Set-Content -LiteralPath (Join-Path $area ($action+'-'+[guid]::NewGuid().ToString('N')+'.log'))
        return $process.ExitCode
    }finally{$process.Dispose()}
}
function RewriteHash([string]$path){
    $file=Join-Path $path 'application-signatures.json';$manifest=Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    $manifest.Files.'DriverHost_PX4.exe'=(Get-FileHash -LiteralPath (Join-Path $path 'DriverHost_PX4.exe') -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $file -Encoding ascii
}
$before=Snapshot
& (Join-Path $winusb 'tools\verify-applications.ps1') -DistributionPath $DistributionPath
$missing=Fixture 'missing';Remove-Item -LiteralPath (Join-Path $missing 'WinSCard.dll')
Rejected {Test-Px4ApplicationSignatures $missing} 'Missing required DLL accepted.'
$tampered=Fixture 'tampered-rewritten-manifest';$file=Join-Path $tampered 'DriverHost_PX4.exe'
$bytes=[IO.File]::ReadAllBytes($file);$pe=[BitConverter]::ToInt32($bytes,0x3c)
$section=$pe+24+[BitConverter]::ToUInt16($bytes,$pe+20);$raw=[BitConverter]::ToInt32($bytes,$section+20)
$bytes[$raw]=$bytes[$raw] -bxor 1;[IO.File]::WriteAllBytes($file,$bytes);RewriteHash $tampered
Rejected {Test-Px4ApplicationSignatures $tampered} 'Modified PE code accepted after rewriting its manifest hash.'
Assert ((RunTrust $tampered Trust -WhatIf) -ne 0) 'Trust entry point accepted a modified PE.'
$unsigned=Fixture 'unsigned-rewritten-manifest';$file=Join-Path $unsigned 'DriverHost_PX4.exe'
$bytes=[IO.File]::ReadAllBytes($file);$pe=[BitConverter]::ToInt32($bytes,0x3c)
$directory=$pe+24+112+4*8;[Array]::Clear($bytes,$directory,8);[IO.File]::WriteAllBytes($file,$bytes);RewriteHash $unsigned
Rejected {Test-Px4ApplicationSignatures $unsigned} 'Removed signature accepted after rewriting manifest.'
$extra=Fixture 'unexpected-manifest-entry';$file=Join-Path $extra 'application-signatures.json';$manifest=Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
$manifest.Files | Add-Member -NotePropertyName '../other.exe' -NotePropertyValue ('0'*64)
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $file -Encoding ascii
Rejected {Test-Px4ApplicationSignatures $extra} 'Traversal or extra manifest entry accepted.'
$omitted=Fixture 'omitted-manifest-entry';$file=Join-Path $omitted 'application-signatures.json';$manifest=Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
$manifest.Files.PSObject.Properties.Remove('BonDriver_Torne.dll');$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $file -Encoding ascii
Rejected {Test-Px4ApplicationSignatures $omitted} 'Unsigned manifest could omit a real BonDriver.'
$wrong=Fixture 'wrong-publisher';Copy-Item -LiteralPath (Join-Path $winusb 'pkg\inf\px4_drv_winusb.cer') -Destination (Join-Path $wrong 'Publisher.cer') -Force
Rejected {Test-Px4ApplicationSignatures $wrong} 'Other publisher certificate accepted.'
Rejected {[Px4ApplicationVerifier]::Verify((Join-Path $source 'DriverHost_PX4.exe'),[byte[]]@(1,2,3))} 'PE verifier accepted a different signer.'
Assert ((RunTrust $source Verify) -eq 0) 'Distribution Verify entry point failed.'
Assert ((RunTrust $source Trust -WhatIf) -eq 0) 'Valid WhatIf trust entry point failed.'
# helper の欠落やマニフェストの書き換えを、トップレベルの EXE と同じ基準で拒否する。
$rtlSource=Join-Path (Resolve-Path -LiteralPath $DistributionPath).Path 'BonDriver_RTLSDR_64bit'
$rtlMissing=Join-Path $area 'rtl-missing-helper'
Copy-Item -LiteralPath $rtlSource -Destination $rtlMissing -Recurse
Remove-Item -LiteralPath (Join-Path $rtlMissing 'rtlsdr\rtlsdr_frontend.exe')
Rejected {Test-Px4ApplicationSignatures $rtlMissing} 'RTL-SDR helper omission accepted.'
$rtlTampered=Join-Path $area 'rtl-tampered-helper'
Copy-Item -LiteralPath $rtlSource -Destination $rtlTampered -Recurse
$file=Join-Path $rtlTampered 'rtlsdr\rtlsdr_frontend.exe'
$bytes=[IO.File]::ReadAllBytes($file);$pe=[BitConverter]::ToInt32($bytes,0x3c)
$section=$pe+24+[BitConverter]::ToUInt16($bytes,$pe+20);$raw=[BitConverter]::ToInt32($bytes,$section+20)
$bytes[$raw]=$bytes[$raw] -bxor 1;[IO.File]::WriteAllBytes($file,$bytes)
$manifestPath=Join-Path $rtlTampered 'application-signatures.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.Files.'rtlsdr\rtlsdr_frontend.exe'=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding ascii
Rejected {Test-Px4ApplicationSignatures $rtlTampered} 'Modified RTL-SDR helper accepted after rewriting its manifest hash.'
Assert ((Snapshot) -ceq $before) 'Tests changed certificate stores.'
Write-Host "PASS: all signed folders, tampering, rewritten/omitted manifests, unsigned PE, pinned signer and unchanged stores. Evidence: $area"
