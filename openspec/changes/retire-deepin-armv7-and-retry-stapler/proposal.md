## 为什么

v1.0.7 的 `v*` 标签构建有两个平台失败，最后都靠基于同一 tag 源码的恢复工作流补出来：

- `linux-deepin-armv7`：buster 容器里 `archive.debian.org` 的 CDN 对确实存在的 `.deb` 长期返回缓存 404，交叉编译依赖装不上。用户决定以后不再发布 Deepin ARMv7 包，不为它补下载容错。
- `macos-x64`：签名后 `xcrun stapler` 反复连不上 Apple 票据服务（CloudKit `-1005`，退出码 68），重跑三次每次断在不同的 stapler 调用上。v1.0.6 也遇到过同样的错误。一个 job 要调十几次 stapler，任何一次断线都会让整个发布失败。

## 变更内容

- 从 `package.yml` 的 Deepin 构建矩阵移除 `linux-deepin-armv7`，连同它专用的交叉编译依赖、ARMv7 可执行文件校验和交叉 binutils 前缀。
- 正式发布的必需产物从 18 个减为 17 个；`acecode-linux-deepin-armv7.tar.gz` 出现时按意外资产拒绝发布。普通 `acecode-linux-armv7.tar.gz` 不变。
- 新增 `scripts/macos_stapler.sh` 的 `acecode_stapler`：只对退出码 68 重试，默认最多 6 次、间隔 15 秒；其它退出码立即返回。macOS 打包脚本与安装器构建脚本里的 stapler 调用全部改走它。
- 契约测试覆盖重试成功、持续 68 后放弃、非 68 不重试，并拒绝新增直接 `xcrun stapler` 调用。

## 能力

### 新增能力

- `macos-stapler-retry`：macOS 打包流程对 Apple 票据服务的瞬时断线重试，其它 stapler 失败照常终止。

### 修改能力

- `deepin-release-packaging`：Deepin 专用发行族只保留 x64 / arm64 两个归档。

## 影响

- `.github/workflows/package.yml` 的 Deepin 构建 job。
- `scripts/verify_release_assets.py` 与 `tests/scripts/verify_release_assets_test.py`。
- `scripts/macos_notarize_app.sh`、`scripts/macos_notarize_pkg.sh`、`scripts/macos_create_installer_dmg.sh`、`scripts/macos_create_update_zip.sh`、`packaging/macos-installer/build.sh`，以及 `tests/scripts/macos_installer_release_test.py`、`tests/scripts/macos_release_scripts_test.sh`。
- `docs/deepin-desktop.md`、`docs/release-artifacts.md`、`docs/macos-release.md`，帮助站点安装页（`docs/help-source/getting-started.json` 及生成的 `docs/help`）。
- 已安装 Deepin ARMv7 包的用户不再收到新版本；升级代码里的 `linux-deepin-armv7` 更新标识保持不变（Deepin 包本来就不进自动更新清单）。
