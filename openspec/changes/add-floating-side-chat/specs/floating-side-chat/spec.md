## Purpose

Provide a movable, resizable side conversation above the ACECode application so users can discuss the current session across multiple turns, watch and stop live answers, and keep that discussion separate from the main task.

## ADDED Requirements

### Requirement: Reuse the rich composer
Side chat SHALL reuse the main InputBar and RichComposer, hiding only the permission selector and plus control. It SHALL support the same file, image and session-reference drop/paste/edit behavior and attachment-only submission. Its model and reasoning controls SHALL select the provider for subsequent side requests without changing the main session.

#### Scenario: Drop content into side chat
- **WHEN** a user drops a file, image or another session into the side composer
- **THEN** the shared editor represents and submits it using the main composer's attachment/reference contract
- **AND** the main composer and draft are unchanged

### Requirement: Retain complete side drafts until destruction
The side controller SHALL preserve rich content, attachment resources, queued submissions, input history and model selection through hiding, session switching and remounting. Clear, expiry or destruction SHALL release temporary resources and invalidate late uploads. Failed uploads SHALL remain recoverable. Queued submissions SHALL snapshot content and model, retaining newer edits.

#### Scenario: Restore a rich draft
- **WHEN** a side draft containing text, file/image attachments and a session reference is hidden and reopened before expiry
- **THEN** its editor content, resources and model selection are restored
- **AND** a late upload after clear or expiry cannot recreate the destroyed draft

### Requirement: Resolve rich side inputs through shared boundaries
The server SHALL resolve side input and historical user inputs through the main input parser, verifying attachment ownership and session references and retaining multimodal content. It SHALL NOT write the side discussion into main history or permit worktree creation. Explicit model selection SHALL use a request-owned provider and fail clearly when unavailable.

#### Scenario: Follow up on an image with a different model
- **WHEN** a user sends an image, completes the side turn and selects another model for a follow-up
- **THEN** the selected provider receives the image in the isolated side history
- **AND** the main provider remains unchanged

### Requirement: Application-wide floating conversation
The Web/Desktop UI SHALL open a themed floating side conversation above application surfaces through `/side`, `/btw`, menus and the session-toolbar SVG toggle. Its header SHALL show title, clear and minimize actions, using an accessible horizontal-line minimize icon. The window SHALL support dragging, eight-way resizing, independent transcript scrolling and viewport constraints.

#### Scenario: Open and resize above the workbench
- **WHEN** the user opens side chat with the sidebar and preview visible
- **THEN** the window overlays those areas without changing the main layout and can be moved and resized within the viewport
- **AND** the minimize control remains reachable after viewport resizing or zoom changes

#### Scenario: Open without a question
- **WHEN** the user submits `/side` without arguments in an existing session
- **THEN** the empty floating conversation opens and focuses its input

#### Scenario: Open from the session toolbar
- **WHEN** the user clicks the speech-bubble toggle immediately left of the browser button in the session toolbar
- **THEN** the same side-chat window toggles open or closed, preserving existing side history and both composer drafts; its pressed state SHALL match visibility

### Requirement: Initial size and toolbar placement
The window SHALL initialize at 600 by 620 CSS pixels, limited by the visible viewport, and use a compact 36px header. Opening from the toolbar bubble SHALL place the window below the button, clamping inside the viewport and shrinking its height when necessary to keep the toggle reachable. Toolbar reopening SHALL preserve user-resized dimensions when they fit. Other entry points SHALL preserve the previous position.

#### Scenario: Open at the toolbar button
- **WHEN** the user activates the toolbar bubble with enough space to its right and below
- **THEN** the window's left aligns with the button and its top is below the button
- **AND** its initial size is 600 by 620 CSS pixels

#### Scenario: Open near a viewport edge
- **WHEN** the anchored window would extend beyond the visible viewport
- **THEN** the window moves inside the viewport with the existing margin and contracts only if its size exceeds the available viewport

### Requirement: Isolated multi-turn conversation
The system SHALL answer each side question using a safe main-session context snapshot plus the preceding successful or nonempty stopped side turns. It SHALL keep the side transcript temporary and independent from main-session history, tools, hooks, goals, event stream, and busy state. Failed or empty stopped turns SHALL remain visible without being added to future model context.

#### Scenario: Follow-up question
- **WHEN** the user sends a follow-up after a side answer
- **THEN** the model receives the preceding side question and answer along with current main-session context
- **AND** the main conversation and task state are unchanged

#### Scenario: Provider cannot guarantee a tool-free conversation
- **WHEN** the selected provider runs its own tools and cannot disable them for a side request
- **THEN** the system refuses the side request before invocation with a clear unsupported-provider error
- **AND** the main task remains available without changing its provider behavior

### Requirement: Read-only tools in side chat
Streaming side chat and the single-turn TUI `/btw` / `/side` and HTTP side questions SHALL offer the model only the read-only built-in tools `file_read`, `grep`, `glob` and `lsp` that the session has registered and its capability policy allows, and SHALL never offer shell, write, sub-agent or MCP tools. Each call SHALL pass configured Deny rules, the main session's path validation and the dangerous-path check, and SHALL leave the main agent's read state unchanged.

#### Scenario: Read a file to answer
- **WHEN** a side question needs a file that is not in the main context
- **THEN** the model reads it through `file_read` and the answer continues after the result
- **AND** the main agent's later read of that file returns its content rather than an unchanged-file stub

### Requirement: Side chat tool call presentation
Each side chat tool call SHALL be shown as a tool row at the position where it happened. Tool calls written as plain text SHALL be discarded and corrected rather than displayed, and calls that would require a confirmation prompt SHALL be refused.

#### Scenario: Model attempts a shell command
- **WHEN** the model calls `bash` or writes a tool call as angle-bracket text
- **THEN** nothing is executed, the markup is not shown, and the model is told that only the read-only tools are available

### Requirement: Confirmed sending to the main session
The floating side chat SHALL let the user pass a message to the main session through the side model's `send_to_main_session` tool or a send action on a completed side answer, which prefills its question and answer. The user SHALL review and may edit the message, and nothing SHALL be delivered before the user confirms.

#### Scenario: Decline or stop before confirming
- **WHEN** the user declines the drafted message, or stops or closes the side chat while it awaits confirmation
- **THEN** nothing reaches the main session and the side model is told so

### Requirement: Delivery into the main session
A confirmed side chat message SHALL be added to the main session's running turn and read at its next model step without interrupting it; when no turn accepts it, it SHALL start or queue a new turn. It SHALL appear in the main session as a user message marked as coming from the side chat, and the side chat SHALL show how it was delivered.

#### Scenario: Relay a correction while the main agent works
- **WHEN** the main session is running and the user asks the side chat to tell the main agent to change approach
- **THEN** after confirmation the main agent reads the message at its next step
- **AND** the side chat reports that it was added to the current turn

### Requirement: Quote main-session text into the side chat
The user SHALL be able to send selected main-session text into the side chat input as a quote.

#### Scenario: Ask about selected main-session text
- **WHEN** the user selects text in the main transcript and sends it to the side chat
- **THEN** the side chat opens with the selection quoted in its input and the cursor after it

### Requirement: Streaming progress and cancellation
The UI SHALL show loading and actual streamed output, with a stop control that cancels only the active side request and retains partial text. Retried attempts SHALL replace provisional text. The input SHALL remain editable while generating or stopping. Concurrent requests SHALL be prevented, and failure SHALL preserve partial output and pause the queue.

#### Scenario: Waiting and streaming
- **WHEN** a side request is waiting for its first token or producing answer text
- **THEN** loading is visible, editing remains available, concurrent side requests are prevented, and stop remains available with an empty draft

#### Scenario: Stop then continue
- **WHEN** the user stops an answer and subsequently asks another question
- **THEN** generation stops, partial text remains readable, queued messages pause, the input remains available, and the new request works independently

#### Scenario: Failure or disconnection
- **WHEN** the provider fails or the stream disconnects unexpectedly
- **THEN** the UI reports the failure, preserves readable partial output, and pauses queued messages and keeps the input available for another attempt

### Requirement: Conversation lifecycle and compatibility
Minimizing SHALL hide the window, cancel any active side request and preserve the temporary transcript and draft while the same main session remains selected. Switching sessions or leaving the view SHALL cancel the old side request and retain its temporary state separately for that main session under the retention policy. Late callbacks SHALL NOT affect another request or session. Existing synchronous side-question HTTP and TUI callers SHALL retain their single-turn behavior.

#### Scenario: Minimize and reopen
- **WHEN** the user minimizes and reopens side chat in the same session
- **THEN** its previous temporary transcript and unsent draft remain available

#### Scenario: Clear the current side conversation
- **WHEN** the user clicks the trash button, including while loading, streaming, or awaiting a stop acknowledgement
- **THEN** the active side request is cancelled, side turns and draft are cleared, and the window stays open with an editable input
- **AND** late events cannot restore cleared content or affect a fresh request, whose side history is empty
- **AND** the main conversation, draft, and running task are unaffected

#### Scenario: Switch sessions during streaming
- **WHEN** the user switches the main session during a side answer
- **THEN** the old side request is cancelled and its subsequent events cannot change the new session's side chat

### Requirement: 可配置的侧边聊天存续时间
设置「常规」SHALL 提供「侧边聊天存续时间」正整数配置，单位分钟、默认 30，并持久化用户选择。每个主会话的侧聊 SHALL 从最后一个回合完成、停止或失败开始计时。切回主会话时 SHALL 使用最新配置检查到期，未到期恢复临时记录、草稿及窗口开闭状态，到期清空该会话侧聊。当前正在查看的侧聊 SHALL NOT 因时间到期自动消失；仅切换、打开、最小化或修改草稿 SHALL NOT 延长最后聊天结束时间。

#### Scenario: 在存续时间内切回
- **WHEN** 侧聊回答结束后用户切到另一主会话，并在配置时间内切回
- **THEN** 原侧聊记录、未发送草稿和原窗口开闭状态恢复，追问仍携带原有成功或非空停止回答
- **AND** 另一主会话的侧聊保持独立

#### Scenario: 达到时间后切回
- **WHEN** 距最后一次侧聊结束恰好达到或超过配置分钟数，用户切回该主会话
- **THEN** 原侧聊记录和草稿清空，窗口关闭，重新打开后追问历史为空
- **AND** 一直停留在该会话直到到期不会自动清空

#### Scenario: 长回答及续聊
- **WHEN** 侧聊回答生成耗时超过存续时间，随后结束并开始新的侧聊回合
- **THEN** 生成期间不判断过期，并从最新回合结束重新计时
- **AND** 切走中断生成时保留部分回答，从停止时开始计时，迟到事件不能更新其他会话

#### Scenario: 修改配置与重新挂载
- **WHEN** 用户修改存续时间并切回之前的主会话，或聊天视图重新挂载
- **THEN** 检查使用最新配置，未到期的内存状态仍可恢复；配置在刷新后保留，聊天记录不做磁盘持久化

### Requirement: Private authenticated streaming transport
The streaming side interface SHALL require the existing daemon authentication, validate session identity, request identity, text-only roles and bounded payloads, and deliver side output only to the requesting connection. Cancellation and disconnect SHALL release request resources without affecting a main task.

#### Scenario: Invalid or concurrent input
- **WHEN** a client sends invalid history or starts a second side request on the same active connection
- **THEN** the daemon returns a structured error without starting another provider call

### Requirement: Side composer queue and keyboard focus
Side chat SHALL send queued questions serially after success, using current history and preserving newer drafts. Stop, failure, minimizing or session switching SHALL pause the queue. Reopening alone SHALL NOT resume it. Explicit continuation or a new submission SHALL resume it; a new idle submission SHALL run before older queued items. Users SHALL be able to cancel items. Queues SHALL stay isolated per main session and be discarded on clear or expiry.

#### Scenario: Continue writing while an answer streams
- **WHEN** a user submits a follow-up while a side answer is running
- **THEN** the follow-up is queued and the composer clears for further writing
- **AND** successful completion starts the queued question with the preceding answer in history without clearing a newer draft

#### Scenario: Stop with pending questions
- **WHEN** the user stops, minimizes or leaves a side conversation with queued questions
- **THEN** pending questions remain paused and are not sent by reopening the window alone
- **AND** with an empty idle draft the primary button offers continuation, while each queued item can be cancelled

#### Scenario: Send from the multiline input
- **WHEN** the user presses the configured newline shortcut or confirms IME composition
- **THEN** the input remains multiline and no question is sent by composition confirmation
- **AND** after a normal submission the input retains focus without an automatic ring around the stop button

### Requirement: Shared composer action and multiline input
Main and side composers SHALL share primary-button styles, icons and action selection: idle sends, busy with an empty draft stops, busy with text queues, and idle with an empty draft and paused queue resumes. Stop acknowledgement SHALL disable repeated stopping. The side editor SHALL use the main rich composer's multiline size, growth and internal scrolling rules.

#### Scenario: Submit while stop acknowledgement is pending
- **WHEN** the user explicitly queues a new question during stop acknowledgement
- **THEN** it waits for the current request to end before the queue resumes
- **AND** no simultaneous side request is created

### Requirement: Preserve composer keyboard focus
The side input SHALL retain focus after sending without automatically focusing the stop button. Keyboard navigation SHALL retain a visible focus indicator. IME confirmation SHALL NOT submit; configured send and newline shortcuts SHALL match the main composer, and empty Enter while busy SHALL NOT stop generation.

#### Scenario: Send without focusing stop
- **WHEN** a user sends from the side input or clicks its send button
- **THEN** focus remains in or returns to the textarea while the primary button becomes stop
- **AND** the stop button has no automatic focus ring, but keyboard navigation can still focus it visibly
