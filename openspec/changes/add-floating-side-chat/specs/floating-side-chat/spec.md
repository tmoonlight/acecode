## Purpose

Provide a movable, resizable side conversation above the ACECode application so users can discuss the current session across multiple turns, watch and stop live answers, and keep that discussion separate from the main task.

## ADDED Requirements

### Requirement: Application-wide floating conversation
The Web and Desktop UI SHALL open side chat from `/side`, `/btw`, the existing side-chat menu, or a speech-bubble SVG button in the top-right session toolbar in a floating window above the application surfaces. The floating header SHALL contain the conversation title and top-right clear and minimize actions, in that order. The minimize action SHALL use a horizontal-line icon and an accessible label describing minimization. The window SHALL support dragging, resizing on all edges and corners, independent transcript scrolling, and a fixed bottom input region totaling 60px including borders. Long drafts SHALL scroll inside the textarea without expanding that region. It SHALL remain usable within the current viewport and use the application's theme.

#### Scenario: Open and resize above the workbench
- **WHEN** the user opens side chat with the sidebar and preview visible
- **THEN** the window overlays those areas without changing the main layout and can be moved and resized within the viewport
- **AND** the minimize control remains reachable after viewport resizing or zoom changes

#### Scenario: Open without a question
- **WHEN** the user submits `/side` without arguments in an existing session
- **THEN** the empty floating conversation opens and focuses its input

#### Scenario: Open from the session toolbar
- **WHEN** the user clicks the speech-bubble button before the trajectory button in the session toolbar
- **THEN** the same side-chat window opens, preserving existing side history and the main composer draft

### Requirement: Initial size and toolbar placement
The window SHALL initialize at 600 by 620 CSS pixels, limited by the visible viewport, and use a compact 36px header. Each activation of the toolbar bubble SHALL align the window's top-left with the button's top-left, clamping inside the viewport when necessary. Toolbar reopening SHALL preserve user-resized dimensions. Other entry points SHALL preserve the previous position.

#### Scenario: Open at the toolbar button
- **WHEN** the user activates the toolbar bubble with enough space to its right and below
- **THEN** the window's left and top match the button's left and top
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
Streaming side chat and the single-turn TUI `/btw` / `/side` and HTTP side questions SHALL offer the model only the read-only built-in tools `file_read`, `grep`, `glob` and `lsp` that the session has registered and its capability policy allows, and SHALL never offer shell, write, sub-agent or MCP tools. The daemon SHALL execute each call only after configured Deny rules, the main session's path validation and the dangerous-path check, refuse any call that would require a confirmation prompt, and leave the main agent's read state unchanged. Each call SHALL be shown as a tool row at the position where it happened. Tool calls written as plain text SHALL be discarded and corrected rather than displayed.

#### Scenario: Read a file to answer
- **WHEN** a side question needs a file that is not in the main context
- **THEN** the model reads it through `file_read`, a tool row shows the path, and the answer continues after the result
- **AND** the main agent's later read of that file returns its content rather than an unchanged-file stub

#### Scenario: Model attempts a shell command
- **WHEN** the model calls `bash` or writes a tool call as angle-bracket text
- **THEN** nothing is executed, the markup is not shown, and the model is told that only the read-only tools are available

### Requirement: Streaming progress and cancellation
The UI SHALL show loading from submission, render actual streamed answer text, and provide a stop control for the active side request. The input SHALL be disabled from submission until completion, failure, or cancellation. Stopping SHALL cancel only the side model request and retain text already produced. Retried provider attempts SHALL replace provisional text rather than concatenate failed attempts.

#### Scenario: Waiting and streaming
- **WHEN** a side request is waiting for its first token or producing answer text
- **THEN** loading is visible, typing and duplicate submission are disabled, and stop remains available

#### Scenario: Stop then continue
- **WHEN** the user stops an answer and subsequently asks another question
- **THEN** generation stops, partial text remains readable, the input becomes available, and the new request works independently

#### Scenario: Failure or disconnection
- **WHEN** the provider fails or the stream disconnects unexpectedly
- **THEN** the UI reports the failure, preserves readable partial output, and releases the input for another attempt

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
