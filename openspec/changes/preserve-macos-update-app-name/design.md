## Context

见 proposal.md。自更新在旧进程仍存活时移动整个应用；桌面重启为后续单独操作。主应用 `CFBundleName` / `CFBundleDisplayName` 已为 ACECode，但更新备份的实际文件名包含 UUID。Apple 文档说明显示名称可能采用实际文件名，TCC 还依赖 responsible code 归属；这里确认了不合适的命名代码，尚未确认用户机器的具体 TCC 归属。

参考：[Apple Display Names](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPFileSystem/Articles/DisplayNames.html)、[On File System Permissions](https://developer.apple.com/forums/thread/678819)。前者说明文件名与显示名称关系，不作为特定 TCC 弹窗已复现的证据。

## Goals / Non-Goals

目标是更新各阶段始终保持应用叶子目录名 `ACECode.app`，且完整保留签名与故障恢复。范围仅为内置 macOS 自更新，不扩展为权限管理或安装器重构，不改变重启时机。

## Decisions

- 单次更新创建同卷私有目录 `.acecode-update-<UUID>`，其中 `staging/ACECode.app`、`previous/ACECode.app` 和 `failed/ACECode.app` 分别承载三个阶段。随机标识只进入外层目录。
- 在已有 portable bundle 模块中封装目录所有权；根目录独占创建，POSIX 权限为 0700，已有目录或符号链接拒绝接管。析构只删除自己创建的空目录，任何仍含应用的目录必须保留。
- 原有复制、信任验证、两步移动与回滚顺序保持。成功备份仍由现有返回值和诊断日志公开实际路径。
- 安装位置校验拒绝 `.acecode-update-*` 下的副本，避免保持名称后把备份当作正式安装再次升级。
- 不直接修改运行中应用的 plist；这会破坏封装签名。单纯改 `CFBundleDisplayName` 也无法消除实际文件名的 UUID。

## Risks / Trade-offs

- [TCC 名称选择受 macOS 版本和启动方式影响] → 源码和文件操作测试只证明名称与恢复约束；系统弹窗列为 Mac 实机待验，不能声称通过。
- [旧版本仍按旧逻辑升级] → 新逻辑随新版本运行后生效，旧版本执行的那次更新仍可能留下旧名字；正常退出并重新打开正式安装可结束旧进程。
- [回滚失败] → 只清理空容器，备份及失败副本保留，错误继续给出可恢复路径。

## Migration Plan

随常规版本发布，不在本任务发布或提交。不重命名或删除历史 `.ACECode-<UUID>.previous.app`；文档记录新旧两类备份。Mac 验收覆盖更新完成但未重启时触发文件权限、重启后申请及回滚故障。
