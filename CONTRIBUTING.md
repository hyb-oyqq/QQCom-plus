# 贡献指南

## 分支和提交

从 `master` 创建短生命周期分支，推荐使用以下前缀：

- `feature/`：新功能
- `fix/`：缺陷修复
- `chore/`：构建、文档和工具调整
- `perf/`：性能优化

提交标题使用简短的 Conventional Commits 风格，例如 `fix: 修复 TCP 服务器关闭时的尾帧处理`。一次提交只解决一个主题，避免把生成物、个人配置或大段无关格式化混入提交。

## 提交前检查

1. 执行 `git diff --check`。
2. 使用 `build.bat` 构建 Release。
3. 修改服务层或公共控件时执行对应的 core/UI 测试。
4. 修改版本或发布脚本时运行 `scripts/validate_ota_manifest.ps1` 的示例校验。
5. 确认 `git status --short` 中没有 `build/`、`debug/`、`release/`、`version/version.h` 等生成物。

## 版本和发布

版本号遵循 `主版本.次版本.修订版本`：

- 破坏兼容性时递增主版本。
- 新增向后兼容能力时递增次版本。
- 修复问题或仅调整发布内容时递增修订版本。

发布包必须由 `scripts/package_release.ps1` 生成，并通过 `scripts/validate_ota_manifest.ps1` 校验。生产发布还必须完成代码签名和人工更新演练，具体要求见 [OTA 方案](docs/OTA.md)。
