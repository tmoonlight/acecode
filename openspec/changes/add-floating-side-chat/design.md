<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`ChatView` currently owns a single result, separate inline `SideQuestionComposer`, and `SideQuestionCard`. `api.askSideQuestion` waits for a complete HTTP response. `AgentLoop::ask_side_question` copies an already primed provider-facing snapshot and performs one tool-free call; providers also expose `chat_stream` with an independent abort flag. Global body portals and `notifyNativeSurfaceOverlayChange` already solve app-shell stacking and native preview occlusion.

## Goals / Non-Goals

**Goals:** Keep one owner for side-request lifecycle, preserve detached context semantics, and reuse existing themes, Markdown rendering and WebSocket authentication.

**Non-Goals:** Persisting another session, write/shell/MCP tools in side chat, modifying TUI interaction, OS-level always-on-top windows outside ACECode, or changing main composer behavior.

## Decisions

1. Render a nonmodal body portal above the app shell and ordinary overlays, with `data-ace-native-overlay="overlap"`. Use a compact title header with clear and minimize actions, spacious independently scrolling messages, and a 60px bottom input region including borders, with send/stop in the same location. The horizontal-line minimize button hides the window while preserving its session-local transcript and draft; its label is `最小化侧边聊天`. Long drafts scroll within the compact textarea. Match the supplied composition with existing ACECode tokens; avoid a backdrop. Pointer capture handles header drag and eight resize directions; geometry is clamped to viewport with a readable minimum that contracts on narrow screens. Announce native occlusion after geometry changes.
2. Keep a testable side-chat controller separate from `ChatView`; it owns transcript, draft, active request and epoch. All Web `/side`, `/btw` and menu entries use this one owner. Closing cancels and hides; reopening in the same view retains transcript/draft/geometry. Session changes cancel the old request and retain session-isolated state under the configurable retention policy below. A synchronous active-request guard prevents double submission; stale callbacks are ignored by request identity. Only completed and stopped nonempty answers enter follow-up history.
3. Use a dedicated connection to the existing authenticated `/ws/sessions/<id>` endpoint without `hello` or main subscriptions. Client sends `side_chat_start` payload `{session_id, request_id, question, history:[{role,content}]}` and `side_chat_stop` payload `{request_id}`. Server sends `side_chat_delta` `{request_id,delta}`, `side_chat_reset` `{request_id}`, `side_chat_done` `{request_id,answer,cancelled}`, or `side_chat_error` `{request_id,code,message}`. The API helper returns a cancel function and terminates each connection when finished. Reusing the main subscription was rejected because side lifecycle and errors must remain private.
4. Server-side provider execution copies the safe main snapshot, appends detached instructions, validated alternating side user/assistant pairs, and the new question. It uses no tools and a request-owned abort flag. Retry clears provisional answer. Validate history role/order/size before model invocation. Own worker lifetime through the server/connection state; disconnect aborts, safe connection identity prevents sends to a recycled pointer, and shutdown joins workers. Existing sync API stays compatible.
5. Disable the textarea throughout connect/wait/stream/stop acknowledgement, with an accessible progress indicator and enabled stop until requested. Preserve partial text on cancel and errors. Use actual provider deltas rather than simulated character animation. Markdown shares existing parsing/copy behavior; keyboard handling respects IME, Shift+Enter, and Escape inside the window, while underlying main-task keyboard handlers cannot consume float input.
6. Cancellation wakes only waits whose request abort flag is set; it must not increment the provider's global retry-wake generation or shorten a main-task Retry-After delay. Providers expose whether they support tool-free requests. Codex's native app-server ignores the caller's tool list and has its own tool runtime, so its side requests are explicitly rejected until that adapter can guarantee tool-free execution. Regular model providers retain the existing empty-tool-list contract.
7. Main and side requests share their configured provider. Copilot token refresh and token snapshots therefore use a short credential lock; network model calls remain concurrent. This prevents concurrent refresh from racing a request's authorization header construction.
8. Add a speech-bubble SVG action before the trajectory action in the session header, using the existing `openSideQuestionComposer` entry and shared icon rendering. Add `clear()` to the existing controller: invalidate/cancel the active request, discard turns and draft, release the input, and preserve visibility. Late callbacks cannot repopulate cleared history or alter a fresh request. The trash button calls this action and focuses the textarea; the next request sends empty side history while retaining the usual main-session snapshot.

9. Read-only tools (follow-up fix): with an empty tool list the model copied the main session's `bash` calls into the answer as angle-bracket text. Streaming side chat now offers only the registered, policy-allowed built-ins `file_read`, `grep`, `glob` and `lsp` (`side_chat_tools.cpp`, an allowlist rather than `is_read_only`, which also marks sub-agent and question tools). The daemon runs each call after Deny rules, the main path validation and the dangerous-path check, refusing anything that would need a prompt, inside `MtimeTracker::DetachedReadScope` so the main agent's read baseline and unchanged-read cache stay untouched. Results feed back for at most 8 steps; `side_chat_tool` frames report progress and `side_chat_reset` now discards only the current step. Text-form tool calls are discarded and corrected up to twice and never displayed. The synchronous HTTP and TUI `/btw` / `/side` questions stay single-turn but run the same loop, report `tools_used`, and reject native-tool providers; `stop_requests` cancels any in-flight side request.

### Window size and toolbar placement refinement

Initialize the window at 600 by 620 CSS pixels, contracting only when the viewport cannot contain it. Use a 36px border-box header with 28px clear/minimize buttons. Each toolbar bubble activation places the window's top-left at the button's top-left, then clamps the window inside the visible viewport with the existing 12px margin. Preserve the user's resized dimensions. Menu and slash-command reopening retain the previous position; subsequent drag and viewport resizing continue to use the existing geometry constraints.

## Risks / Trade-offs

### 侧聊存续时间（2026-10-15）

- 设置 → 常规，在工作模式之后增加「侧边聊天存续时间」数字输入，单位分钟，默认 30，支持正整数；沿用 UI 偏好 hook 持久化配置，修改后下次切回立即使用最新值。
- 使用页面内存中的会话控制器缓存，按 daemon 端口、工作区与主会话 ID 隔离；聊天内容不写磁盘，刷新或重启后仍为临时状态。空会话不进入缓存。
- 控制器通过可注入时钟记录最后一个侧聊回合完成、停止或失败的时间；切走中断生成视为停止，并保留非空的部分回答。仅打开窗口、编辑未发送草稿或输入校验失败不启动或延长聊天结束计时。
- 切回该会话的 layout effect 检查到期：距最后结束时间达到配置分钟数时重置该会话；未到期则恢复回答、追问上下文、草稿和窗口开闭状态。切换、最小化、打开与草稿编辑不续期，正在生成的回合不判断过期。当前会话停留超过时间也不被定时清空。
- 切走只停止生成，保持原窗口开闭状态以便切回恢复；迟到回调仍由请求身份隔离。明确清空会重置计时与该会话记录，不影响其他会话。
- 缓存获取时更新 transport 委托，避免恢复控制器持有旧 API 凭证。React StrictMode 的 effect 清理只挂起请求，不丢弃缓存。

- [Large temporary history] → Bound request history and return clear errors without silent truncation or modifying the main context.
- [Native preview covers DOM] → Use the existing overlap coordinator and notify on mount, move, resize and unmount.
- [Late network events / shutdown] → Guard by connection/request identity, independent abort and explicit cleanup; cover with lifecycle tests.
- [No restored side transcript after refresh] → Temporary view-scoped state matches the existing detached behavior; persistence is outside this change.
- [Older daemon lacks streaming messages] → Surface its protocol error and unlock input; do not fake streaming or stop.
- [Native agent providers execute their own tools] → Report unsupported side chat before invoking the native agent; users can select a provider that supports tool-free requests. This does not alter native main-task execution.

## Migration Plan

### 2026-10-31 复用完整输入框

按用户追加要求，移除侧聊专用 textarea，直接使用 InputBar/RichComposer。只通过显式 props 隐藏权限与加号；拖入文件、会话引用、图片、粘贴、编辑历史、模型菜单及键盘行为由共享组件负责。提取主会话已有的提交规范化和附件上传步骤供两处调用，不另造拖放处理。

临时侧聊控制器持有结构化 composer content、附件资源、文本历史、排队提交快照和模型/推理选择。收起、切换或组件重挂载不丢草稿；清空、到期和销毁释放本地资源并使在途上传回填失效。上传失败保留可重试资源；提交时绑定模型和附件快照，不清空随后输入的新草稿。主会话草稿持久化位置保持独立，侧聊在销毁前保留完整草稿。

私有 WS 增加与主输入相同的 input 对象（附件 ID、会话引用、composer_content），历史用户消息保存相同的 input。服务器逐条复用 parse_session_user_input_request 校验和解析，禁止 worktree 副作用并保留图片 content_parts；不接受客户端提供的已解析附件路径。独立模型使用已有 saved-model resolver 和 provider factory 创建请求自有 provider，默认选中打开侧聊时的主模型，切换仅影响后续侧聊请求，主会话 provider/配置/历史不变。

### 2026-10-31 入口与输入修订

侧聊从会话菜单移到「打开浏览器」左侧，使用原有 chat SVG 和 aria-pressed 表达开闭状态；开启仍复用既有控制器，收起仍保留记录与草稿并停止侧聊请求。浮窗开启时放在工具栏下方，避免挡住 toggle。此决定替代旧的轨迹按钮前入口与顶部重合锚点。

输入区取消固定 60px 总高度，至少可见三行并随内容增长，到上限后内部滚动；Enter 与 Shift+Enter 沿用主会话快捷键配置并保留 IME 保护。提取主会话发送按钮为共享组件，统一圆形强调色、图标、停止中的禁用状态及无障碍名称。自动焦点不再移到停止按钮，键盘主动导航仍有可见焦点。

生成和等待停止时继续编辑；主按钮按 getInputBarActionState 切换发送、停止、排队和继续。队列由侧聊控制器按主会话持有，通过微任务在成功结束后启动下一条，重新校验历史大小并保留新草稿。停止、失败、收起和切换会话暂停队列，主动继续或发送新消息才恢复；空闲时新消息先发，再恢复旧队列；取消条目、清空、到期和迟到回调都不能意外启动请求。此修订替代旧的生成中禁用输入要求。

Ship frontend and daemon together. Retire the two inline Web components when all entries use the float; retain the synchronous HTTP endpoint for existing clients. Validate focused backend/controller/API cases, the full Web test suite and production build, OpenSpec strict validation, and browser checks across desktop/narrow viewports and light/dark themes.
