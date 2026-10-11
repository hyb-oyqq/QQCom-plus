param(
    [Parameter(Mandatory = $true)]
    [string] $BinaryDir,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^https://')]
    [string] $BaseUrl,
    [string] $OutputDir = "build/ota",
    [ValidateSet('stable', 'beta')]
    [string] $Channel = 'stable'
)

$ErrorActionPreference = 'Stop'

function Get-ReleaseNotes {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Version,
        [string] $NotesPath
    )

    if ($NotesPath -and (Test-Path -LiteralPath $NotesPath -PathType Leaf)) {
        $text = [System.IO.File]::ReadAllText($NotesPath).TrimEnd()
        if ($text.Length -gt 0) { return $text + "`r`n" }
    }

    $placeholder = '# V' + $Version + "`r`n`r`n" + ([System.Text.Encoding]::UTF8.GetString([byte[]](0xE6,0x9C,0xAC,0xE6,0xAC,0xA1,0xE5,0x8F,0x91,0xE5,0xB8,0x83)))
    return $placeholder + "`r`n"
}

$binaryRoot = (Resolve-Path -LiteralPath $BinaryDir).Path
$exePath = Join-Path $binaryRoot 'QQCom.exe'
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "QQCom.exe was not found in release directory: $binaryRoot"
}

if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    $outputRoot = [System.IO.Path]::GetFullPath($OutputDir)
} else {
    $outputRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputDir))
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$packageName = "QQCom-$Version-win64.zip"
$packagePath = Join-Path $outputRoot $packageName
if (Test-Path -LiteralPath $packagePath) { Remove-Item -LiteralPath $packagePath -Force }

$stage = Join-Path $outputRoot ("stage-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Path (Join-Path $binaryRoot '*') -Destination $stage -Recurse -Force
$updaterSource = Join-Path $PSScriptRoot 'qqcom-updater.ps1'
Copy-Item -LiteralPath $updaterSource -Destination (Join-Path $stage 'qqcom-updater.ps1') -Force
# GBK treats 0xB4 as a line-continuation lead byte, so build the name from safe decimals.
$notesName = -join ([char]0x56FA, [char]0x4EF6, [char]0x66F4, [char]0x65B0, [char]0x8BB0, [char]0x5F55) + '.md'
$recordName = -join ([char]0x66F4, [char]0x65B0, [char]0x8BB0, [char]0x5F55) + '.md'
$recordPath = Join-Path (Split-Path -Parent $PSScriptRoot) $recordName
$notesText = Get-ReleaseNotes -Version $Version -NotesPath $recordPath
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$notesPath = Join-Path $stage $notesName
[System.IO.File]::WriteAllText($notesPath, $notesText, $utf8NoBom)
[System.IO.File]::WriteAllText((Join-Path $outputRoot $notesName), $notesText, $utf8NoBom)
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $packagePath -CompressionLevel Optimal
Remove-Item -LiteralPath $stage -Recurse -Force
$packageInfo = Get-Item -LiteralPath $packagePath
$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    schema = 1
    product = 'QQCom'
    channel = $Channel
    version = $Version
    releaseDate = [DateTime]::UtcNow.ToString('o')
    mandatory = $false
    download = [ordered]@{
        type = 'zip'
        url = "$($BaseUrl.TrimEnd('/'))/$packageName"
        size = [int64]$packageInfo.Length
        sha256 = $hash
    }
    notesUrl = "https://gitee.com/qinjmin/qqcom/releases/tag/v$Version"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $names = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
} finally {
    $zip.Dispose()
}
if ($names -notcontains 'QQCom.exe') {
    throw 'Package does not contain QQCom.exe at the archive root.'
}
if ($names -notcontains $notesName) {
    throw 'Package does not contain the release notes at the archive root.'
}
foreach ($name in $names) {
    $parts = $name.Split('/')
    if ($name.StartsWith('/') -or $name.Contains(':') -or ($parts -contains '..')) {
        throw "Unsafe package entry: $name"
    }
}

$manifestPath = Join-Path $outputRoot 'manifest.json'
$json = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Package:  $packagePath"
Write-Host "Manifest: $manifestPath"
Write-Host "Notes:    $(Join-Path $outputRoot $notesName)"
Write-Host "SHA-256:  $hash"
