# SPDX-License-Identifier: GPL-2.0-only
# Windows PowerShell 5.1. Distribution-folder verification precedes trust changes.
[CmdletBinding(SupportsShouldProcess=$true)]
param([ValidateSet('Verify','Trust','Untrust')][string]$Action='Verify',
      [ValidateSet('CurrentUser','LocalMachine')][string]$Scope='CurrentUser',
      [string]$Directory='')
$ErrorActionPreference='Stop'
if(-not $Directory){$Directory=$PSScriptRoot}
. (Join-Path $PSScriptRoot 'Application-Signatures.ps1')
$directory=(Resolve-Path -LiteralPath $Directory).Path
$certificatePath=Join-Path $directory 'Publisher.cer'
if($Action -ne 'Untrust'){Test-Px4ApplicationSignatures $directory $certificatePath}
if($Action -eq 'Verify'){return}
$publisher=Get-Px4ApplicationPublisher $certificatePath
try {
    foreach($name in @('Root','TrustedPublisher')){
        $storePath="Cert:\$Scope\$name"
        if($Action -eq 'Trust'){
            if($PSCmdlet.ShouldProcess($storePath,"Trust application publisher $($publisher.Thumbprint)")){
                Import-Certificate -FilePath $certificatePath -CertStoreLocation $storePath | Out-Null
            }
        }else{
            $entry=Join-Path $storePath $publisher.Thumbprint
            if((Test-Path -LiteralPath $entry) -and $PSCmdlet.ShouldProcess($entry,'Remove application publisher trust')){
                Remove-Item -LiteralPath $entry
            }
        }
    }
}finally{$publisher.Dispose()}
