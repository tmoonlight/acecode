## Why

用户反馈 macOS 权限弹窗出现 `.acecode-xxxx-xxxx`，无法辨认请求者。代码中存在高度吻合的来源：自更新把仍可能运行的旧应用重命名为 `.ACECode-<UUID>.previous.app`；系统可能采用实际 bundle 文件名。当前 Windows 环境尚未复现用户的系统弹窗。

## What Changes

- 自更新的暂存、备份和失败隔离应用均保持 `ACECode.app` 名称，随机编号及阶段信息移入外层私有目录。
- 保持同卷替换、签名验证、失败回滚和备份保留行为；清理仅删除本次创建的空容器。
- 更新目录中的副本不得被作为正式安装位置再次自更新。
- 补充路径、回滚文件操作和清理回归验证，以及 Mac 实机待验说明。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `self-upgrade`: macOS 更新期间保持应用名称及备份恢复能力。

## Impact

涉及 `src/adapters/upgrade/macos_bundle.*`、`macos_app_installer.mm`、对应测试与 `docs/macos-release.md`。不改签名内容、bundle identifier、权限范围或发布配置；不自动迁移或删除旧备份。
