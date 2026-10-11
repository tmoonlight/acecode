## Why

桌面窗口经 Alt+Tab、最小化恢复或最大化后，聊天输入框可能显示光标但收不到键盘输入，需要再次点击。用户要求窗口重新获得焦点时，有可用聊天输入框就自动恢复输入，Windows 优先并覆盖 macOS、Linux。

## What Changes

- 补齐原生窗口激活、WebView 键盘焦点和聊天光标之间的恢复流程。
- 窗口重新激活时，即使此前使用过终端或预览，也回到当前聊天输入框。
- 保留草稿和选区；后台窗口、隐藏聊天和正在交互的阻塞弹框不执行恢复；取消失效的异步恢复。
- 补充 Windows 原生及跨浏览器回归验证，并分别记录其他平台的实现和实测状态。

## Capabilities

### New Capabilities
- `desktop-chat-focus`: 桌面窗口重新激活时恢复当前可用聊天输入框的键盘焦点。

### Modified Capabilities
无。

## Impact

涉及 desktop WebHost、原生 bridge、ChatView 和 composerCaretRestore；不增加依赖，不改变消息提交协议。
