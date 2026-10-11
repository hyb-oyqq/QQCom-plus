# Refresh the build stamp inside the VER MANAGER block of version.h.
# QQCOM_VERSION_MAJOR / MINOR / PATCH stay as edited in the header.
param(
    [string] $Root = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $PSScriptRoot
}

$headerPath = Join-Path $Root "version\version.h"
if (-not (Test-Path -LiteralPath $headerPath)) {
    throw "Version header not found: $headerPath"
}

function Read-HeaderText([string] $Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $utf8 = New-Object System.Text.UTF8Encoding $false
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return $utf8.GetString($bytes, 3, $bytes.Length - 3)
    }
    return $utf8.GetString($bytes)
}

function Get-DefineNumber([string] $Text, [string] $Name) {
    $match = [regex]::Match($Text, "(?m)^#define\s+$Name\s+(\d+)")
    if (-not $match.Success) {
        throw "missing #define $Name"
    }
    return [int]$match.Groups[1].Value
}

function Set-DefineToken([string] $Text, [string] $Name, [string] $Value) {
    $match = [regex]::Match($Text, "(?m)^(#define\s+$Name\s+)(\S+)")
    if (-not $match.Success) {
        throw "missing #define $Name"
    }
    $start = $match.Groups[2].Index
    $len = $match.Groups[2].Length
    return $Text.Remove($start, $len).Insert($start, $Value)
}

function Set-DefineString([string] $Text, [string] $Name, [string] $Value) {
    $match = [regex]::Match($Text, "(?m)^(#define\s+$Name\s+"")([^""]*)("")")
    if (-not $match.Success) {
        throw "missing #define $Name"
    }
    $start = $match.Groups[2].Index
    $len = $match.Groups[2].Length
    return $Text.Remove($start, $len).Insert($start, $Value)
}

$text = Read-HeaderText $headerPath
$blockMatch = [regex]::Match($text, '(?s)//\*+\s*VER MANAGER START\s*\*+.*?//\*+\s*VER MANAGER END\s*\*+')
if (-not $blockMatch.Success) {
    throw "VER MANAGER block not found in $headerPath"
}

$block = $blockMatch.Value
$major = Get-DefineNumber $block "QQCOM_VERSION_MAJOR"
$minor = Get-DefineNumber $block "QQCOM_VERSION_MINOR"
$patch = Get-DefineNumber $block "QQCOM_VERSION_PATCH"
$version = "$major.$minor.$patch"
$timestamp = Get-Date -Format "yyyyMMddHHmm"
$buildText = "v$version Build:$timestamp"

$block = Set-DefineString $block "QQCOM_VERSION_STR" $version
$block = Set-DefineString $block "QQCOM_BUILD_TIMESTAMP" $timestamp
$block = Set-DefineString $block "QQCOM_BUILD_STR" $buildText
$block = Set-DefineString $block "QQCOM_APP_TITLE" "QQCom $buildText"

$text = $text.Remove($blockMatch.Index, $blockMatch.Length).Insert($blockMatch.Index, $block)

$utf8NoBom = New-Object System.Text.UTF8Encoding $false
$tmp = "$headerPath.tmp"
[System.IO.File]::WriteAllText($tmp, $text, $utf8NoBom)
Move-Item -LiteralPath $tmp -Destination $headerPath -Force

Write-Host "Version: $version | Build: $timestamp"
