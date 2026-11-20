# QQCom-plus updater. After the main process exits, extract the Release zip,
# replace files in the install directory, and restart. No rollback, no
# scripts from the archive, and no changes to the user-data directory.
param(
    [Parameter(Mandatory = $true)]
    [int] $ProcessId,
    [Parameter(Mandatory = $true)]
    [string] $Package,
    [Parameter(Mandatory = $true)]
    [string] $InstallDir,
    [Parameter(Mandatory = $true)]
    [string] $Restart,
    [int] $WaitSeconds = 60,
    [string] $LogPath = '',
    [string] $UserDataDir = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path ([System.IO.Path]::GetTempPath()) 'QQCom-plus-update\update.log'
}
if ([string]::IsNullOrWhiteSpace($UserDataDir)) {
    $UserDataDir = Join-Path $env:LOCALAPPDATA 'QQCom-plus\QQCom-plus'
}

$logDirectory = Split-Path -Parent $LogPath
if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
}

function Write-UpdateLog([string] $Message) {
    $line = "$(Get-Date -Format o) $Message"
    Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8
}

function Test-PathInside([string] $Outer, [string] $Inner) {
    $outerFull = [System.IO.Path]::GetFullPath($Outer).TrimEnd('\')
    $innerFull = [System.IO.Path]::GetFullPath($Inner).TrimEnd('\')
    return $innerFull.Equals($outerFull, [System.StringComparison]::OrdinalIgnoreCase) -or
        $innerFull.StartsWith($outerFull + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-UnsafeZipEntry([string] $Name) {
    if ([string]::IsNullOrWhiteSpace($Name)) { return $false }
    $normalized = $Name.Replace('\', '/')
    if ($normalized.StartsWith('/') -or $normalized.Contains(':')) { return $true }
    foreach ($part in $normalized.Split('/')) {
        if ($part -eq '..') { return $true }
    }
    return $false
}

function Copy-ReleaseTree([string] $Source, [string] $Destination) {
    if (Test-Path -LiteralPath $Destination) {
        $existingRoot = Get-Item -LiteralPath $Destination -Force
        if (-not $existingRoot.PSIsContainer) {
            throw "Install path is a file, not a directory: $Destination"
        }
    } else {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    }

    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        if ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to copy reparse point: $($_.FullName)"
        }
        $target = Join-Path $Destination $_.Name
        if ($_.PSIsContainer) {
            if ((Test-Path -LiteralPath $target) -and -not (Get-Item -LiteralPath $target -Force).PSIsContainer) {
                throw "Cannot replace a file with a directory: $target"
            }
            # return here only ends this ForEach-Object iteration. Do not switch
            # this loop to the foreach statement: return would leave the function
            # and skip every remaining file.
            Copy-ReleaseTree -Source $_.FullName -Destination $target
            return
        }
        if (Test-Path -LiteralPath $target) {
            $existing = Get-Item -LiteralPath $target -Force
            if ($existing.PSIsContainer) { throw "Cannot replace a directory with a file: $target" }
            $existing.IsReadOnly = $false
        }
        [System.IO.File]::Copy($_.FullName, $target, $true)
    }
}

$stage = $null
$exitCode = 0
try {
    Write-UpdateLog "start package=$Package install=$InstallDir"
    if ($WaitSeconds -lt 1) { throw 'WaitSeconds must be at least 1.' }
    if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) { throw "Package not found: $Package" }
    if ([string]::IsNullOrWhiteSpace($InstallDir) -or [string]::IsNullOrWhiteSpace($Restart)) {
        throw 'Install directory and restart path are required.'
    }
    if ((Test-PathInside $InstallDir $UserDataDir) -or (Test-PathInside $UserDataDir $InstallDir)) {
        throw "Refusing to update because the install directory overlaps user data: $UserDataDir"
    }

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    $stillRunning = $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
    while ($stillRunning -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $stillRunning = $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
    }
    if ($stillRunning) { throw "QQCom-plus did not exit within $WaitSeconds seconds." }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
    try {
        foreach ($entry in $zip.Entries) {
            if (Test-UnsafeZipEntry $entry.FullName) { throw "Unsafe zip entry: $($entry.FullName)" }
        }
    } finally {
        $zip.Dispose()
    }

    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("QQCom-plus-stage-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Expand-Archive -LiteralPath $Package -DestinationPath $stage -Force
    $stageFull = [System.IO.Path]::GetFullPath($stage).TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $stage -Recurse -Force | ForEach-Object {
        $full = [System.IO.Path]::GetFullPath($_.FullName)
        if (-not $full.StartsWith($stageFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Extracted file escaped staging: $full"
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'QQCom-plus.exe') -PathType Leaf)) {
        throw 'The update package does not contain QQCom-plus.exe.'
    }

    Copy-ReleaseTree -Source $stage -Destination $InstallDir
    Write-UpdateLog 'files replaced'

    if (-not (Test-Path -LiteralPath $Restart -PathType Leaf)) { throw "Restart executable not found: $Restart" }
    Start-Process -FilePath $Restart -WorkingDirectory (Split-Path -Parent $Restart) | Out-Null
    Write-UpdateLog 'restart requested'
    Remove-Item -LiteralPath $Package -Force -ErrorAction SilentlyContinue
} catch {
    Write-UpdateLog "failed: $($_.Exception.Message)"
    $exitCode = 1
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
