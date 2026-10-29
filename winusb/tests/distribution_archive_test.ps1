# SPDX-License-Identifier: GPL-2.0-only
$ErrorActionPreference='Stop'
$winusb=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $winusb 'tools\New-Px4DistributionArchive.ps1')
$area=Join-Path $winusb ('build\distribution-archive-test-'+[guid]::NewGuid().ToString('N'))
$source=Join-Path $area 'source'
New-Item -ItemType Directory -Path $source,(Join-Path $source '.cargo') -Force | Out-Null
$files=@{'old.txt'='old timestamp';'future.txt'='future timestamp';'.cargo/config.toml'='hidden source config';'empty.txt'=''}
foreach ($name in $files.Keys) { [IO.File]::WriteAllText((Join-Path $source $name),$files[$name],[Text.UTF8Encoding]::new($false)) }
(Get-Item -LiteralPath (Join-Path $source 'old.txt')).LastWriteTimeUtc=[datetime]::SpecifyKind([datetime]'1970-01-01',[DateTimeKind]::Utc)
(Get-Item -LiteralPath (Join-Path $source 'future.txt')).LastWriteTimeUtc=[datetime]::SpecifyKind([datetime]'2200-01-01',[DateTimeKind]::Utc)
(Get-Item -LiteralPath (Join-Path $source '.cargo') -Force).Attributes=[IO.FileAttributes]::Directory -bor [IO.FileAttributes]::Hidden
$zip=Join-Path $area 'test.zip'
New-Px4DistributionArchive $source $zip
$archive=[IO.Compression.ZipFile]::OpenRead($zip)
try {
    if ($archive.Entries.Count -ne $files.Count) { throw 'ZIP omitted a hidden or empty file.' }
    foreach ($entry in $archive.Entries) {
        if ($entry.LastWriteTime.Year -lt 1980 -or $entry.LastWriteTime.Year -gt 2107) { throw 'Invalid ZIP timestamp.' }
        $reader=New-Object IO.StreamReader($entry.Open())
        try { if ($reader.ReadToEnd() -cne $files[$entry.FullName]) { throw "ZIP changed file bytes: $($entry.FullName)" } } finally { $reader.Dispose() }
    }
} finally { $archive.Dispose() }
if ((Get-Item -LiteralPath (Join-Path $source 'old.txt')).LastWriteTimeUtc.Year -ne 1970) { throw 'Archive modified the source timestamp.' }
# 同じ出力の再生成も確認する。既存のZIPだけを置き換える。
New-Px4DistributionArchive $source $zip
Write-Host 'PASS: ZIP timestamp bounds, hidden Cargo sources, empty files, contents and source timestamp preservation.'
