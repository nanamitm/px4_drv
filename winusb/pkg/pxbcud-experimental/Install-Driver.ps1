$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'install-driver.log'
$resultPath = Join-Path $PSScriptRoot 'install-result.txt'
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script as administrator.'
    }
    & pnputil.exe /add-driver (Join-Path $PSScriptRoot 'Driver\PX-BCUD.inf') /install 2>&1 | Out-File -LiteralPath $log -Encoding utf8
    $installCode = $LASTEXITCODE
    if ($installCode -ne 0 -and $installCode -ne 3010) { throw "pnputil failed with exit code $installCode. See install-driver.log." }
    "SUCCESS pnputil=$installCode" | Set-Content -LiteralPath $resultPath -Encoding utf8
} catch {
    $_ | Out-String | Add-Content -LiteralPath $log -Encoding utf8
    "FAILURE $_" | Set-Content -LiteralPath $resultPath -Encoding utf8
    exit 1
}
