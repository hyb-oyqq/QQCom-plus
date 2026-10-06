# easyDebug 在线升级（OTA）方案

> 版本：草案 v1 · 2026-10-06
> 目标：应用内「发现新版本 → 提示 → 点击下载 → 校验 → 自动替换并重启」

## 1. 结论先说

**不需要强依赖 GitHub。** 更新功能对运行时的唯一要求是「能 HTTPS 拿到一个 JSON 清单 + 一个包文件」。
GitHub 只是**成本最低的存放地**（免费 + 自带 CDN + 自带 Release API），我们用它托管 Release 资源，
但代码里只认一段「URL 候选列表」，将来换自建 COS/OSS 只改配置、不改代码。

推荐组合：**GitHub Releases 做源 + 国内镜像做加速候选**，两条 URL 顺序回退。

---

## 2. 现状盘点（已核对）

| 项 | 现状 | 对方案的影响 |
|---|---|---|
| 构建 | Qt 6.10.1 / qmake / MSVC2022 | `QNetworkAccessManager` 直接可用 |
| QtNetwork | 已链接（`src/transport/transport.pri` 自带 `QT += network`），`Qt6Network.dll` 已在发布目录 | **无需新增 Qt 模块，无需补拷 DLL** |
| TLS | Qt6 在 Windows 默认走 **Schannel**（已确认） | HTTPS 开箱可用，不需要 OpenSSL DLL；且 6.10.1 > 6.9.2，已规避 CVE-2025-6338 |
| 版本源 | `version/version.json`（major/minor/patch），由 `scripts/update_version.ps1` 生成 `version.h` | 复用即可，作为唯一真源 |
| 偏好存储 | `%LOCALAPPDATA%/easyDebug/easyDebug/preferences.ini` | 新增 `update/*` 键 |
| git remote | **当前没有配置任何远端** | 需先建 GitHub 仓库（本地 `gh` 未登录，可走 GitHub 连接器或直接网页创建） |
| 产物体积 | Qt DLL 73.3 MB + 插件 ≈ 76 MB；`easyDebug.exe` 675 KB | 见 §4 分包策略：绝大部分更新只需下载 ~675 KB |
| 安装形态 | 目前无安装包，解压即用 | 见 §6：建议引入 NSIS，但非必须 |

---

## 3. 更新源选型

### 3.1 三个候选

| 方案 | 成本 | 优点 | 缺点 | 建议 |
|---|---|---|---|---|
| **A. GitHub Releases** | 0 | 免费、有 CDN、有 API、有审计与回滚（可编辑 Release）、 Tag 驱动 | 国内访问不稳定/慢；匿名 API 60 次/小时限流 | **起步首选** |
| **B. GitHub + 国内镜像** | 0 | 保留 A 全部优点，解决访问问题 | 依赖第三方镜像可用性（做成「候选之一」即可降级） | **推荐默认配置** |
| **C. 自建 OSS/COS/Nginx** | 几元/月 | 国内速度最快、完全可控、可配 CDN 域名 | 要维护、要钱、要备案（域名） | 产品化后再迁 |

### 3.2 为什么「不绑定」

代码只依赖：

```cpp
// 清单地址候选，按序尝试，成功即止
QStringList manifestUrls();   // 来自内置默认值 + preferences.ini 覆盖
QStringList baseUrls();       // 包文件的 base URL 候选
```

默认内置例如：

```
https://raw.githubusercontent.com/<owner>/easyDebug/main/update/latest.json
https://ghfast.top/raw.githubusercontent.com/<owner>/easyDebug/main/update/latest.json
```

> 建议：**清单用 Release 附件发布**，而不是只放仓库 raw 文件。Release 附件走 `objects.githubusercontent.com`，CDN 命中更好，且能随 Release 回滚（`latest` 指最新 Release，`latest.json` 内容即签发时的产物）。

### 3.3 关于 GitHub API

起步阶段**完全不需要调** GitHub REST API：直接读静态 JSON 即可绕开匿名 60 次/小时限流。
后续若要「自动取最新 Release 名」，可用 `https://api.github.com/repos/<owner>/<repo>/releases/latest`，但只作为 CI/发布脚本用，不放客户端。

---

## 4. 清单格式 `latest.json`

```json
{
  "schema": 1,
  "product": "easyDebug",
  "channel": "stable",
  "latest": {
    "version": "0.2.0",
    "publishedAt": "2026-10-20T08:00:00Z",
    "required": false,
    "notes": [
      "修复…（中文更新日志，数组逐条渲染为列表）"
    ],
    "runtime": "2026.10.1-qt6.10.1",
    "artifacts": [
      {
        "kind": "app",
        "os": "windows",
        "arch": "x86_64",
        "file": "easyDebug-0.2.0-app.bin",
        "size": 675840,
        "sha256": "…",
        "target": "easyDebug.exe",
        "targetVersion": "0.1.0"
      },
      {
        "kind": "full",
        "os": "windows",
        "arch": "x86_64",
        "file": "easyDebug-0.2.0-win64-setup.exe",
        "size": 31457280,
        "sha256": "…"
      }
    ]
  },
  "minimumVersion": "0.0.1"
}
```

关键字段说明：

- `runtime`：当前安装包所带的 Qt/第三方依赖版本。**这是分包策略的开关**（见 §4.1）。
- `artifacts[].targetVersion`：该 `app` 增量包能覆盖的**已安装版本上限集合**——实际上 exe 增量不做二进制差分，
  所以语义是「任何 ≤ latest 的已安装版本且 runtime 相同」都能直接替换 exe。
- `required`：为 true 时对话框不给「跳过」，只能更新或退出（用于协议/安全类紧急版本）。
- `minimumVersion` < 已安装版本 → 把已安装版本判为过旧，强制走 `full` 包。

### 4.1 分包策略（体积优化，成本极低）

| 场景 | 下载量 | 通道 |
|---|---|---|
| Qt 依赖未变（绝大多数日常更新） | **~675 KB**（裸 exe） | `kind=app`，下载后由 helper 直接替换 |
| Qt 依赖变了 / 首次安装 / 增量不可用 | ~30 MB（NSIS 安装包，压缩后） | `kind=full`，静默安装接管 |

判定：客户端把安装时的 `runtime` 写进 `preferences.ini` 的 `update/installedRuntime`，
与清单里 `runtime` 逐字符串比较，相同就走 app 增量。

> 这条避免了引入 bsdiff/zstd 等二进制差分工具——只用「依赖层 vs 应用层」拆分，实现成本近乎为零，收益和差分接近。

---

## 5. 客户端模块设计（贴合现有 .pri 分层）

```
src/update/                          # 新增，挂进 src/services.pri
  update.pri                         # QT += network（QtNetwork 主工程已链接，此处显式声明依赖）
  semanticversion.{h,cpp}            # 语义化版本解析与比较（major.minor.patch，支持预发布后缀）
  updatemanifest.{h,cpp}             # latest.json → 值对象；字段缺失一律按「无更新」降级
  updateresolver.{h,cpp}             # 版本 + runtime + 通道 → 选出 app / full / none
  updatechecker.{h,cpp}              # QNetworkAccessManager 拉取清单，URL 候选逐个回退
  updatedownloader.{h,cpp}           # Range 断点续传 + 流式 SHA-256 + 进度/速率/取消
  updateinstaller.{h,cpp}            # 写 plan.json → 拉起 helper/installer → 主程序退出
  updatemanager.{h,cpp}              # 状态机门面：Idle→Checking→Available→Downloading→Ready→Installing

src/ui/panels/update/
  updatenotice.{h,cpp}               # 状态栏右上角低调角标（addPermanentWidget）
  updatedialog.{h,cpp}               # 版本详情 + 更新日志 + 进度条 + 取消/稍后/立即安装

tools/updatehelper/
  main.cpp                           # 纯 Win32（无 Qt），~30KB：等 PID 退出 → 改名备份 → 换文件 → 拉起新版
```

> helper 刻意不用 Qt：它永远不需要更新（换文件时它自己也不能被覆盖），保持零依赖、体积极小。

### 必须遵守的项目既有约定

- **状态栏提示一律走 `MainWindow::showStatusNote(text, ms)`**，绝不用 `showMessage()`
  （后者会隐藏 `addWidget()` 挂的统计行——项目踩过的坑）。
- 新增偏好键全部落在 `update/*`（`channel` / `lastCheck` / `skippedVersion` / `installedRuntime` / `autoCheck`）。
- **UI 测试的 `cleanup()` 必须 `PreferencesStore::remove()` 真删 `update/*`**，否则与其它用例共享 ini 产生串扰。
- 更新相关的 DNS/TLS 失败只能落到 `qWarning`，**绝不弹窗、绝不阻断收发**（数据性能优先）。

---

## 6. 替换机制（Windows 上最容易出错的一环）

Windows **不能覆盖正在运行的 exe**，所以必须具备「第三方进程在我退出后动手」的能力。

### 路线 1：NSIS 安装包（推荐，但引入构建期依赖）

```
1. 下载 easyDebug-x.y.z-win64-setup.exe 到 %LOCALAPPDATA%/easyDebug/updates/
2. 校验 SHA-256
3. QProcess::startDetached(setup.exe, {"/S", "/D=%LOCALAPPDATA%\\Programs\\easyDebug"})
4. qApp->quit()
5. NSIS 脚本：FindProc/等待 easyDebug.exe 退出 → 覆盖安装 → notSilent? 否 → Exec '"$INSTDIR\easyDebug.exe"'
```

- NSIS 同时解决：卸载项、开始菜单/桌面快捷方式、文件关联。
- 依赖：`makensis` + `nsProcess`/`FindProcDLL` 插件（或纯 Win32 `taskkill` 兜底）。

### 路线 2：zip + 自研 helper（零工具链，但要引入解压库）

Qt 没有公开 zip API（`QZipWriter` 在 `gui-private`），需要引入 libzip / QuaZip / minizip。
**因此不推荐作为首发**，留作不希望引入 NSIS 时的备选。

### 路线 3：app.bin + updatehelper（日常更新的主力）

这是 §4.1 里那个 ~675 KB 增量的落地方式，**不需要任何解压库**：

```
plan.json:
{
  "waitPid": 4820,
  "workDir": "%LOCALAPPDATA%/easyDebug/updates/0.2.0-app.bin",
  "operations": [
    { "src": "0.2.0-app.bin", "dst": "%LOCALAPPDATA%/Programs/easyDebug/easyDebug.exe",
      "sha256": "…", "backup": ".bak" }
  ],
  "launch": "%LOCALAPPDATA%/Programs/easyDebug/easyDebug.exe"
}
```

helper 流程：`WaitForSingleObject(openProcess(waitPid))` → 逐个校验 src sha256 → `MoveFileEx(dst, dst.bak, MOVEFILE_REPLACE_EXISTING)` → `MoveFileEx(src, dst)` → `CreateProcess(launch)` → 自行退出。

### 安装位置与 UAC

建议安装到 **`%LOCALAPPDATA%\Programs\easyDebug`（每用户安装）**。
好处：不需要管理员权限 → 静默更新全程无 UAC 弹窗 → 用户体验干净；也和程序现有的 `%LOCALAPPDATA%` 偏好存储定位一致。
（`Program Files` 方案需要 `requireAdministrator` manifest，每次更新都要过 UAC，不推荐。）

---

## 7. 交互设计（不打扰，符合「把空间让给数据区」）

```
┌ 启动时 ────────────────────────────────────────────────────────┐
│ 延迟 5s 异步检查（不阻塞启动）→ 24h 节流（update/lastCheck）    │
└───────────────────────────────────────────────────────────────┘
        │ 有新版本
        ▼
状态栏 permanent 位出现低调蓝色胶囊「有新版本 0.2.0」  ← showStatusNote
   点击 → UpdateDialog：当前版本 / 新版本 / 大小 / 更新日志列表
                        [稍后提醒] [跳过此版本] [立即下载]（宽度走 unifyButtonWidths）
        │ 立即下载
        ▼
对话框内就地变进度条：已下载 x.x MB / y.y MB · 速率 · 剩余时间 · [取消]
        │ 完成 + sha256 通过
        ▼
按钮变 [安装并重启] → 用户点了才真正动手（保留最终决定权）
        │
        ▼
主程序退出 → helper/installer 替换 → 新版本自动启动
```

- **不自动开始下载**：一定有用户的明确点击。避免占带宽、避免突然退化。
- **`required=true` 时**：「稍后提醒」和「跳过此版本」隐藏。
- 菜单入口：`帮助 → 检查更新`（`mainwindow.ui` 里 `menubar` 目前是空的，正好补）。
- 命令行：`--check-update`（手动触发一次并打印结果）、`--no-update-check`（**UI 测试/压测必须默认带上**，杜绝网络串扰）。

---

## 8. 安全

| 措施 | 是否必须 | 说明 |
|---|---|---|
| HTTPS | **必须** | 强制；任何 http URL 直接拒绝 |
| SHA-256 校验 | **必须** | 下载流式计算，比对失败即删文件、标记失败、可换下一个 URL 候选重试 |
| 清单 must-fail 降级 | 必须 | 清单解析失败/字段缺失 → 一律当作「暂无更新」，绝不当作有更新 |
| 代码签名 / EV 证书 | 建议 | 未签名的安装包会触发 SmartScreen 蓝框；起步阶段在更新日志里说明「仍要运行」。有预算建议补 EV 证书 |
| 安装包完整性签名（minisig / Ed25519） | 可选 | 防 CDN/镜像被劫持。实现成本不高，建议 P3 补 |
| 回退 | 建议 | 替换前留下 `.bak`，`帮助 → 回退上一版` 手动恢复；健康检查式自动回滚放 P4 |

---

## 9. 发布流水线

`.github/workflows/release.yml`，**push tag `v*.*.*` 触发**：

```
1. checkout
2. （可选）由 tag 写回 version/version.json
3. 装 Qt 6.10.1 MSVC2022（用 jurplel/install-qt-action 或自托管 runner）
4. qmake && nmake release
5. 复用现有 scripts/deploy_qt_windows.ps1 做 windeployqt
6. 产出三件套：
     easyDebug-<ver>-app.bin            （裸 exe，供增量）
     easyDebug-<ver>-win64-setup.exe    （NSIS 打包）
     easyDebug-<ver>-win64.zip          （便携版，可选）
7. 生成 latest.json（含各产物 size + sha256 + runtime），从 Release notes 取 notes[]
8. softprops/action-gh-release 上传 Release + latest.json 附件
```

> 本地也可以跑（不依赖 Actions）：写一个 `scripts/publish_update.ps1` 做同样的事情，手工 `gh release create`。

---

## 10. 配置与开关（`preferences.ini`）

```ini
[update]
autoCheck=true          ; 关闭则完全不联网检查
channel=stable          ; stable | beta
lastCheck=1760000000    ; Unix 秒，节流用
skippedVersion=         ; 用户点过「跳过此版本」
installedRuntime=       ; 安装时的 runtime 标识，分包判定用
```

企业/离线环境：`帮助用户把它们降级到 AutoUpdateLevel=None`（注册表 HKLM 策略）或程序内设置均可杀死检查。

---

## 11. 实施分期

| 阶段 | 内容 | 产出 |
|---|---|---|
| **P0** | `semanticversion`、`updatemanifest`、`updateresolver` + 单元测试（纯 computations，无网络） | 版本比较与清单解析 100% 可测 |
| **P1** | `updatechecker`（URL 候选回退）+ `updatemanager` 节流 + `帮助→检查更新` + 命令行开关 | 点菜单能拿到结果，但不下载 |
| **P2** | `updatedialog` + `updatedownloader`（续传/进度/取消/SHA-256） | 完整的「提示 → 下载」 |
| **P3** | `tools/updatehelper` + app.bin 增量替换 + 备份回退 | 日常 ~675 KB 升级跑通 |
| **P4** | NSIS 安装包 + full 通道 + GitHub Actions 发布流水线 | runtime 变更也能平滑升级 |
| **P5** | 签名、Ed25519 manifest 签名、渠道灰度（百分比放量） | 生产级 |

建议顺序：**P0 → P1 → P3 → P2 → P4**。理由：先把「能不能安全替换 exe」这个最大风险点验证掉，UI 打磨放在后面。

---

## 12. 需要你拍板的开放问题

1. **仓库**：现在 `git remote -v` 是空的。要不要现在就在 GitHub 上建 `easyDebug` 仓库并把 remote 配上？（决定我了是否能直接搭 Actions 流水线）
2. **是否引入 NSIS**：不引入的话日常增量能用（P3），但「Qt 依赖变了」的升级只能靠用户手动下便携版；引入了就能全类型覆盖（P4）。倾向：**先 P3 跑通，NSIS 放到 P4 视需要再做**。
3. **安装位置**：`%LOCALAPPDATA%\Programs\easyDebug`（无 UAC，推荐）还是 `Program Files`（有 UAC）？
4. **更新通道**：现阶段是否需要 beta 通道？如果不要，P0~P2 可以省掉 channel 相关分支。
