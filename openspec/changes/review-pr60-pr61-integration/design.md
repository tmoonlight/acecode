<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

当前 PR 检查仅覆盖 Linux。图形安装器使用 `.acecode-install.lock`，更新器使用 `.ACECode.update.lock`；前者打开既有 FIFO 时可能阻塞。顶部导航四个 24px 按钮与三个 4px 间距已占满原来的 108px。

## Goals / Non-Goals

完成两个 PR 的审核、必要修复和合并。生产签名、公证、干净非管理员 Mac 的端到端安装仍属于实际发布验收，本任务不执行发布。

## Decisions

- 为顶部导航定义统一最小宽度，将新增 8px 左边距计入桌面及窄窗口规则。
- 图形安装器沿用既有更新器锁名与锁安全策略：非阻塞、禁止跟随符号链接，检查普通文件、单硬链接及当前用户属主，保持锁 inode 不删除。
- 使用真实文件系统故障注入验证互斥、异常锁、复制失败与回滚；在 GitHub macOS runner 运行原生 Swift 测试及双架构 UI 编译。
- `NSApplication.shared` 必须先于含窗口属性的 UI delegate 构造；原生冒烟测试运行完整应用包并检查个人/系统页面截图，覆盖编译无法发现的启动错误。
- 真实打包在 `hdiutil create` 遇到 `Resource busy`：明确使用 HFS+，仅对该错误最多尝试三次，每次使用不同的临时输出；其他错误立即失败。可信 PKG 的生成、公证和上传先于 DMG，正式 Release 仍要求整个构建成功。
- 本次 macOS 自定义目录和双架构 DMG 规范取代 `add-macos-self-update`、`allow-release-without-macos-pkg` 等旧变更里的目录白名单和禁用 DMG 条款；签名、版本、更新 ZIP 校验与可选 PKG 规则继续适用。

## Risks / Trade-offs

- 共享目录内他人拥有的锁会被明确拒绝，避免对不可信锁操作；用户可选择个人可写目录。
- 离线测试能验证顺序和失败行为，不能证明生产公证或 Gatekeeper 下载隔离行为；发布文档保留这些验收限制。
