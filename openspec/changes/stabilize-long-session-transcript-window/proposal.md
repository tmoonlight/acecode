## Why

大会话通过尾部窗口限制同步 DOM 数量，但 retry `transcript_replace` 会重建所有
临时 item id，使旧窗口边界失效并永久退化为全量历史渲染。后续高速流式输出
因此反复协调数百个 Markdown、工具和 diff 行，表现为当前会话特有的卡顿与滚动
抖动。

## What Changes

- 将 transcript window 边界从 reducer 临时 item id 改为跨历史重建稳定的消息
  身份，并为 key 加命名空间以避免偶然碰撞。
- 在 `renderedItems` 替换后、窗口切片前同步协调边界：保留仍存在的稳定边界，
  对真正消失的边界重新选择尾部窗口。
- 保留用户显式“显示全部”、逐段揭示、会话查找、回合跳转和既有
  tail-follow/review 语义。
- 增加真实 reducer `transcript_replace` 回归测试，证明大会话替换前后继续保持
  有界 DOM，且没有一次全量渲染帧。
- 在 `docs/web-chat/` 记录窗口身份、状态机、错误回退和验证契约。

## Capabilities

### New Capabilities

- `web-chat-transcript-window`: 定义 Web/Desktop 大会话尾部窗口的稳定边界、
  历史替换协调、显式全量状态与内容安全回退。

### Modified Capabilities

- None.

## Impact

- Affected frontend code: `web/src/components/ChatView.jsx`,
  `web/src/lib/transcriptWindow.js` and focused tests.
- Affected documentation: `docs/web-chat/`, `docs/plan/` and `docs/INDEX.md`.
- No daemon protocol, provider retry, session persistence, projection rules, dependency,
  or API changes.
- Existing `chatScrollFollow.js` remains the authority for viewport movement; this change
  does not add automatic `scrollTop` writes.
