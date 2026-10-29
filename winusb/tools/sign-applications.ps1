# SPDX-License-Identifier: GPL-2.0-only
# Run with Windows PowerShell 5.1. No private key is copied into a distribution.
[CmdletBinding()]
param([string]$DistributionPath='',
      [string]$PfxPath, [switch]$UseDriverPublisher,
      [string]$PasswordEnvironment='', [string]$PasswordFile='')
$ErrorActionPreference='Stop'
if(-not $DistributionPath){$DistributionPath=Join-Path $PSScriptRoot '..\dist'}
$signing=Join-Path $PSScriptRoot '..\pkg\application-signing'
. (Join-Path $signing 'Application-Signatures.ps1')
$distribution=(Resolve-Path -LiteralPath $DistributionPath).Path
$folders=@(Get-ChildItem -LiteralPath $distribution -Directory -Filter 'BonDriver_*')
if(-not $folders.Count){throw 'Build the distribution before signing.'}
$private=@(Get-ChildItem -LiteralPath $distribution -Recurse -File | Where-Object {$_.Extension -in '.pfx','.pvk','.key' -or $_.Name -eq 'password.dpapi'})
if($private.Count){throw 'Private signing material must stay outside distribution.'}
if($UseDriverPublisher){
    if($PfxPath -or $PasswordEnvironment -or $PasswordFile){throw 'UseDriverPublisher cannot be combined with external signing credentials.'}
    $PfxPath=Join-Path $PSScriptRoot '..\pkg\signing-tools\trustedpub.pfx'
}else{
    if(-not $PfxPath){throw 'Specify UseDriverPublisher or an external PFX.'}
    if(($PasswordEnvironment -and $PasswordFile) -or (-not $PasswordEnvironment -and -not $PasswordFile)){throw 'Specify one password source: environment variable or DPAPI password file.'}
}
$password=$null;$key=$null;$publisher=$null
try {
    if($UseDriverPublisher){
        # 上流の公開開発用 PFX と、同梱 sign.ps1 と同じ公開パスワードを使う。
        $password=ConvertTo-SecureString '123' -AsPlainText -Force
    }elseif($PasswordEnvironment){
        $value=[Environment]::GetEnvironmentVariable($PasswordEnvironment)
        if(-not $value){throw 'Signing password environment variable is empty.'}
        $password=ConvertTo-SecureString $value -AsPlainText -Force;$value=$null
    }else{
        $password=ConvertTo-SecureString ((Get-Content -LiteralPath $PasswordFile -Raw).Trim())
    }
    $publisher=Get-Px4ApplicationPublisher (Join-Path $signing 'Publisher.cer')
    $key=[Security.Cryptography.X509Certificates.X509Certificate2]::new($PfxPath,$password,[Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if(-not $key.HasPrivateKey -or [Convert]::ToBase64String($key.RawData) -cne [Convert]::ToBase64String($publisher.RawData)){throw 'Signing key differs from the pinned application publisher.'}
    Initialize-Px4ApplicationVerifier
    $identical=@{}
    foreach($folder in $folders){
        foreach($name in @(Get-Px4ApplicationFiles $folder.FullName)){
            $file=Join-Path $folder.FullName $name
            $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if($identical.ContainsKey($hash)){
                Copy-Item -LiteralPath $identical[$hash] -Destination $file -Force
            }else{
                if($UseDriverPublisher){
                    $signtool=Join-Path $PSScriptRoot '..\pkg\signing-tools\signtool.exe'
                    & $signtool sign /f $PfxPath /p 123 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $file
                    if($LASTEXITCODE -ne 0){throw "Application signing/timestamp failed: $file"}
                    $signature=Get-AuthenticodeSignature -LiteralPath $file
                    if(-not $signature.TimeStamperCertificate){throw "Application timestamp missing: $file"}
                }else{
                    $signature=Set-AuthenticodeSignature -LiteralPath $file -Certificate $key -HashAlgorithm SHA256 -IncludeChain Signer
                }
                if(-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $publisher.Thumbprint){throw "Signing failed: $file"}
                [Px4ApplicationVerifier]::Verify($file,$publisher.RawData)
                $identical[$hash]=$file
            }
        }
        Copy-Item -Path (Join-Path $signing '*') -Destination $folder.FullName -Force
        $manifest=[ordered]@{Schema=2;RootCertificateSHA256=(Get-FileHash -LiteralPath (Join-Path $signing 'Root.cer') -Algorithm SHA256).Hash;CertificateSHA256=(Get-FileHash -LiteralPath (Join-Path $signing 'Publisher.cer') -Algorithm SHA256).Hash;Files=[ordered]@{}}
        foreach($name in @(Get-Px4ApplicationFiles $folder.FullName)){$manifest.Files[$name]=(Get-FileHash -LiteralPath (Join-Path $folder.FullName $name) -Algorithm SHA256).Hash}
        $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $folder.FullName 'application-signatures.json') -Encoding ascii
        Test-Px4ApplicationSignatures $folder.FullName
    }
}finally{
    if($key){$key.Dispose()};if($publisher){$publisher.Dispose()};if($password){$password.Dispose()};$value=$null
}
