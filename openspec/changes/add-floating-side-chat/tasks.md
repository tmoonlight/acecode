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

## 7. 侧聊入口与输入一致性（2026-10-31）

- [x] 7.1 将会话菜单中的侧聊入口移到打开浏览器左侧，作为有选中状态的 toggle，保留会话隔离和收起语义。
- [x] 7.2 改为可见多行、内容增长后内部滚动的输入区，复用主会话发送／停止按钮，修正自动聚焦导致的蓝圈。
- [x] 7.3 验证真实 ChatView 的入口、草稿、多行、发送／停止、焦点和窄屏明暗主题，运行 Web 测试、构建与严格校验。
- [x] 7.4 生成中允许编辑并排队；成功后顺序发送，停止、失败、收起或切换后暂停，支持继续与取消，并覆盖隔离和迟到回调。

## 8. 复用完整输入框（2026-10-31）

- [x] 8.1 侧聊复用 InputBar/RichComposer，只隐藏权限和加号控件；使用现有文件、图片、会话拖入和粘贴行为。
- [x] 8.2 保存结构化草稿、附件资源、排队快照及独立模型选择直到侧聊销毁；复用主输入的上传与提交规范化，覆盖异步上传和清空竞态。
- [x] 8.3 侧聊传输及后端复用主输入解析，支持图片、文件与会话引用的多轮上下文；按请求选择模型和推理强度而不修改主会话。
- [x] 8.4 运行前端与针对性原生测试、构建及 OpenSpec 校验，浏览器验证拖入、草稿恢复、模型菜单和隔离，记录实测边界。

## 9. 展开与收起动画（2026-11-03 集成验收）

- [x] 9.1 从当前会话工具栏按钮展开、最小化时缩回按钮；按钮不可见时原地淡入淡出，支持反向操作接续和减少动态效果。
- [x] 9.2 保留会话切换、草稿和排队状态的既有语义；收起动画完成后卸载，清理被取消的动画和帧回调。
- [x] 9.3 运行动效纯逻辑测试、Web 全量测试及构建，并验证真实 ChatView/SettingsPage 的打开、收起、会话切换、富输入、模型设置、窄屏和明暗主题。

第 9 节验证：`pnpm test`、`pnpm build` 和 `node scripts/test-side-chat-retention.mjs` 通过。浏览器测试在打开模型设置后等待收起动画完成再断言窗口卸载，避免依赖旧的立即卸载时序。截图位于系统临时目录 `ace-side-chat-retention-iVHVSg`；API/WebSocket 和桌面桥接仍使用测试替身。

第 8 节验证（2026-10-31）：Web 全量 `pnpm test`、`pnpm build`、严格 OpenSpec 与 `git diff --check` 通过。Windows Release `acecode_unit_tests` 构建成功，`SideChat*.*:SideQuestionService.*:SessionRegistry.SideQuestion*` 共 41 项通过，覆盖富输入协议、图片历史、独立 provider、取消及原有只读工具隔离。共享输入框 `test-composer-file-drop.mjs` 通过；真实 ChatView 的 `test-side-chat-retention.mjs` 验证文件/图片/会话拖入、完整草稿恢复、主草稿隔离、模型/推理切换、历史附件、长文本粘贴编辑、预览/菜单/编辑弹窗层级、模型设置入口及明暗窄屏。截图目录为系统临时目录 `ace-side-chat-retention-r9ibzO`，设计检测无发现。浏览器 API/WebSocket 与桌面文件系统桥接使用测试替身，未调用真实模型或重启桌面；本轮原生构建目标是单元测试。

验证（2026-10-31）：`pnpm test`、`pnpm build`、`pnpm i18n:audit`、`openspec validate add-floating-side-chat --strict` 和 `git diff --check` 通过。`test-composer-actions.mjs` 验证共享主按钮全部动作与键盘焦点；`test-side-chat-retention.mjs` 在真实 ChatView/SettingsPage 中验证入口顺序、toggle、草稿、多行增长/滚动、IME、发送/排队/停止/继续、会话隔离和存续，覆盖 1440px/390px 明暗主题。测试夹具使用与应用一致的稳定空权限列表，修正旧夹具触发的滚动 effect 更新循环。截图位于系统临时目录 `ace-side-chat-retention-s9fFrt`。设计检测无发现。使用模拟 API/WebSocket 验证前端，未调用真实模型、重建桌面二进制或重启桌面。
