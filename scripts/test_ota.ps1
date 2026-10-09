# 本地验证 OTA 打包、清单校验和更新器。不访问网络，也不修改真实用户配置。
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $PSScriptRoot 'package_release.ps1'
$validateScript = Join-Path $PSScriptRoot 'validate_ota_manifest.ps1'
$updaterScript = Join-Path $PSScriptRoot 'qqcom-updater.ps1'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('qqcom-ota-test-' + [Guid]::NewGuid().ToString('N'))
$failures = New-Object System.Collections.Generic.List[string]

function Assert-True([bool] $Condition, [string] $Message) {
    if ($Condition) {
        Write-Host "OK   $Message"
        return
    }
    Write-Host "FAIL $Message"
    $script:failures.Add($Message)
}

function Invoke-Updater([string[]] $UpdaterArgs) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updaterScript @UpdaterArgs
    return $LASTEXITCODE
}

function Read-Log([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return [System.IO.File]::ReadAllText($Path)
}

function New-ExitedProcessId {
    $process = Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile', '-Command', 'exit 0') -WindowStyle Hidden -PassThru
    $process.WaitForExit()
    return $process.Id
}

New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $emptyDir = Join-Path $work 'empty-bin'
    New-Item -ItemType Directory -Force -Path $emptyDir | Out-Null
    $missingExeFailed = $false
    try {
        & $packageScript -BinaryDir $emptyDir -Version 0.2.0 -BaseUrl 'https://example.com/releases/download/v0.2.0' -OutputDir (Join-Path $work 'unused')
    } catch {
        $missingExeFailed = $true
    }
    Assert-True $missingExeFailed '打包目录没有 QQCom.exe 时失败'

    $binaryDir = Join-Path $work 'binary'
    New-Item -ItemType Directory -Force -Path (Join-Path $binaryDir 'plugins\platforms') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $binaryDir 'plugins\imageformats') | Out-Null
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\whoami.exe') -Destination (Join-Path $binaryDir 'QQCom.exe') -Force
    Set-Content -LiteralPath (Join-Path $binaryDir 'plugins\platforms\old.txt') -Value 'NEW' -Encoding ascii -NoNewline
    Set-Content -LiteralPath (Join-Path $binaryDir 'plugins\imageformats\extra.txt') -Value 'EXTRA' -Encoding ascii -NoNewline

    $outputDir = Join-Path $work 'ota'
    & $packageScript -BinaryDir $binaryDir -Version 0.2.0 -BaseUrl 'https://gitee.com/qinjmin/qqcom/releases/download/v0.2.0' -OutputDir $outputDir
    $package = Join-Path $outputDir 'QQCom-0.2.0-win64.zip'
    $manifest = Join-Path $outputDir 'manifest.json'
    Assert-True (Test-Path -LiteralPath $package) '生成 Release zip'
    Assert-True (Test-Path -LiteralPath $manifest) '生成 manifest.json'

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    } finally {
        $zip.Dispose()
    }
    Assert-True ($entries -contains 'QQCom.exe') '压缩包根目录有 QQCom.exe'
    Assert-True ($entries -contains 'qqcom-updater.ps1') '压缩包带有更新器'
    Assert-True (($entries -contains 'plugins/platforms/old.txt') -or ($entries -contains 'plugins\platforms\old.txt')) '压缩包保留 plugins 目录'

    $validOk = $true
    try { & $validateScript -ManifestPath $manifest -PackagePath $package } catch { $validOk = $false }
    Assert-True $validOk '校验脚本接受匹配的大小和 SHA-256'

    $badSize = Join-Path $work 'bad-size.json'
    $sizeManifest = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $sizeManifest.download.size = 1
    $sizeManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badSize -Encoding UTF8
    $badSizeFailed = $false
    try { & $validateScript -ManifestPath $badSize -PackagePath $package } catch { $badSizeFailed = $true }
    Assert-True $badSizeFailed '清单大小不匹配时校验失败'

    $badHash = Join-Path $work 'bad-hash.json'
    $hashManifest = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $hashManifest.download.sha256 = '11' * 32
    $hashManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badHash -Encoding UTF8
    $badHashFailed = $false
    try { & $validateScript -ManifestPath $badHash -PackagePath $package } catch { $badHashFailed = $true }
    Assert-True $badHashFailed '清单 SHA-256 不匹配时校验失败'

    $install = Join-Path $work 'install'
    $userData = Join-Path $work 'user-data'
    New-Item -ItemType Directory -Force -Path (Join-Path $install 'plugins\platforms') | Out-Null
    New-Item -ItemType Directory -Force -Path $userData | Out-Null
    Set-Content -LiteralPath (Join-Path $install 'QQCom.exe') -Value 'OLD-EXE' -Encoding ascii -NoNewline
    Set-Content -LiteralPath (Join-Path $install 'plugins\platforms\old.txt') -Value 'OLD' -Encoding ascii -NoNewline
    Set-Content -LiteralPath (Join-Path $install 'plugins\platforms\keep.txt') -Value 'KEEP' -Encoding ascii -NoNewline
    Set-Content -LiteralPath (Join-Path $install 'notes.txt') -Value 'LOCAL' -Encoding ascii -NoNewline
    Set-Content -LiteralPath (Join-Path $userData 'preferences.ini') -Value 'preferences=1' -Encoding ascii -NoNewline
    Set-ItemProperty -LiteralPath (Join-Path $install 'plugins\platforms\old.txt') -Name IsReadOnly -Value $true

    $overlapLog = Join-Path $work 'overlap.log'
    $overlapCode = Invoke-Updater @(
        '-ProcessId', "$PID",
        '-Package', $package,
        '-InstallDir', $userData,
        '-Restart', (Join-Path $userData 'QQCom.exe'),
        '-LogPath', $overlapLog,
        '-WaitSeconds', '2',
        '-UserDataDir', $userData)
    Assert-True ($overlapCode -eq 1) '安装目录与用户数据重叠时拒绝更新'
    Assert-True ((Read-Log $overlapLog) -match 'overlaps user data') '重叠失败写入 update.log'
    Assert-True ((Get-Content -LiteralPath (Join-Path $userData 'preferences.ini') -Raw) -eq 'preferences=1') '拒绝更新时不改用户配置'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $userData 'QQCom.exe'))) '拒绝更新时不写入用户数据目录'

    $slipZip = Join-Path $work 'slip.zip'
    $archive = [System.IO.Compression.ZipFile]::Open($slipZip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $exeEntry = $archive.CreateEntry('QQCom.exe')
        $exeWriter = New-Object System.IO.StreamWriter($exeEntry.Open())
        $exeWriter.Write('not-run')
        $exeWriter.Dispose()
        $badEntry = $archive.CreateEntry('../escaped.txt')
        $badWriter = New-Object System.IO.StreamWriter($badEntry.Open())
        $badWriter.Write('escaped')
        $badWriter.Dispose()
    } finally {
        $archive.Dispose()
    }
    $slipLog = Join-Path $work 'slip.log'
    $slipCode = Invoke-Updater @(
        '-ProcessId', "$(New-ExitedProcessId)",
        '-Package', $slipZip,
        '-InstallDir', $install,
        '-Restart', (Join-Path $install 'QQCom.exe'),
        '-LogPath', $slipLog,
        '-WaitSeconds', '2',
        '-UserDataDir', $userData)
    Assert-True ($slipCode -eq 1) '压缩包路径穿越时更新失败'
    Assert-True ((Read-Log $slipLog) -match 'Unsafe zip entry') '路径穿越原因写入日志'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'QQCom.exe') -Raw) -eq 'OLD-EXE') '路径穿越时不覆盖安装目录'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $work 'escaped.txt'))) '路径穿越文件没有落盘'

    $missingZip = Join-Path $work 'missing-exe.zip'
    $missingArchive = [System.IO.Compression.ZipFile]::Open($missingZip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $readme = $missingArchive.CreateEntry('readme.txt')
        $readmeWriter = New-Object System.IO.StreamWriter($readme.Open())
        $readmeWriter.Write('readme')
        $readmeWriter.Dispose()
    } finally {
        $missingArchive.Dispose()
    }
    $missingLog = Join-Path $work 'missing.log'
    $missingCode = Invoke-Updater @(
        '-ProcessId', "$(New-ExitedProcessId)",
        '-Package', $missingZip,
        '-InstallDir', $install,
        '-Restart', (Join-Path $install 'QQCom.exe'),
        '-LogPath', $missingLog,
        '-WaitSeconds', '2',
        '-UserDataDir', $userData)
    Assert-True ($missingCode -eq 1) '压缩包没有 QQCom.exe 时更新失败'
    Assert-True ((Read-Log $missingLog) -match 'does not contain QQCom.exe') '缺少主程序的原因写入日志'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'QQCom.exe') -Raw) -eq 'OLD-EXE') '失败时保留原主程序'

    $sleeper = Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') -WindowStyle Hidden -PassThru
    $timeoutLog = Join-Path $work 'timeout.log'
    try {
        $timeoutCode = Invoke-Updater @(
            '-ProcessId', "$($sleeper.Id)",
            '-Package', $package,
            '-InstallDir', $install,
            '-Restart', (Join-Path $install 'QQCom.exe'),
            '-LogPath', $timeoutLog,
            '-WaitSeconds', '2',
            '-UserDataDir', $userData)
    } finally {
        Stop-Process -Id $sleeper.Id -Force -ErrorAction SilentlyContinue
    }
    Assert-True ($timeoutCode -eq 1) '主程序超时未退出时终止更新'
    Assert-True ((Read-Log $timeoutLog) -match 'did not exit within 2 seconds') '超时原因写入日志'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'plugins\platforms\old.txt') -Raw) -eq 'OLD') '超时后不覆盖已有文件'

    $finished = Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile', '-Command', 'exit 0') -WindowStyle Hidden -PassThru
    $finished.WaitForExit()
    $successLog = Join-Path $work 'success.log'
    $successCode = Invoke-Updater @(
        '-ProcessId', "$($finished.Id)",
        '-Package', $package,
        '-InstallDir', $install,
        '-Restart', (Join-Path $install 'QQCom.exe'),
        '-LogPath', $successLog,
        '-WaitSeconds', '5',
        '-UserDataDir', $userData)
    Assert-True ($successCode -eq 0) '主程序退出后更新成功'
    $successText = Read-Log $successLog
    Assert-True ($successText -match 'files replaced') '成功日志记录文件覆盖'
    Assert-True ($successText -match 'restart requested') '成功日志记录重新启动'
    Assert-True (-not (Test-Path -LiteralPath $package)) '安装成功后删除临时压缩包'
    $expectedExe = Get-FileHash -LiteralPath (Join-Path $binaryDir 'QQCom.exe') -Algorithm SHA256
    $updatedExe = Get-FileHash -LiteralPath (Join-Path $install 'QQCom.exe') -Algorithm SHA256
    Assert-True ($expectedExe.Hash -eq $updatedExe.Hash) '安装目录中的 QQCom.exe 已替换'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'plugins\platforms\old.txt') -Raw) -eq 'NEW') '同名插件文件被覆盖而不是嵌套'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $install 'plugins\platforms\platforms'))) '已有 plugins 目录没有被套一层'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'plugins\platforms\keep.txt') -Raw) -eq 'KEEP') '发布包没有的文件被保留'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'plugins\imageformats\extra.txt') -Raw) -eq 'EXTRA') '新的子目录被复制进来'
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'notes.txt') -Raw) -eq 'LOCAL') '安装目录里的本地文件被保留'
    Assert-True ((Get-Content -LiteralPath (Join-Path $userData 'preferences.ini') -Raw) -eq 'preferences=1') '用户配置目录保持不变'
    Assert-True (Test-Path -LiteralPath (Join-Path $install 'qqcom-updater.ps1')) '新的更新器脚本已写入安装目录'
} finally {
    if ($failures.Count -eq 0) {
        # 更新器会立刻启动新的 QQCom.exe，文件句柄可能晚几百毫秒才放开。
        for ($attempt = 0; $attempt -lt 15; $attempt++) {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
            if (-not (Test-Path -LiteralPath $work)) { break }
            Start-Sleep -Milliseconds 200
        }
    } else {
        Write-Host "失败现场保留在 $work"
    }
}

if ($failures.Count -gt 0) {
    Write-Host ("OTA 测试失败 {0} 项" -f $failures.Count)
    exit 1
}
Write-Host 'OTA 测试通过'
exit 0
