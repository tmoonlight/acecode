# 正式发布产物约定

`.github/workflows/package.yml` 的 `v*` 标签构建必须生成下列 18 个用户下载包。发布任务通过 `scripts/verify_release_assets.py` 检查每个文件只有一个非空副本，并拒绝不属于当前版本的包、未签名包或重名文件；检查失败时不创建 GitHub Release。

发布前先检查 `assets/models_dev/MANIFEST.json` 的快照时间不超过 30 天。刷新模型目录后必须执行 `pnpm --dir web sync:provider-logos`，同步官方图标与生成映射，再运行前端全量测试和构建；目录、图标 manifest 和映射的提供商数量必须一致。图标仍执行去重、单文件 100 KiB 和总量 576 KiB 上限，以及脚本/远程内容拒绝检查。

各平台打包可以与测试并行。发布任务还必须等待同一提交在 master 上的 `test.yml` 检查全部成功；测试缺失、失败或超时都不能发布。

| 平台 | 架构 | 必需文件 |
| --- | --- | --- |
| Windows | x64、arm64 | `acecode-windows-<arch>.zip` |
| Linux | x64、arm64、armv7 | `acecode-linux-<arch>.tar.gz` |
| UOS / Deepin | x64、arm64、armv7 | `acecode-linux-deepin-<arch>.tar.gz` |
| Linux 自动更新 | x64、arm64 | `acecode-<version>-linux-<arch>-update.zip` |
| macOS | x64、arm64 | `acecode-macos-<arch>.tar.gz` |
| macOS 安装器 | x64、arm64 | `ACECode-<version>-macos-<arch>.dmg` 和 `.pkg` |
| macOS 自动更新 | x64、arm64 | `ACECode-<version>-macos-<arch>-update.zip` |

Deepin 构建是发布任务的必需依赖，不能因已有普通 Linux 包而跳过。它替代原 Linux old 流水线，保持 GLIBC 2.28 基线；x64/arm64 开启 `ACECODE_DEEPIN`，使用 WebKitGTK 4.0 和 DTK 5，ARMv7 保持 CLI-only。依赖及升级方式见 [UOS / Deepin 专用构建](deepin-desktop.md)。macOS 正式发布同时要求 Developer ID Application、Developer ID Installer 证书及公证凭据；安装器证书或密码缺失必须失败，不允许发布没有 PKG 的正式版。手动触发的开发构建仍可在缺少签名凭据时生成非正式产物，不创建 Release。

调试符号使用 `dev_only.` 文件名前缀；避免方括号等会被 GitHub 改写的字符，使 `SHA256SUMS.txt` 中的文件名与下载名一致。

完整正式版同步到 aupdate 时，验证六类更新 ZIP 和两个 PKG 的版本、大小、哈希及公网下载。Linux 更新 ZIP 使用 `linux-x64-updater-v1`、`linux-arm64-updater-v1` manifest target。PKG 是独立手动下载项，不写入自动更新 manifest。

本地可运行 `python tests/scripts/verify_release_assets_test.py` 验证缺包、空包、重复包和错误版本的阻断逻辑；macOS CI 另验证签名、公证、安装器布局与打包脚本。
