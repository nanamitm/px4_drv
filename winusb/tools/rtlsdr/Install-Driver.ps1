param()
$ErrorActionPreference = 'Stop'
$winusbRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$logRoot = Join-Path $winusbRoot 'build\rtlsdr-install'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$log = Join-Path $logRoot 'install-driver.log'
$resultPath = Join-Path $logRoot 'install-result.json'
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run Install-Driver.ps1 as administrator.'
    }
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0BDA&PID_2838&MI_00\*' })
    if ($devices.Count -eq 0) { throw 'No RTL2838UHIDIR receive interface (MI_00) is connected.' }
    & (Join-Path $winusbRoot 'pkg\signing-tools\verify.ps1') -DriverPath (Join-Path $winusbRoot 'pkg\inf')
    # 既存の配布用証明書で検証する。証明書ストアや署名ポリシーはこのスクリプトでは変更不要。
    $inf = Join-Path $winusbRoot 'pkg\inf\RTL2838UHIDIR.inf'
    if (-not (Test-Path -LiteralPath $inf -PathType Leaf)) { throw "Driver INF not found: $inf" }
    "INF=$inf`r`nPowerShell=$($PSVersionTable.PSVersion)" | Set-Content -LiteralPath $log -Encoding utf8
    & "$env:SystemRoot\System32\pnputil.exe" '/add-driver' $inf '/install' 2>&1 | Out-File -LiteralPath $log -Encoding utf8 -Append
    $installCode = $LASTEXITCODE
    if ($installCode -ne 0 -and $installCode -ne 3010) { throw "pnputil failed: $installCode. See $log" }
    $states = @(& (Join-Path $PSScriptRoot 'Invoke-RtlSdr.ps1') -Action Status | Where-Object ReceiveInterface)
    if (@($states | Where-Object { $_.Service -ne 'WinUSB' -or $_.ProblemCode -ne 0 }).Count) {
        throw "Driver was staged but WinUSB is not active. Reconnect the RTL-SDR and inspect $log"
    }
    [ordered]@{ success = $true; pnputil = $installCode; interfaces = $states } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    $states
} catch {
    [ordered]@{ success = $false; error = $_.Exception.Message; position = $_.InvocationInfo.PositionMessage; stack = $_.ScriptStackTrace } | ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
    throw
}
