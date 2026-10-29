param([string]$Serial = '')
$ErrorActionPreference = 'Stop'
$winusbRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$invoke = Join-Path $winusbRoot 'tools\rtlsdr\Invoke-RtlSdr.ps1'
$testRoot = Join-Path $winusbRoot ('build\rtlsdr-tests\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$passed = 0
function Expect-Failure([scriptblock]$Operation, [string]$MessagePattern) {
    $failed = $false
    try { & $Operation | Out-Null } catch {
        if ($_.Exception.Message -notmatch $MessagePattern) { throw }
        $failed = $true
    }
    if (-not $failed) { throw "Expected failure: $MessagePattern" }
}

# 個体の列挙番号を推測した操作、レートの禁止帯域、出力先不備を USB オープン前に拒否する。
Expect-Failure { & $invoke -Action Probe } 'Specify -Serial'
Expect-Failure { & $invoke -Action Capture -Serial 'test' -SampleRate 800000 -OutputPath (Join-Path $testRoot 'invalid.cu8') } 'sample rates'
Expect-Failure { & $invoke -Action Capture -Serial 'test' } 'OutputPath'
$passed += 3

foreach ($arch in @('x86','x64')) {
    $binary = Join-Path $winusbRoot "build\$arch\Release-static\rtlsdr\rtlsdr_probe.exe"
    # CLI の整数解析は不正値と桁あふれをライブラリーのロード前に拒否する。
    foreach ($frequency in @('-1','999999999999999999999999999','473142857x')) {
        $savedPreference = $ErrorActionPreference
        try {
            # Windows PowerShell 5.1 は native stderr を ErrorRecord に変換する。
            $ErrorActionPreference = 'Continue'
            & $binary capture test $frequency 1015873 10 auto (Join-Path $testRoot 'invalid.cu8') 2>&1 | Out-File (Join-Path $testRoot "$arch-invalid.log")
        } finally { $ErrorActionPreference = $savedPreference }
        if ($LASTEXITCODE -eq 0) { throw "Invalid numeric value was accepted: $arch / $frequency" }
        $passed++
    }
    if ($Serial) {
        $probe = & $invoke -Action Probe -Serial $Serial -Architecture $arch
        if ($probe.serial -ne $Serial -or $probe.tuner -eq 'Unknown') { throw 'Device identity/tuner probe failed.' }
        # Unicode と空白を含むファイル名で Windows の引数引用・ファイル API も検証する。
        $path = Join-Path $testRoot "$arch capture 受信.cu8"
        $capture = & $invoke -Action Capture -Serial $Serial -Architecture $arch -FrequencyHz 473142857 -SampleRate 1015873 -Seconds 2 -GainDb 30 -OutputPath $path
        if ($capture.bytes -ne 4063492 -or $capture.samples -ne 2031746) { throw "Wrong capture size: $arch" }
        if ($capture.automatic_gain -or $null -eq $capture.gain_tenths_db) { throw 'Manual gain was not recorded.' }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $capture.sha256) { throw 'Capture hash mismatch.' }
        Expect-Failure { & $invoke -Action Capture -Serial $Serial -Architecture $arch -OutputPath $path } 'already exists'
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hash) { throw 'Existing capture was modified.' }
        $passed += 3
    }
}
if ($Serial) {
    $timeoutPath = Join-Path $testRoot 'timeout.cu8'
    Expect-Failure { & $invoke -Action Capture -Serial $Serial -Seconds 10 -TimeoutSeconds 1 -OutputPath $timeoutPath } 'timed out'
    if (Test-Path -LiteralPath $timeoutPath) { throw 'Timed-out capture was promoted to final output.' }
    $failure = Get-Content -LiteralPath "$timeoutPath.failed.json" -Raw | ConvertFrom-Json
    if ($failure.success -ne $false) { throw 'Timeout was recorded as success.' }
    $recovery = & $invoke -Action Capture -Serial $Serial -Seconds 2 -OutputPath (Join-Path $testRoot 'recovery.cu8')
    if ($recovery.bytes -ne 4063492) { throw 'Capture after timeout failed.' }
    $passed += 2
}
[pscustomobject]@{ passed = $passed; live_device = [bool]$Serial; results = $testRoot }
