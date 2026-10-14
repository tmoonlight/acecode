## 1. Backend

- [x] 1.1 Add isolated multi-turn streaming provider calls and validate history; verify focused tests cover context, retry/reset, tools, and independent cancellation.
- [x] 1.2 Add private authenticated WebSocket start/stop messages with safe worker/connection cleanup; verify protocol and cancellation tests and document daemon API.

## 2. Web conversation

- [x] 2.1 Add a side-stream API helper and testable conversation controller; verify follow-up history, duplicate submission, cancellation, errors and stale callback tests.
- [x] 2.2 Build the themed floating window with clear and minimize actions, drag/eight-way resize, Markdown, loading and disabled composer; verify geometry tests and browser interactions.
- [x] 2.3 Route Web slash commands and menu into the float, retire inline UI and regenerate translations; verify routing/i18n tests and session isolation.

## 3. Integration verification

- [x] 3.1 Run full Web tests and production build, focused C++ tests/build, OpenSpec strict validation and diff whitespace checks; record exact coverage and any environment limitations.
- [x] 3.2 Inspect desktop/narrow and light/dark browser renders; verify multi-turn streaming, stop, minimize/reopen, session switch, geometry and top-level overlay behavior.

## 4. Session toolbar and compact composer refinement

- [x] 4.1 Add a session-toolbar speech-bubble SVG entry using the existing side-chat opener.
- [x] 4.2 Add a clear action to the controller and window header; verify cancellation, draft/history reset, and late-event isolation with focused tests.
- [x] 4.3 Compact the entire bottom input region to 60px including borders, preserving multiline editing through internal scrolling.
- [x] 4.4 Regenerate translations, run Web tests/build and strict OpenSpec/diff checks, and verify toolbar/clear/height behavior in desktop and narrow light/dark browser renders.

## 5. Initial size and toolbar placement refinement

- [x] 5.1 Set initial geometry to 600 by 620 (revised 2026-09-17) and compact the header to 36px.
- [x] 5.2 Anchor toolbar activation to the bubble button's top-left, retaining viewport constraints and resized dimensions.
- [x] 5.3 Verify geometry/placement, run Web tests/build and strict OpenSpec/diff checks, and record validation limits.

Validation (2026-09-16): geometry tests, full `pnpm test`, `pnpm build`, strict OpenSpec validation and `git diff --check` passed. Browser fixture measured 880x800 initial size and a 36px header; verified toolbar anchoring/clamping, resized dimensions and draft retention on reopen, and 1440px/390px light/dark layouts without page errors. No desktop binary rebuild or live-provider test was performed. Test/build required running outside the sandbox because its MSYS shell could not initialize.

Size revision (2026-09-17): changed initial dimensions to 600x620. Geometry tests, production build, strict OpenSpec validation and scoped diff checks passed. Full Web tests failed in `settingsNavigation.test.js` because the current settings list includes the separately added security center while the expectation does not. No desktop rebuild was performed.

## 6. 侧聊存续时间（2026-10-15）

- [x] 6.1 在常规设置新增分钟配置（默认 30、正整数），接入偏好持久化、设置搜索和中英文文案。
- [x] 6.2 按主会话缓存侧聊控制器，切走停止并保留，切回按最后聊天结束时间判断过期；覆盖回合结束、停止、失败、草稿、跨会话隔离和迟到回调。
- [x] 6.3 验证浏览器中的真实设置与聊天切换行为，运行 Web 测试、生产构建、i18n 和 OpenSpec 严格校验，记录验证范围。
