# QQCom OTA 自动升级方案

QQCom 采用 Windows 桌面程序常见的 Release + zip + 独立更新器方案：客户端读取一个很小的 JSON 清单，发现新版本后下载 Gitee Release 的 zip 包；更新器等待主程序退出，解压并覆盖程序目录，然后重新启动 QQCom。

本方案刻意不做旧版本回滚。更新失败时保留错误日志，用户可以重新下载当前版本的 Release 包覆盖修复。这样实现简单，适合 QQCom 当前单机工具的发布规模。

## 资源布局

Gitee 仓库只保存源码和清单模板，二进制放在 Release 附件：

```text
https://gitee.com/api/v5/repos/qinjmin/qqcom/contents/ota/stable.json?ref=master
https://gitee.com/qinjmin/qqcom/releases/download/v<version>/QQCom-<version>-win64.zip
```

稳定清单应在 zip 已上传、校验并人工试装后再更新。清单示例见 [`ota/stable.example.json`](../ota/stable.example.json)。

## 清单格式

| 字段 | 说明 |
| --- | --- |
| `schema` | 清单协议版本，当前为 `1` |
| `product` | 必须为 `QQCom` |
| `channel` | `stable` 或 `beta` |
| `version` | 目标版本，使用 `主.次.修订` |
| `mandatory` | 是否强制升级；默认 `false` |
| `download.url` | Release zip 的 HTTPS 地址 |
| `download.size` | zip 字节数 |
| `download.sha256` | zip 的 SHA-256，用于发现下载损坏 |
| `notesUrl` | Release 说明地址。安装包根目录另有版本说明文件，内容来自仓库根目录的 `更新记录.md` |

客户端处理规则：

1. 只接受 HTTPS、`product=QQCom`、已知 `schema` 和 `type=zip`。本地自测可以用 `--local-update-source http://127.0.0.1:<端口>` 打开本次进程的回环源；这个源不写入配置，也不能把下载地址改到其它主机或端口。
2. 目标版本不高于当前版本时忽略；不支持降级。
3. 下载到 `%TEMP%\QQCom-update\<version>\`，校验文件大小和 SHA-256。
4. 校验成功后启动更新器，主程序退出；校验失败删除临时文件并提示用户。
5. 启动约 3 秒后自动检查。有新版本时询问是否下载；下载过程显示进度，可以取消。状态栏只显示简短状态：「正在检查更新」「已是最新版本」或「检查更新失败」。不展示 HTTP 码、路径或清单字段。自动检查失败不弹窗。手动检查使用同样的状态文案。`QQCOM_DISABLE_UPDATE=1` 只关闭自动检查。

SHA-256 不是发布者签名，但可以发现 CDN/网络传输损坏。生产环境还应对 `QQCom.exe` 做 Windows Authenticode 签名；如果未来接入独立 CDN 或公开分发，再增加清单签名字段，不改变上述基本流程。

## 更新流程

主程序不能覆盖正在运行的自身，因此更新动作由随发布包提供的 `qqcom-updater.ps1` 完成。以后需要降低 PowerShell 依赖时，可以把它替换为同样参数的 Qt console updater，主程序协议不变：

```text
powershell.exe -File qqcom-updater.ps1 \
  -ProcessId <qqcom-pid> \
  -Package <zip-path> \
  -InstallDir <program-dir> \
  -Restart <qqcom-exe>
```

更新器执行以下步骤：

1. 等待 `-ProcessId` 指定的主程序退出，最多等待固定时间；超时则终止更新并记录原因。
2. 创建唯一 staging 目录并解压 zip。
3. 确认 staging 内存在 `QQCom.exe`，再把 staging 文件直接复制到安装目录并覆盖同名文件。
4. 保留 `%LOCALAPPDATA%\QQCom\QQCom` 下的用户配置、日志和导出文件，不从 zip 删除这些目录。
5. 把结果追加到 `%TEMP%\QQCom-update\update.log`，启动新的 QQCom，更新器退出。失败时保留这份日志和下载包，不回滚已经覆盖的文件。

覆盖时只处理发布包中的文件，不执行 zip 内脚本。安装目录建议使用用户可写的 `%LOCALAPPDATA%\Programs\QQCom`，这样普通用户可以完成升级；如果采用 `Program Files`，更新器需要管理员权限或单独安装器。

## 发布流程

发布前先清理 `docs/`：删掉已经过时的说明，把还在用的文档改成和当前程序一致。版本记录仍写仓库根目录的 `更新记录.md`，不在 `docs/` 另写一份。

最终交付按根目录 [AGENTS.md](../AGENTS.md) 执行，直接平铺在 `build/releases/`：

```text
build/releases/
  QQCom-<version>-win64.zip
  固件更新记录.md
```

构建运行目录、暂存目录和 `manifest.json` 放在其他 `build/` 子目录。用户在交付目录修订更新记录后，先同步回根目录源文件，再打包。

发布校验报告、线上清单/Release 快照及测试日志统一保存到 `build/release-verification/<version>/`，测试截图放在其中的 `screenshots/`。确认线上与最终 ZIP 一致后，可删除重复下载包、打包暂存和旧版本运行库；清理前检查正在运行的 QQCom 程序路径。

1. 版本号直接改 `version/version.h` 里的主/次/修订号，构建只刷新时间戳。本版更新记录标题用 `# QQCom-<version>-win64`，最新记录置顶，保留原有历史标题和 `1.内容`、条目间空行的格式。
2. 使用固定 Qt Kit 构建 Release，并运行 core/UI 测试。检查工作区并 `git fetch origin master` 核对分支差异，提交源码、规则、文档和更新记录，创建并推送 `v<version>` 标签及 `master`。不提交构建产物，不强制推送，不移动已有发布标签。
3. 准备仅含运行文件的目录，包含程序、Qt/MSVC 运行库、插件、更新器及 CLI 入口和说明，排除 obj/moc 等构建产物。使用 `scripts/package_release.ps1` 先在 `build/ota` 生成 ZIP、`manifest.json` 和版本说明；包内外说明使用根目录 `更新记录.md` 的同一定稿。校验后将 ZIP 和 `固件更新记录.md` 复制到 `build/releases/`，不套版本号目录。
4. 创建 Gitee Release 并上传 ZIP；如果用户已上传，直接复用已有 Release，不重复上传或要求令牌。核对版本、标签、名称和说明，从附件链接重新下载 ZIP，检查完整性，并与用户定稿的本地 ZIP 核对大小和 SHA-256。清单数据以验证后的线上附件为准。
5. 在干净机器安装旧版本，验证清单检查、下载、文件覆盖、配置保留和重新启动。
6. 将实际附件的版本、Release 日期、HTTPS 下载地址、字节数、SHA-256 和说明页地址写入 `ota/stable.json`，提交并推送到 `origin/master`；灰度时先使用 `beta` 通道，验证后再切到 `stable`。源码及稳定清单可以分次提交，已发布标签仍指向原构建源码。
7. 推送后核对远端分支提交和标签，读取客户端使用的 Gitee Contents API，确认 `ota/stable.json` 的版本、链接、大小和哈希已更新。生成包和本地提交本身不代表线上发布完成。

远端固定为 `origin`（`https://gitee.com/qinjmin/qqcom.git`），发布分支为 `master`。源码推送、Release 附件和稳定清单分别验收：前者供代码同步，附件供下载，稳定清单决定客户端是否提示升级。

本地打包和校验：

```powershell
# 先把完整运行文件整理到 build/release-runtime/<version>，不含编译中间产物。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1 `
  -BinaryDir build/release-runtime/<version> `
  -Version <version> `
  -BaseUrl https://gitee.com/qinjmin/qqcom/releases/download/v<version>

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_ota_manifest.ps1 `
  -ManifestPath build/ota/manifest.json `
  -PackagePath build/ota/QQCom-<version>-win64.zip

New-Item -ItemType Directory -Force -Path build/releases | Out-Null
Copy-Item -LiteralPath build/ota/QQCom-<version>-win64.zip, build/ota/固件更新记录.md -Destination build/releases

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_ota.ps1

# 用当前 Release 跑真实检查、下载、覆盖和重启。不访问 Gitee，也不改用户配置。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/local_update_test.ps1 `
  -BinaryDir build/release-runtime/<version>
```

## 还没做的部分

检查、下载、校验、覆盖和重启已经接入。下载使用系统代理；连续 10 秒没有新字节会从头重试，过程写在本机 update.log。还没做的是代码签名、清单签名和错误日志上传。更新仍由用户确认，不做后台静默强制升级。错误日志不上传用户通信数据。
