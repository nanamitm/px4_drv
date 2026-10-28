# SPDX-License-Identifier: GPL-2.0-only
# Windows PowerShell 5.1. Package already-built, signed distributions and source.
param([string]$Version='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$winusb=Join-Path $root 'winusb';$dist=Join-Path $winusb 'dist'
$revision=(& git -C $root rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Cannot identify source revision.'}
if(-not $Version){$Version=$revision.Substring(0,12)}
if($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$'){throw 'Unsafe package version.'}
$changes=@(& git -C $root status --porcelain --untracked-files=normal)
if($LASTEXITCODE -ne 0 -or $changes.Count){throw 'Commit source changes before packaging matching source.'}
& (Join-Path $PSScriptRoot 'verify-applications.ps1') -DistributionPath $dist
& (Join-Path $winusb 'pkg\signing-tools\verify.ps1') -DriverPath (Join-Path $dist 'Driver')
$release=Join-Path $winusb "build\release\$Version"
New-Item -ItemType Directory -Path $release -Force | Out-Null
[ordered]@{Version=$Version;SourceRevision=$revision;ApplicationPublisherSHA256='5B6288396CA6D34EFFE2B1DBFDA3B6EA8B861E08FBFDD7F83356AA1EC594A1EE'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dist 'RELEASE.json') -Encoding ascii
$binaryZip=Join-Path $release "px4_drv_winusb-$Version.zip"
$sourceZip=Join-Path $release "px4_drv_winusb-$Version-source.zip"
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $binaryZip -Force
& git -C $root archive --format=zip "--output=$sourceZip" HEAD
if($LASTEXITCODE -ne 0){throw 'Corresponding source archive failed.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::Open($sourceZip,[IO.Compression.ZipArchiveMode]::Update)
try {
    # 既存 PX4 開発用キーも含め、再配布用ソース ZIP には署名キーを含めない。
    foreach($entry in @($archive.Entries)){
        if($entry.Name -match '(?i)\.(pfx|pvk|key)$' -or $entry.Name -eq 'password.dpapi'){$entry.Delete()}
        elseif($entry.FullName -match '^winusb/pkg/inf/[^/]+\.inf$'){
            # git archive は .gitattributes のチェックアウト時 CRLF 変換を行わない。
            # INF だけを署名時と同じバイト列へ戻し、ZIP 展開からのビルドでも CAT を検証可能にする。
            $name=$entry.FullName;$sourceStream=$entry.Open();$buffer=New-Object IO.MemoryStream
            try {$sourceStream.CopyTo($buffer);$bytes=$buffer.ToArray()}finally{$sourceStream.Dispose();$buffer.Dispose()}
            $normalized=New-Object IO.MemoryStream
            try {
                for($i=0;$i -lt $bytes.Length;$i++){
                    if($bytes[$i] -eq 13){$normalized.WriteByte(13);$normalized.WriteByte(10);if($i+1 -lt $bytes.Length -and $bytes[$i+1] -eq 10){$i++}}
                    elseif($bytes[$i] -eq 10){$normalized.WriteByte(13);$normalized.WriteByte(10)}
                    else{$normalized.WriteByte($bytes[$i])}
                }
                $content=$normalized.ToArray()
            }finally{$normalized.Dispose()}
            $entry.Delete();$replacement=$archive.CreateEntry($name);$targetStream=$replacement.Open()
            try{$targetStream.Write($content,0,$content.Length)}finally{$targetStream.Dispose()}
        }
    }
}finally{$archive.Dispose()}
$verify=Join-Path $release ('verify-'+[guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $binaryZip -DestinationPath $verify
& (Join-Path $PSScriptRoot 'verify-applications.ps1') -DistributionPath $verify
& (Join-Path $winusb 'pkg\signing-tools\verify.ps1') -DriverPath (Join-Path $verify 'Driver')
$sourceVerify=Join-Path $release ('verify-source-'+[guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $sourceZip -DestinationPath $sourceVerify
& (Join-Path $winusb 'pkg\signing-tools\verify.ps1') -DriverPath (Join-Path $sourceVerify 'winusb\pkg\inf')
Get-FileHash -LiteralPath $binaryZip,$sourceZip -Algorithm SHA256 |
    ForEach-Object {'{0}  {1}' -f $_.Hash.ToLowerInvariant(),(Split-Path $_.Path -Leaf)} |
    Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii
Write-Host "PASS: signed binary ZIP, corresponding source, extracted signatures and driver catalog: $release"
