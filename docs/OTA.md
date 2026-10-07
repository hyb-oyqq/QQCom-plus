# QQCom OTA 自动升级方案

QQCom 采用 Windows 桌面程序常见的 Release + zip + 独立更新器方案：客户端读取一个很小的 JSON 清单，发现新版本后下载 Gitee Release 的 zip 包；更新器等待主程序退出，解压并覆盖程序目录，然后重新启动 QQCom。

本方案刻意不做旧版本回滚。更新失败时保留错误日志，用户可以重新下载当前版本的 Release 包覆盖修复。这样实现简单，适合 QQCom 当前单机工具的发布规模。

## 资源布局

Gitee 仓库只保存源码和清单模板，二进制放在 Release 附件：

```text
https://gitee.com/qinjmin/qqcom/raw/master/ota/stable.json
https://gitee.com/qinjmin/qqcom/releases/download/v0.2.0/QQCom-0.2.0-win64.zip
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
| `notesUrl` | Release 说明地址 |

客户端处理规则：

1. 只接受 HTTPS、`product=QQCom`、已知 `schema` 和 `type=zip`。
2. 目标版本不高于当前版本时忽略；不支持降级。
3. 下载到 `%TEMP%\QQCom-update\<version>\`，校验文件大小和 SHA-256。
4. 校验成功后启动更新器，主程序退出；校验失败删除临时文件并提示用户。

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

1. 等待 `--pid` 退出，最多等待固定时间；超时则终止更新并记录原因。
2. 创建唯一 staging 目录并解压 zip。
3. 确认 staging 内存在 `QQCom.exe`，再把 staging 文件直接复制到安装目录并覆盖同名文件。
4. 保留 `%LOCALAPPDATA%\QQCom\QQCom` 下的用户配置、日志和导出文件，不从 zip 删除这些目录。
5. 写入 `update.log`，启动新的 QQCom，更新器退出。

覆盖时只处理发布包中的文件，不执行 zip 内脚本。安装目录建议使用用户可写的 `%LOCALAPPDATA%\Programs\QQCom`，这样普通用户可以完成升级；如果采用 `Program Files`，更新器需要管理员权限或单独安装器。

## 发布流程

1. 修改 `version/version.json`，更新 `CHANGELOG.md`，提交并打 tag `v<version>`。
2. 使用固定 Qt Kit 构建 Release，并运行 core/UI 测试。
3. 使用 `scripts/package_release.ps1` 生成 `QQCom-<version>-win64.zip` 和 `manifest.json`。
4. 创建 Gitee Release，上传 zip；把脚本输出的 SHA-256 填入清单。
5. 在干净机器安装旧版本，验证清单检查、下载、文件覆盖、配置保留和重新启动。
6. 把校验后的 manifest 发布到 `ota/stable.json`；灰度时先把 `channel` 设为 `beta`，验证后再切到 `stable`。

本地打包和校验：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1 `
  -BinaryDir build/release/release `
  -Version 0.2.0 `
  -BaseUrl https://gitee.com/qinjmin/qqcom/releases/download/v0.2.0

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_ota_manifest.ps1 `
  -ManifestPath build/ota/manifest.json `
  -PackagePath build/ota/QQCom-0.2.0-win64.zip
```

## 分阶段落地

1. **发布基础**：版本清单、打包脚本、校验脚本和 Release 约定。
2. **客户端检查**：加入 Qt `QNetworkAccessManager`，启动后延迟检查清单，设置页提供“检查更新”。
3. **下载和更新器**：下载 zip、校验 SHA-256、启动 updater、退出主程序、覆盖并重启。
4. **运营增强**：增加 beta 通道、强制升级、下载进度、代理/超时设置和错误日志上传（仍不上传用户通信数据）。

第一版建议只实现前 3 个阶段，不做后台静默强制升级。用户看到版本号、大小、Release 说明后确认更新，失败时重试即可。
