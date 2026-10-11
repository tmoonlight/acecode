## 1. 实现

- [x] 1.1 封装保留 ACECode.app 名称的更新目录和空目录清理生命周期；以目录独占、名称和备份保留回归测试验证。
- [x] 1.2 接入 macOS 原生更新器并拒绝对备份自身更新；审查所有复制、移动、验证和回滚分支，并通过路径校验测试。
- [x] 1.3 更新 Mac 发布说明，解释新旧备份路径及更新后重启行为；核对文档和实现一致。

## 2. 验证

- [x] 2.1 执行定向原生测试、OpenSpec strict 校验和 git diff --check，记录结果与平台限制。
- [ ] 2.2 在 Mac 上编译原生更新器并验证签名更新、故障回滚，以及更新前后权限弹窗的实际名称。

## 验证记录（2026-10-11）

- WSL Ubuntu 使用 g++ C++17 编译实际 `macos_bundle.cpp`、`encoding.cpp` 和 `macos_bundle_test.cpp`（仓库现有 GoogleTest），10 项全部通过，0 跳过。包括 0700 权限、已有目录/符号链接拒绝、应用名称、成功备份、恢复后空目录清理、未恢复副本保留，以及备份不可自更新。
- `openspec validate preserve-macos-update-app-name --strict` 通过。
- 本次五个源代码/测试/文档文件 `git diff --check` 通过。
- 当前为 Windows 主机，未编译 Objective-C++ 原生更新器、未使用 Developer ID 签名应用做实际更新，未复现 TCC 弹窗；2.2 保持待验。未发布、未提交。
