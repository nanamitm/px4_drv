# SPDX-License-Identifier: GPL-2.0-only
function New-Px4DistributionArchive([string]$SourceDirectory, [string]$DestinationPath) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source=(Resolve-Path -LiteralPath $SourceDirectory).Path.TrimEnd('\')
    $destination=[IO.Path]::GetFullPath($DestinationPath)
    if ($destination.StartsWith($source+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'ZIP output must be outside its source directory.' }
    if (Test-Path -LiteralPath $destination) { [IO.File]::Delete($destination) }
    $archive=[IO.Compression.ZipFile]::Open($destination,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in @(Get-ChildItem -LiteralPath $source -Recurse -File -Force | Sort-Object FullName)) {
            $name=$file.FullName.Substring($source.Length+1).Replace('\','/')
            $entry=$archive.CreateEntry($name,[IO.Compression.CompressionLevel]::Optimal)
            # Cargo の配布ソースには1970年の日時がある。ZIP のDOS日時は1980〜2107年のため、
            # アーカイブ内だけ範囲へ収め、元ファイルの日時・バイト列は変更しない。
            $stamp=$file.LastWriteTimeUtc
            if ($stamp.Year -lt 1980 -or $stamp.Year -gt 2107) { $stamp=[datetime]::SpecifyKind([datetime]'1980-01-01',[DateTimeKind]::Utc) }
            $entry.LastWriteTime=[DateTimeOffset]$stamp
            $inputStream=$null;$outputStream=$null
            try {
                $inputStream=[IO.File]::OpenRead($file.FullName)
                $outputStream=$entry.Open()
                $inputStream.CopyTo($outputStream)
            } finally {
                if ($outputStream) { $outputStream.Dispose() }
                if ($inputStream) { $inputStream.Dispose() }
            }
        }
    } finally { $archive.Dispose() }
}
