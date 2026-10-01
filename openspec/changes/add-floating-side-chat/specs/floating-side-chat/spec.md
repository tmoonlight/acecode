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
Minimizing SHALL hide the window, cancel any active side request and preserve the temporary transcript and draft while the same main session remains selected. Switching sessions or leaving the view SHALL cancel the old side request and discard its temporary state. Late callbacks SHALL NOT affect another request or session. Existing synchronous side-question HTTP and TUI callers SHALL retain their single-turn behavior.

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

### Requirement: Private authenticated streaming transport
The streaming side interface SHALL require the existing daemon authentication, validate session identity, request identity, text-only roles and bounded payloads, and deliver side output only to the requesting connection. Cancellation and disconnect SHALL release request resources without affecting a main task.

#### Scenario: Invalid or concurrent input
- **WHEN** a client sends invalid history or starts a second side request on the same active connection
- **THEN** the daemon returns a structured error without starting another provider call
