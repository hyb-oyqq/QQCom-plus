param(
    [Parameter(Mandatory = $true)]
    [int] $ProcessId,
    [Parameter(Mandatory = $true)]
    [string] $Package,
    [Parameter(Mandatory = $true)]
    [string] $InstallDir,
    [Parameter(Mandatory = $true)]
    [string] $Restart
)

$ErrorActionPreference = 'Stop'
$logPath = Join-Path ([System.IO.Path]::GetTempPath()) 'QQCom-update.log'

function Write-UpdateLog([string] $Message) {
    $line = "$(Get-Date -Format o) $Message"
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
}

try {
    Write-UpdateLog "start package=$Package install=$InstallDir"
    if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) { throw "Package not found: $Package" }

    try {
        Get-Process -Id $ProcessId -ErrorAction Stop | Out-Null
        Wait-Process -Id $ProcessId -Timeout 60 -ErrorAction SilentlyContinue
        if (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
            throw 'QQCom did not exit within 60 seconds.'
        }
    } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # The main process may have exited before the updater started.
    }

    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("QQCom-stage-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Expand-Archive -LiteralPath $Package -DestinationPath $stage -Force
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'QQCom.exe') -PathType Leaf)) {
        throw 'The update package does not contain QQCom.exe.'
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Get-ChildItem -LiteralPath $stage -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $InstallDir $_.Name) -Recurse -Force
    }
    Write-UpdateLog 'files replaced'

    if (-not (Test-Path -LiteralPath $Restart -PathType Leaf)) { throw "Restart executable not found: $Restart" }
    Start-Process -FilePath $Restart -WorkingDirectory (Split-Path -Parent $Restart)
    Write-UpdateLog 'restart requested'
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $Package -Force -ErrorAction SilentlyContinue
} catch {
    Write-UpdateLog "failed: $($_.Exception.Message)"
    exit 1
}
