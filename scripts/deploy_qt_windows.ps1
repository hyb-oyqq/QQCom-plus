param(
    [Parameter(Mandatory = $true)]
    [string] $Binary,
    [string] $QtBin = "C:\MyProgram\Qt\6.10.1\msvc2022_64\bin",
    [switch] $IncludeOffscreen
)

$ErrorActionPreference = "Stop"

$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$windeployqt = Join-Path $QtBin "windeployqt.exe"
$qtRoot = Split-Path -Parent $QtBin

if (-not (Test-Path -LiteralPath $windeployqt)) {
    throw "找不到 windeployqt.exe：$windeployqt。请通过 -QtBin 指定 Qt 的 bin 目录。"
}

& $windeployqt --release --no-translations --no-compiler-runtime $binaryPath
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt 失败，退出码 $LASTEXITCODE。"
}

if ($IncludeOffscreen) {
    $platformDir = Join-Path (Split-Path -Parent $binaryPath) "platforms"
    New-Item -ItemType Directory -Force -Path $platformDir | Out-Null
    $offscreen = Join-Path $qtRoot "plugins\platforms\qoffscreen.dll"
    if (-not (Test-Path -LiteralPath $offscreen)) {
        throw "找不到 Qt offscreen 插件：$offscreen"
    }
    Copy-Item -LiteralPath $offscreen -Destination (Join-Path $platformDir "qoffscreen.dll") -Force
}

Write-Host "Qt 运行库已部署到 $((Split-Path -Parent $binaryPath))"
