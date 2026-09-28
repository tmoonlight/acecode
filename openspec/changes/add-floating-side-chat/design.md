<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`ChatView` currently owns a single result, separate inline `SideQuestionComposer`, and `SideQuestionCard`. `api.askSideQuestion` waits for a complete HTTP response. `AgentLoop::ask_side_question` copies an already primed provider-facing snapshot and performs one tool-free call; providers also expose `chat_stream` with an independent abort flag. Global body portals and `notifyNativeSurfaceOverlayChange` already solve app-shell stacking and native preview occlusion.

## Goals / Non-Goals

**Goals:** Keep one owner for side-request lifecycle, preserve detached context semantics, and reuse existing themes, Markdown rendering and WebSocket authentication.

**Non-Goals:** Persisting another session, tools in side chat, modifying TUI interaction, OS-level always-on-top windows outside ACECode, or changing main composer behavior.

## Decisions

1. Render a nonmodal body portal above the app shell and ordinary overlays, with `data-ace-native-overlay="overlap"`. Use a compact title header with clear and minimize actions, spacious independently scrolling messages, and a 60px bottom input region including borders, with send/stop in the same location. The horizontal-line minimize button hides the window while preserving its session-local transcript and draft; its label is `最小化侧边聊天`. Long drafts scroll within the compact textarea. Match the supplied composition with existing ACECode tokens; avoid a backdrop. Pointer capture handles header drag and eight resize directions; geometry is clamped to viewport with a readable minimum that contracts on narrow screens. Announce native occlusion after geometry changes.
2. Keep a testable side-chat controller separate from `ChatView`; it owns transcript, draft, active request and epoch. All Web `/side`, `/btw` and menu entries use this one owner. Closing cancels and hides; reopening in the same view retains transcript/draft/geometry. Session changes reset. A synchronous active-request guard prevents double submission; stale callbacks are ignored by request identity. Only completed and stopped nonempty answers enter follow-up history.
3. Use a dedicated connection to the existing authenticated `/ws/sessions/<id>` endpoint without `hello` or main subscriptions. Client sends `side_chat_start` payload `{session_id, request_id, question, history:[{role,content}]}` and `side_chat_stop` payload `{request_id}`. Server sends `side_chat_delta` `{request_id,delta}`, `side_chat_reset` `{request_id}`, `side_chat_done` `{request_id,answer,cancelled}`, or `side_chat_error` `{request_id,code,message}`. The API helper returns a cancel function and terminates each connection when finished. Reusing the main subscription was rejected because side lifecycle and errors must remain private.
4. Server-side provider execution copies the safe main snapshot, appends detached instructions, validated alternating side user/assistant pairs, and the new question. It uses no tools and a request-owned abort flag. Retry clears provisional answer. Validate history role/order/size before model invocation. Own worker lifetime through the server/connection state; disconnect aborts, safe connection identity prevents sends to a recycled pointer, and shutdown joins workers. Existing sync API stays compatible.
5. Disable the textarea throughout connect/wait/stream/stop acknowledgement, with an accessible progress indicator and enabled stop until requested. Preserve partial text on cancel and errors. Use actual provider deltas rather than simulated character animation. Markdown shares existing parsing/copy behavior; keyboard handling respects IME, Shift+Enter, and Escape inside the window, while underlying main-task keyboard handlers cannot consume float input.
6. Cancellation wakes only waits whose request abort flag is set; it must not increment the provider's global retry-wake generation or shorten a main-task Retry-After delay. Providers expose whether they support tool-free requests. Codex's native app-server ignores the caller's tool list and has its own tool runtime, so its side requests are explicitly rejected until that adapter can guarantee tool-free execution. Regular model providers retain the existing empty-tool-list contract.
7. Main and side requests share their configured provider. Copilot token refresh and token snapshots therefore use a short credential lock; network model calls remain concurrent. This prevents concurrent refresh from racing a request's authorization header construction.
8. Add a speech-bubble SVG action before the trajectory action in the session header, using the existing `openSideQuestionComposer` entry and shared icon rendering. Add `clear()` to the existing controller: invalidate/cancel the active request, discard turns and draft, release the input, and preserve visibility. Late callbacks cannot repopulate cleared history or alter a fresh request. The trash button calls this action and focuses the textarea; the next request sends empty side history while retaining the usual main-session snapshot.

### Window size and toolbar placement refinement

Initialize the window at 600 by 620 CSS pixels, contracting only when the viewport cannot contain it. Use a 36px border-box header with 28px clear/minimize buttons. Each toolbar bubble activation places the window's top-left at the button's top-left, then clamps the window inside the visible viewport with the existing 12px margin. Preserve the user's resized dimensions. Menu and slash-command reopening retain the previous position; subsequent drag and viewport resizing continue to use the existing geometry constraints.

## Risks / Trade-offs

- [Large temporary history] → Bound request history and return clear errors without silent truncation or modifying the main context.
- [Native preview covers DOM] → Use the existing overlap coordinator and notify on mount, move, resize and unmount.
- [Late network events / shutdown] → Guard by connection/request identity, independent abort and explicit cleanup; cover with lifecycle tests.
- [No restored side transcript after refresh] → Temporary view-scoped state matches the existing detached behavior; persistence is outside this change.
- [Older daemon lacks streaming messages] → Surface its protocol error and unlock input; do not fake streaming or stop.
- [Native agent providers execute their own tools] → Report unsupported side chat before invoking the native agent; users can select a provider that supports tool-free requests. This does not alter native main-task execution.

## Migration Plan

Ship frontend and daemon together. Retire the two inline Web components when all entries use the float; retain the synchronous HTTP endpoint for existing clients. Validate focused backend/controller/API cases, the full Web test suite and production build, OpenSpec strict validation, and browser checks across desktop/narrow viewports and light/dark themes.
