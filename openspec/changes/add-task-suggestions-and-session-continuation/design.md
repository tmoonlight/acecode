<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md. ThreadService already creates persistent sessions and queues first input. Web session routes can attach structured source-session references and create a worktree before the first input. AgentLoop persists summary checkpoints but current window numbers also interact with repair/fork; normal @ references only include a bounded recent visible transcript.

## Goals / Non-Goals

The domain service owns suggestion state, execution location and startup receipts. Web/Desktop are the interactive card surfaces; in-process AI tools can propose or withdraw suggestions. Existing direct thread tools and subagent orchestration remain distinct. This change does not build general multi-agent scheduling, automatically merge side work, copy uncommitted changes into new worktrees, or publish a release.

## Decisions

1. A project-local SQLite suggestion store uses stable source/target IDs, bounded validated text, deduplication, and atomic acceptance. States are pending, queued, starting, started, failed and dismissed. Runtime callbacks must not own raw short-lived HTTP/service objects. Record a target ID before provisioning, and reuse it after recoverable failure. Initial input carries the suggestion identity so recovery can recognize already accepted input.
2. A shared service serves tools and authenticated session HTTP routes. Tools only propose/dismiss; user acceptance selects current/worktree. The source scope and actual execution directory are resolved by the host, not model arguments. Current-directory launches queue at an idle worker boundary; known busy sessions/descendants sharing the directory defer launch. Worktree launches use a verified source commit and cannot fall back to shared execution. Source and target worktree ownership must not cause shared-directory deletion.
3. Successful auto/manual summary checkpoints determine compaction count, excluding repair/mechanical fallback and honoring fork reset boundaries. Threshold defaults to three, zero disables it. Once a continuation suggestion exists for a source, no repeat reminder is generated. Persisted suggestions remain authoritative across restarts; no counting UI warning rows.
4. Handoff captures the latest state at the safe worker boundary using the latest semantic summary plus bounded recent state. Repair checkpoints and hidden automatic goal inputs do not replace that summary or user constraints. Include source reference metadata and bounded goals/todos/constraints/workspace/evidence, with on-demand read_thread access. Preserve effective model, permission and expert settings, and independently construct production providers for concurrent tasks. Source automatic goal continuation is paused once its successor has accepted the first input. Background tasks and pending user input must not be silently dropped or run concurrently with the transferred main work.
5. GET session suggestions returns persisted records, source/workspace busy and worktree availability. Accept/dismiss POST returns a suggestion envelope. Host lifecycle work handles queued acceptance independently of tab focus; reconnect also recovers interrupted phases. Expected failures are shown on the card with retry against the same target.
6. A compact non-modal card is attached to the chat's upper right, uses theme tokens, supports keyboard and narrow screens, and never steals focus. Apply the theme's large and ordinary shadow tokens to each card, and reserve padding inside the scrolling container so overflow does not clip the shadow while preserving the visible card position. Side tasks expose a split location action and stay in the source view; continuation starts in the same directory and navigates to its successor. Polling is bounded/single-flight and source-scoped, with stale responses ignored. Unsupported old backends silently omit this optional surface.

### 2026-09-19：建议卡片自动关闭

- 控制器为首次展示的 `pending` 建议记录绝对截止时间（30 秒），使用可注入时钟和定时器；轮询刷新不得重置倒计时，多张卡片各自计时，销毁控制器时清理所有定时器。
- 到期沿用既有 dismiss 请求持久化关闭，不触发 accept。用户开始任务、手动关闭或建议进入排队／启动／失败状态后停止本次计时，防止到期取消已接受的任务；关闭失败保留卡片和重试操作，不自动反复请求。
- 倒计时文字位于底部细线左上方，中文严格为“xx秒后关闭”，英文为“Closes in xx s”。进度由满到空，右端以小火苗随剩余长度左移，最后 5 秒略微提亮；火苗保持约 6×9px，以柔和外焰和微亮内芯轻微摆动、伸缩及明暗变化，不添加粒子或扩大光晕。沿用主题 token、原有标题、按钮和关闭图标。减少动态效果时按秒更新长度，关闭平滑移动、火苗闪动与光晕。
- 倒计时只负责当前展示周期；切换会话销毁旧控制器，重新进入尚未关闭的建议时开始新的 30 秒展示。已成功关闭的建议仍由服务端过滤。
- 任务位置菜单通过 `AnchoredMenu` 挂载至 `document.body`；调用方必须显式提供主题实底、边框、圆角、内边距、阴影和高于建议卡片的层级，避免选项透底或被卡片遮挡。保持既有定位、键盘选择及焦点恢复行为。


## Risks / Trade-offs

- A shared directory can also be modified by external editors or processes. Serialize known session work and tell the user the location is shared; existing file read guards still apply.
- A frozen commit does not include uncommitted changes. Show this beside worktree selection and require the new task to verify its assumptions in its actual checkout.
- Summary generation can fail or omit evidence. Preserve both conversations, make failures retryable, retain original user constraints and source reference access, and instruct the new session to verify live state.
- Restart can occur between provisioning and input submission. Persist target identity and accepted input intent, recognize the durable user-message receipt, and recover missing delivery to the same target. Hiding a started card does not cancel its accepted input.

## Migration Plan

Additive project-local storage initializes lazily. Existing successful checkpoints provide the reminder baseline. Existing sessions and ordinary thread APIs keep their behavior until a suggestion is accepted. API docs describe new routes, lifecycle states, the threshold, and actual execution-location semantics.
