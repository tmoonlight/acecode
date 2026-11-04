## Why

“我的定时任务”缺少单次立即执行入口。撤销/重做仍由编辑器默认按键处理，无法在快捷键设置里修改。

## What Changes

- 定时任务行增加播放按钮，通过 daemon 调度器立即执行一次，保留原定时间与启用状态。
- 为撤销/重做增加可持久化快捷键，默认 Ctrl+Z / Ctrl+Shift+Z（macOS 使用 Cmd）。
- 富文本输入、Markdown 编辑与普通文本输入遵循当前绑定；快捷键录入不执行编辑动作。
- 快捷键设置沿用现有冲突检测与跨窗口同步，并按设置页规则即时保存。

## Capabilities

### New Capabilities

- `scheduled-task-manual-run`: 定时任务单次手动执行及并发保护。
- `editable-history-shortcuts`: 文本撤销/重做的可配置快捷键。

### Modified Capabilities

无。

## Impact

LoopStore、LoopScheduler、daemon/web 依赖和 API 文档；LoopPage、快捷键注册表与设置、Slate 编辑器、原生文本输入。无需新依赖或数据库迁移。
