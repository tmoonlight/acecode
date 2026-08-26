---
id: chat-scroll-001
scope: web-chat/transcript-window
status: done
depends-on: []
---

# objective

让大会话尾部窗口在 `transcript_replace` 重建临时 item id 后继续使用稳定消息
边界，并在边界确实消失时同步恢复有界窗口，消除后续高速流式输出的全量 DOM
退化与滚动抖动。

# context

- `docs/INDEX.md`
- `docs/web-chat/README.md`
- `docs/web-chat/transcript-window.md`
- `docs/plan/analysis/stabilize-long-session-transcript-window.md`
- `openspec/changes/stabilize-long-session-transcript-window/proposal.md`
- `openspec/changes/stabilize-long-session-transcript-window/design.md`
- `openspec/changes/stabilize-long-session-transcript-window/specs/web-chat-transcript-window/spec.md`

# path

- `docs/INDEX.md`
- `docs/web-chat/`
- `docs/plan/analysis/stabilize-long-session-transcript-window.md`
- `docs/plan/backlog.md`
- `docs/plan/tasks/chat-scroll-001.md`
- `openspec/changes/stabilize-long-session-transcript-window/`
- `web/src/components/ChatView.jsx`
- `web/src/lib/transcriptWindow.js`
- `web/src/lib/transcriptWindow.test.js`

# verification

- 测试真实 reducer `transcript_replace` 重建链路，而不只手工构造相同 id 的数组。
- 断言大会话替换前后保持隐藏历史且可见窗口有界。
- 断言稳定 `messageId` 保持边界，缺失边界同步重选，显式全量状态保持。
- 确认没有新增无条件 `scrollTop` 写入，tail-follow/review 逻辑不变。
- 在 `web/` 运行 `pnpm test`。
- 在 `web/` 运行 `pnpm build`。
- 运行 `openspec validate stabilize-long-session-transcript-window --strict`。
- 运行 `git diff --check`。

# delivery

- 两轮独立 verify 完成；首轮发现的短 transcript 旧 key 问题已修复，第二轮
  Findings 为零并通过交付。
- 合并后的 `master` 已通过完整 `pnpm test`、`pnpm build`、OpenSpec strict
  validation 和 diff check。
