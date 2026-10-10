# QQCom 0.1.0 发布记录

记录 2026-10-10 第一次把 OTA 发到 Gitee 的实际顺序。协议和字段见 [OTA 方案](OTA.md)。以后发新版本时按文末清单执行，把本文当作已核对过的样例。

## 已发布内容

| 项 | 值 |
| --- | --- |
| 版本 | `0.1.0` |
| 发行版 | https://gitee.com/qinjmin/qqcom/releases/tag/v0.1.0 |
| 标签指向 | `4e67301c7070e7f39bc547cdf996bd0017864c8a` |
| 安装包 | https://gitee.com/qinjmin/qqcom/releases/download/v0.1.0/QQCom-0.1.0-win64.zip |
| 大小 | 31,680,688 字节 |
| SHA-256 | `d78cbbd6ac3f873d964f6722400fcbbbfc92b1e7c1c832caadfc96205c5a8d90` |
| 稳定清单 | https://gitee.com/api/v5/repos/qinjmin/qqcom/contents/ota/stable.json?ref=master |
| 清单提交 | `2738a8a` |
| 预览版 | 否 |
| 构建 | `QQCom v0.1.0 Build:202610092318` |

`v0.1.0` 标签指向 OTA 客户端提交，不包含后来的 `2738a8a` 清单提交和 `9f07243` A/B 桥接提交。已安装 `0.1.0` 的程序读到同一版本时显示已是最新版本，不会下载这次之后的源码改动。

## 实际顺序

1. 用 Qt 6.10.1 MSVC 2022 x64 构建 `build/release/release/QQCom.exe`。版本源仍是 `version/version.json` 的 `0.1.0`，没有为这次发布递增版本。
2. 用 `scripts/package_release.ps1` 生成 `build/ota/QQCom-0.1.0-win64.zip` 和 `build/ota/manifest.json`。下载地址写成上面的 Gitee Release 地址。
3. 用 `scripts/validate_ota_manifest.ps1` 核对清单里的大小和 SHA-256。
4. 在 Gitee 网页创建发行版，不勾选「这是一个预览版本」。版本号 `v0.1.0`，目标分支 `master`，标题 `v0.1.0`，附件是上面的 zip。网页创建说明见 https://help.gitee.com/repository/release/create/。
5. 公开下载安装包，确认 HTTP 200、大小和 SHA-256 与本地包一致。Gitee 会先 302 到附件地址，再 302 到 `foruda.gitee.com`。
6. 把校验过的 `manifest.json` 原样提交为 `ota/stable.json` 并推到 `master`。公开 contents 接口返回的是 base64 包裹，客户端会解开后再读清单。
7. 复制一份 Release 运行目录，用 `--local-update-current-version 0.0.1 --accept-update` 对真实 Gitee 清单做升级。临时用户目录由 `QQCOM_UPDATE_USER_DATA` 指定，不改本机 `%LOCALAPPDATA%\QQCom`。

第 7 步成功时，更新日志出现 `files replaced` 和 `restart requested`。覆盖后的 `QQCom.exe` 哈希与发布目录一致，包外标记文件和临时偏好都还在，程序重新启动。

## 下次发布

1. 先提交要发布的代码。执行 `scripts/update_version.ps1 -Mode patch`，更新 `CHANGELOG.md`，再提交并推送。
2. 运行 `build.bat`，确认 `QQCom.exe` 标题里的版本是新版本。
3. 打包并校验。把下面命令里的版本号换成新版本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1 `
  -BinaryDir build/release/release `
  -Version 0.1.1 `
  -BaseUrl https://gitee.com/qinjmin/qqcom/releases/download/v0.1.1

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_ota_manifest.ps1 `
  -ManifestPath build/ota/manifest.json `
  -PackagePath build/ota/QQCom-0.1.1-win64.zip
```

4. 在 https://gitee.com/qinjmin/qqcom/releases/new 创建发行版。版本号用 `v` 加版本，分支选 `master`，不要勾选预览版。附件选择 `build/ota/QQCom-<version>-win64.zip`。
5. 公开下载该附件，确认大小和 SHA-256 与 `build/ota/manifest.json` 一致。不一致就不要改 `ota/stable.json`。
6. 用清单里的版本覆盖 `ota/stable.json`，只提交这一个文件并推到 `master`。zip 不进 Git。
7. 复制当前 Release 目录，把比较版本设成旧版本，对 Gitee 再跑一次。成功标准仍是覆盖、配置保留和重启：

```powershell
$work = Join-Path $env:TEMP 'qqcom-release-check'
$install = Join-Path $work 'install'
$userData = Join-Path $work 'user-data'
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $install, $userData | Out-Null
Get-ChildItem build\release\release | Where-Object {
    $_.Extension -notin '.obj', '.cpp', '.h', '.pdb', '.ilk'
} | ForEach-Object { Copy-Item $_.FullName (Join-Path $install $_.Name) -Recurse -Force }
Copy-Item scripts\qqcom-updater.ps1 (Join-Path $install 'qqcom-updater.ps1') -Force
$env:QQCOM_UPDATE_LOG = Join-Path $work 'update.log'
$env:QQCOM_UPDATE_USER_DATA = $userData
& (Join-Path $install 'QQCom.exe') --local-update-current-version 0.1.0 --accept-update
```

比较版本必须低于新清单。窗口标题仍显示编译进 exe 的版本，这不影响比较。下载大约 30 MB，Gitee 附件速度可能较慢；进度框取消会中止下载。

## 不要重复的做法

- 不要把 zip 提交进仓库。客户端只从 Release 附件地址下载。
- 不要在附件公开下载并核对哈希之前更新 `ota/stable.json`。
- 不要用仓库里的 Git 密码调用 Gitee API。网页创建被自动化浏览器拦截时，在本机浏览器里手工创建发行版。
- 不要把 `QQCOM_UPDATE_USER_DATA` 指到真实用户目录。
- 窗口标题里的 `v0.1.0` 不能证明比较版本已经改低。比较版本只来自 `--local-update-current-version`。
