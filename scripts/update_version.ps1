param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet("build", "major", "minor", "patch")]
    [string] $Mode = "build"
)

$ErrorActionPreference = "Stop"

$versionDir = Join-Path $Root "version"
$manifestPath = Join-Path $versionDir "version.json"
$headerPath = Join-Path $versionDir "version.h"

if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Version manifest not found: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$major = [int]$manifest.major
$minor = [int]$manifest.minor
$patch = [int]$manifest.patch

switch ($Mode) {
    "major" {
        $major++
        $minor = 0
        $patch = 0
    }
    "minor" {
        $minor++
        $patch = 0
    }
    "patch" {
        $patch++
    }
}

if ($Mode -ne "build") {
    $manifest.major = $major
    $manifest.minor = $minor
    $manifest.patch = $patch
    $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

$version = "$major.$minor.$patch"
$timestamp = Get-Date -Format "yyyyMMddHHmm"
$buildText = "v$version Build:$timestamp"
$header = @"
#ifndef EASYDEBUG_VERSION_H
#define EASYDEBUG_VERSION_H

#define EASYDEBUG_VERSION_MAJOR $major
#define EASYDEBUG_VERSION_MINOR $minor
#define EASYDEBUG_VERSION_PATCH $patch
#define EASYDEBUG_VERSION_STR "$version"
#define EASYDEBUG_BUILD_TIMESTAMP "$timestamp"
#define EASYDEBUG_BUILD_STR "$buildText"
#define EASYDEBUG_APP_TITLE "easyDebug $buildText"

#endif // EASYDEBUG_VERSION_H
"@

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($headerPath, $header.TrimStart(), $utf8NoBom)
Write-Host "Version: $version | Build: $timestamp | Mode: $Mode"
