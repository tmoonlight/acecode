## 1. 停止发布 Deepin ARMv7

- [x] 1.1 从 `package.yml` 的 Deepin 矩阵移除 `linux-deepin-armv7` 及其交叉编译依赖、ARMv7 校验步骤和交叉 binutils 前缀
- [x] 1.2 必需产物改为 17 个，混入的 Deepin ARMv7 归档按意外资产拒绝，并补测试
- [x] 1.3 更新 `docs/deepin-desktop.md`、`docs/release-artifacts.md` 与帮助站点安装页（`docs/help-source/getting-started.json` 重新生成）

## 2. stapler 断线重试

- [x] 2.1 新增 `scripts/macos_stapler.sh`，只对退出码 68 重试
- [x] 2.2 公证、安装器 DMG、更新 ZIP、安装器构建脚本的 stapler 调用全部改走 `acecode_stapler`
- [x] 2.3 契约测试覆盖重试成功、持续失败、非 68 不重试，并拒绝直接 `xcrun stapler`
- [x] 2.4 更新 `docs/macos-release.md`、`docs/release-artifacts.md`

## 3. 验证

- [x] 3.1 本机运行 `verify_release_assets_test.py`、`macos_release_scripts_test.sh`（含变异检查）、Shell 语法检查与 `git diff --check`
- [ ] 3.2 PR 的 `macos-installer-tests`（macOS 上的 `macos_installer_release_test.py`）通过
- [ ] 3.3 下一次 `v*` 标签构建确认 Deepin 只产出 x64 / arm64，macOS 两个架构完整
