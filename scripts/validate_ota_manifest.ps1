param(
    [Parameter(Mandatory = $true)]
    [string] $ManifestPath,
    [string] $PackagePath
)

$ErrorActionPreference = 'Stop'

$manifestFile = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json

if ([int]$manifest.schema -ne 1) { throw 'Unsupported OTA manifest schema.' }
if ([string]$manifest.product -ne 'QQCom-plus') { throw 'Manifest product must be QQCom-plus.' }
if ([string]$manifest.channel -notmatch '^(stable|beta)$') { throw 'Invalid channel.' }
if ([string]$manifest.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid version.' }

$download = $manifest.download
if ([string]$download.type -ne 'zip') { throw 'Only zip packages are supported.' }
if ([string]$download.url -notmatch '^https://') { throw 'Download URL must use HTTPS.' }
if ([int64]$download.size -le 0) { throw 'Download size must be greater than zero.' }
if ([string]$download.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid SHA-256.' }

if ($PackagePath) {
    $packageFile = (Resolve-Path -LiteralPath $PackagePath).Path
    $file = Get-Item -LiteralPath $packageFile
    if ([int64]$file.Length -ne [int64]$download.size) {
        throw "Package size mismatch: manifest=$($download.size), actual=$($file.Length)."
    }
    $actualHash = (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash
    if ($actualHash -ine [string]$download.sha256) {
        throw "Package SHA-256 mismatch: manifest=$($download.sha256), actual=$actualHash."
    }
}

Write-Host "Valid OTA manifest: $manifestFile"
if ($PackagePath) { Write-Host 'Package hash and size: OK' }
