param(
    [Parameter(Mandatory = $true)]
    [string] $Binary,
    [string] $QtBin = "C:\MyProgram\Qt\6.10.1\msvc2022_64\bin",
    [ValidateSet("Debug", "Release")]
    [string] $Configuration = "Release",
    [switch] $IncludeOffscreen
)

$ErrorActionPreference = "Stop"

$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$windeployqt = Join-Path $QtBin "windeployqt.exe"
$qtRoot = Split-Path -Parent $QtBin

if (-not (Test-Path -LiteralPath $windeployqt)) {
    throw "Cannot find windeployqt.exe: $windeployqt. Specify the Qt kit bin directory with -QtBin."
}

$deployMode = "--" + $Configuration.ToLowerInvariant()
& $windeployqt $deployMode --qtpaths (Join-Path $QtBin "qtpaths.exe") --no-translations --no-compiler-runtime $binaryPath
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE."
}

if ($IncludeOffscreen) {
    $platformDir = Join-Path (Split-Path -Parent $binaryPath) "platforms"
    New-Item -ItemType Directory -Force -Path $platformDir | Out-Null
    $offscreenName = if ($Configuration -eq "Debug") { "qoffscreend.dll" } else { "qoffscreen.dll" }
    $offscreen = Join-Path $qtRoot "plugins\platforms\$offscreenName"
    if (-not (Test-Path -LiteralPath $offscreen)) {
        throw "Cannot find the Qt offscreen plugin: $offscreen"
    }
    Copy-Item -LiteralPath $offscreen -Destination (Join-Path $platformDir $offscreenName) -Force
}

$updater = Join-Path $PSScriptRoot 'qqcom-updater.ps1'
if (-not (Test-Path -LiteralPath $updater -PathType Leaf)) {
    throw "Cannot find the updater script: $updater"
}
Copy-Item -LiteralPath $updater -Destination (Join-Path (Split-Path -Parent $binaryPath) 'qqcom-updater.ps1') -Force

Write-Host "Qt $Configuration runtime deployed to $((Split-Path -Parent $binaryPath))"
