param(
    [ValidateSet('Status','List','Probe','Capture')][string]$Action = 'Status',
    [ValidateSet('x86','x64')][string]$Architecture = 'x64',
    [string]$Serial = '',
    [ValidateRange(24000000,2000000000)][uint32]$FrequencyHz = 473142857,
    [ValidateRange(225001,2400000)][uint32]$SampleRate = 1015873,
    [ValidateRange(1,600)][int]$Seconds = 30,
    [ValidateRange(-60,60)][double]$GainDb,
    [string]$OutputPath,
    [ValidateRange(0,3600)][int]$TimeoutSeconds = 0
)
$ErrorActionPreference = 'Stop'
$winusbRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ($Action -eq 'Status') {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0BDA&PID_2838*' })
    foreach ($device in $devices) {
        $properties = @{}
        Get-PnpDeviceProperty -InstanceId $device.InstanceId -ErrorAction SilentlyContinue | ForEach-Object { $properties[$_.KeyName] = $_.Data }
        [pscustomobject]@{
            InstanceId = $device.InstanceId
            Name = $device.FriendlyName
            Status = $device.Status
            ProblemCode = $properties['DEVPKEY_Device_ProblemCode']
            Service = $properties['DEVPKEY_Device_Service']
            Inf = $properties['DEVPKEY_Device_DriverInfPath']
            ReceiveInterface = $device.InstanceId -like 'USB\VID_0BDA&PID_2838&MI_00\*'
        }
    }
    if ($devices.Count -eq 0) { Write-Warning 'RTL2838UHIDIR (0BDA:2838) is not connected.' }
    return
}

$runtime = Join-Path $winusbRoot "build\$Architecture\Release-static\rtlsdr"
$executable = Join-Path $runtime 'rtlsdr_probe.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Run tools/rtlsdr/build.ps1 first.' }
if ($Action -ne 'List' -and [string]::IsNullOrWhiteSpace($Serial)) { throw 'Specify -Serial from the List result. Duplicate serials require disconnecting the other devices.' }
if ($SampleRate -gt 300000 -and $SampleRate -le 900000) { throw 'RTL-SDR does not support sample rates in (300000,900000].' }

# Windows の argv 引用規則で空白・末尾バックスラッシュ・引用符を保持する。
function Quote-ProcessArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
function Invoke-ProbeProcess([string[]]$Arguments, [string]$StdoutPath, [string]$StderrPath, [int]$Limit) {
    $argumentLine = ($Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '
    $process = Start-Process -FilePath $executable -ArgumentList $argumentLine -WindowStyle Hidden -PassThru -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath
    # Windows PowerShell 5.1 でも終了コードを取得できるよう、終了前にハンドルを保持する。
    $null = $process.Handle
    try {
        if (-not $process.WaitForExit($Limit * 1000)) {
            # 自分が作成した診断プロセスだけを停止する。既存の SDR/録画プロセスは対象外。
            $process.Kill()
            $process.WaitForExit()
            throw "RTL-SDR operation timed out after $Limit seconds. Partial capture, if any, was retained. See $StderrPath"
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "RTL-SDR operation failed (exit $($process.ExitCode)): $(Get-Content -LiteralPath $StderrPath -Raw)" }
        Get-Content -LiteralPath $StdoutPath -Raw | ConvertFrom-Json
    } finally { $process.Dispose() }
}

if ($Action -ne 'Capture') {
    $logRoot = Join-Path $winusbRoot 'build\rtlsdr-diagnostics'
    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    $prefix = Join-Path $logRoot ([guid]::NewGuid().ToString('N'))
    $arguments = if ($Action -eq 'List') { @('list') } else { @('probe',$Serial) }
    $limit = if ($TimeoutSeconds) { $TimeoutSeconds } else { 30 }
    Invoke-ProbeProcess $arguments "$prefix.json" "$prefix.stderr.log" $limit
    return
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) { throw 'Capture requires -OutputPath.' }
$output = [IO.Path]::GetFullPath($OutputPath)
$partial = "$output.partial"
$metadata = "$output.json"
foreach ($path in @($output,$partial,$metadata,"$output.failed.json","$output.process.json","$output.stderr.log")) {
    if (Test-Path -LiteralPath $path) { throw "Capture output already exists: $path" }
}
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($output)) -PathType Container)) { throw 'Output directory must already exist.' }
$samples = [uint64]$SampleRate * [uint64]$Seconds
$gain = if ($PSBoundParameters.ContainsKey('GainDb')) { ([int][math]::Round($GainDb * 10)).ToString([Globalization.CultureInfo]::InvariantCulture) } else { 'auto' }
$arguments = @('capture',$Serial,$FrequencyHz.ToString(),$SampleRate.ToString(),$samples.ToString(),$gain,$partial)
$limit = if ($TimeoutSeconds) { $TimeoutSeconds } else { $Seconds + 30 }
try {
    $result = Invoke-ProbeProcess $arguments "$output.process.json" "$output.stderr.log" $limit
    if ((Get-Item -LiteralPath $partial).Length -ne $samples * 2 -or $result.bytes -ne $samples * 2) {
        throw 'Capture length does not match the requested CU8 sample count.'
    }
    # 完了したファイルだけを正式名にする。途中終了した raw を成功結果と混同しない。
    Move-Item -LiteralPath $partial -Destination $output -ErrorAction Stop
    $result | Add-Member -NotePropertyName sha256 -NotePropertyValue (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
    $result | Add-Member -NotePropertyName path -NotePropertyValue $output
    $result | Add-Member -NotePropertyName captured_at_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    $result | Add-Member -NotePropertyName runtime -NotePropertyValue (Get-Content -LiteralPath (Join-Path $runtime 'runtime.json') -Raw | ConvertFrom-Json)
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $metadata -Encoding UTF8
    $result
} catch {
    [ordered]@{ success = $false; error = $_.Exception.Message; partial = $partial; expected_bytes = $samples * 2 } | ConvertTo-Json | Set-Content -LiteralPath "$output.failed.json" -Encoding UTF8
    throw
}
