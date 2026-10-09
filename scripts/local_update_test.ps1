# Run the real QQCom OTA flow against a loopback fixture.
# Does not contact Gitee and does not touch the real user profile.
param(
    [Parameter(Mandatory = $true)]
    [string] $BinaryDir,
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $TargetVersion = '0.1.1',
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $CurrentVersion = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $PSScriptRoot 'package_release.ps1'
$binaryRoot = (Resolve-Path -LiteralPath $BinaryDir).Path
$sourceExe = Join-Path $binaryRoot 'QQCom.exe'
if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "QQCom.exe was not found: $sourceExe"
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('qqcom-local-update-' + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $work 'install'
$userData = Join-Path $work 'user-data'
$packageDir = Join-Path $work 'package'
$logPath = Join-Path $work 'update.log'
$serverScript = Join-Path $env:TEMP 'qqcom-local-update-server.py'
New-Item -ItemType Directory -Force -Path $install, $userData, $packageDir | Out-Null

function Stop-Tree([System.Diagnostics.Process] $Process) {
    if ($null -eq $Process) { return }
    if ($Process.HasExited) { return }
    & taskkill.exe /PID $Process.Id /T /F | Out-Null
}

$server = $null
$app = $null
try {
    Set-Content -LiteralPath (Join-Path $userData 'preferences.ini') -Value 'ota-test=kept' -Encoding ascii -NoNewline
    Copy-Item -Path (Join-Path $binaryRoot '*') -Destination $install -Recurse -Force
    $oldMarker = Join-Path $install 'ota-old.txt'
    Set-Content -LiteralPath $oldMarker -Value 'OLD' -Encoding ascii -NoNewline
    $oldHash = (Get-FileHash -LiteralPath (Join-Path $install 'QQCom.exe') -Algorithm SHA256).Hash

    & $packageScript -BinaryDir $binaryRoot -Version $TargetVersion -BaseUrl "https://gitee.com/qinjmin/qqcom/releases/download/v$TargetVersion" -OutputDir $packageDir
    $package = Join-Path $packageDir "QQCom-$TargetVersion-win64.zip"
    if (-not (Test-Path -LiteralPath $package)) { throw "Package was not created: $package" }

    $server = Start-Process -FilePath 'python' -ArgumentList @($serverScript, $package, $TargetVersion) -WorkingDirectory $work -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $work 'server.out') -RedirectStandardError (Join-Path $work 'server.err')
    $origin = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath (Join-Path $work 'server.out')) {
            $line = Select-String -LiteralPath (Join-Path $work 'server.out') -Pattern '^ORIGIN ' | Select-Object -First 1
            if ($line) {
                $origin = $line.Line.Substring(7).Trim()
                break
            }
        }
        if ($server.HasExited) { throw "Local update server exited early: $(Get-Content -LiteralPath (Join-Path $work 'server.err') -Raw)" }
        Start-Sleep -Milliseconds 100
    }
    if ([string]::IsNullOrWhiteSpace($origin)) { throw 'Local update server did not publish an origin.' }
    Write-Host "LOCAL_SOURCE $origin"

    $env:QQCOM_UPDATE_LOG = $logPath
    $env:QQCOM_UPDATE_USER_DATA = $userData
    $env:QQCOM_DISABLE_UPDATE = $null
    $app = Start-Process -FilePath (Join-Path $install 'QQCom.exe') -ArgumentList @(
        '--local-update-source', $origin,
        '--local-update-current-version', $CurrentVersion,
        '--accept-update') -WorkingDirectory $install -PassThru
    Write-Host "APP_PID $($app.Id)"

    $deadline = (Get-Date).AddSeconds(180)
    $replaced = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $logPath) {
            $text = [System.IO.File]::ReadAllText($logPath)
            if ($text -match 'restart requested') { $replaced = $true; break }
            if ($text -match 'failed:') { throw "Updater failed: $text" }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $replaced) {
        $serverErr = ''
        if (Test-Path -LiteralPath (Join-Path $work 'server.err')) { $serverErr = Get-Content -LiteralPath (Join-Path $work 'server.err') -Raw }
        $log = ''
        if (Test-Path -LiteralPath $logPath) { $log = Get-Content -LiteralPath $logPath -Raw }
        throw "Update did not finish. log=$log server=$serverErr"
    }

    $newHash = (Get-FileHash -LiteralPath (Join-Path $install 'QQCom.exe') -Algorithm SHA256).Hash
    $expected = (Get-FileHash -LiteralPath $sourceExe -Algorithm SHA256).Hash
    if ($newHash -ne $expected) { throw "Installed QQCom.exe hash changed unexpectedly: $newHash" }
    if ($newHash -ne $oldHash) { throw "Same-build replacement should keep the executable bytes." }
    if ((Get-Content -LiteralPath $oldMarker -Raw) -ne 'OLD') { throw 'A file outside the package was removed.' }
    if ((Get-Content -LiteralPath (Join-Path $userData 'preferences.ini') -Raw) -ne 'ota-test=kept') {
        throw 'User preferences were modified.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $install 'qqcom-updater.ps1'))) {
        throw 'Updater script was not installed.'
    }

    $restarted = $false
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $running = Get-CimInstance Win32_Process -Filter "Name = 'QQCom.exe'" | Where-Object {
            $_.ExecutablePath -and ($_.ExecutablePath -ieq (Join-Path $install 'QQCom.exe')) -and $_.ProcessId -ne $app.Id
        }
        if ($running) { $restarted = $true; break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $restarted) { throw 'Updated QQCom.exe did not restart.' }

    $report = [ordered]@{
        success = $true
        install = $install
        source = $origin
        currentVersion = $CurrentVersion
        targetVersion = $TargetVersion
        executableHash = $newHash
        preferencesPreserved = $true
        restarted = $true
    }
    $report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'verification.json') -Encoding UTF8
    Write-Host 'OTA_SUCCESS'
    Write-Host ($report | ConvertTo-Json -Compress)
} finally {
    Get-CimInstance Win32_Process -Filter "Name = 'QQCom.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($install, [System.StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Stop-Tree $app
    Stop-Tree $server
    Remove-Item Env:QQCOM_UPDATE_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:QQCOM_UPDATE_USER_DATA -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath (Join-Path $work 'verification.json')) {
        Write-Host "VERIFICATION $(Join-Path $work 'verification.json')"
    } else {
        Write-Host "FAILURE_DIR $work"
    }
}
