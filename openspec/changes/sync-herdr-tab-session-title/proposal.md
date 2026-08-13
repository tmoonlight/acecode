## Why

ACECode 已经会把生成或设置的会话标题写入终端标题，但 Herdr 顶部 tab 仍保留创建时名称，用户无法从 tab 栏识别各会话。现有生命周期 hook 也没有标题变化事件，seed 无法可靠取得最终标题并同步给 Herdr。

## What Changes

- 新增 `SessionTitleChanged` hook 事件，在当前会话标题被设置、清除或恢复时携带最终 `title` 与变更来源。
- 让 TUI、daemon/Web 及共享会话标题更新路径按一致语义派发标题事件，并避免未实际生效的标题产生事件。
- 更新默认 Herdr managed seed：收到非空标题时使用当前 pane 对应的 tab ID 重命名 Herdr tab；标题清空时不覆盖用户可见 tab 名。
- 修复历史官方 Herdr seed 因本地 seed 状态漂移而被误判为用户修改的问题，确保已经初始化的用户也能收到标题 handler。
- 保持 Herdr 不可用、环境变量缺失或 CLI 调用失败时静默降级，不影响 ACECode 标题更新。
- 增加 hook 载荷、派发、seed 安装升级及假 Herdr CLI 回归测试，并更新集成文档。

## Capabilities

### New Capabilities

- `session-title-hook`: 定义 `SessionTitleChanged` 的触发时机、载荷以及跨运行表面的一致性。
- `herdr-session-title-sync`: 定义默认 managed hook 如何把 ACECode 会话标题安全同步到当前 Herdr tab。

### Modified Capabilities

无。

## Impact

- Hook 事件注册、载荷构造和 session 标题更新入口。
- TUI、daemon/Web 会话生命周期及相关单元测试。
- `assets/seed/hooks/agent-reporting/hooks.json`、seed 版本/manifest/官方指纹与安装升级测试。
- 默认 seed reconcile 对已知历史官方 hook 定义的迁移识别。
- `docs/herdr-hooks.md` 及 OpenSpec 行为契约。
