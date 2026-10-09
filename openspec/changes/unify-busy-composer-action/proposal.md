## Why

会话运行时，输入框并列显示中断和排队按钮，空草稿仍占用一个禁用的排队按钮。用户希望底部只保留一个主题色图标按钮，根据草稿内容切换停止或排队。

## What Changes

- 运行中且无可提交内容时，显示正方形停止图标，点击中断当前任务。
- 运行中且有文本或附件等可提交内容时，显示排队 SVG 图标，点击或按 Enter 使用既有排队提交路径。
- 按钮位置和尺寸固定，背景始终使用主题色；停止请求在途时防止重复停止。
- 保留空闲发送、暂停队列继续、输入法和 Shift+Enter 换行行为。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `busy-input-queue`: 将并列的中断与排队动作改为随草稿内容切换的单一图标按钮。

## Impact

影响共享的 `InputBar`、`inputBarState`、图标源及对应测试、生成的 SVG 和 i18n 源文案目录。无需修改后端/API、队列生命周期或 Desktop 原生壳。
