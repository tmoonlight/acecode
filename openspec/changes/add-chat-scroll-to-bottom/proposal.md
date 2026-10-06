## Why

用户查看较早的会话内容后，需要一个明确、随手可点的入口返回最新消息。

## What Changes

- 消息区未到底时，在底部中央显示圆形向下箭头悬浮按钮。
- 点击后回到底部，解除活动展开锚定并恢复后续消息跟随。
- 复用主题通用阴影，位置避开变更审查浮层，并提供中英文可访问名称。
- 会话仍在运行时，将按钮内的箭头替换为从左到右依次跳动的三点；结束后恢复箭头，点击行为不变。

## Capabilities

### New Capabilities
- `web-chat-scroll-to-bottom`: 会话消息区回到底部入口及其显示、定位、跟随行为。

### Modified Capabilities

无。

## Impact

涉及 Web/Desktop 共用的 ChatView、滚动状态机和静态文案目录；不修改后端协议。
