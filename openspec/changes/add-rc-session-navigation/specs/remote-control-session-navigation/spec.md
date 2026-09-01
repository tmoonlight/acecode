## ADDED Requirements

### Requirement: RC session command aliases

An active remote-control binding SHALL recognize `/session`, `/sessions`, and `/resume` as case-insensitive aliases and SHALL consume those commands in the control plane instead of submitting them to the bound agent conversation.

#### Scenario: Bare alias lists recent sessions

- **WHEN** an RC user sends any bare alias
- **THEN** the system returns the ten most recently updated ordinary, unarchived sessions across persisted workspaces and no-workspace conversations
- **AND** each returned session has a one-based number

#### Scenario: All aliases share behavior

- **WHEN** equivalent arguments are sent with `/session`, `/sessions`, or `/resume`
- **THEN** parsing, results, numbering, errors, and selection behavior are identical

### Requirement: Complete and searched session lists

The RC session command SHALL support complete listing and bounded search.

#### Scenario: Request all sessions

- **WHEN** the user sends an alias followed by `more` or `all`
- **THEN** every resumable ordinary, unarchived user session is listed with continuous one-based numbering

#### Scenario: Search sessions

- **WHEN** the user sends an alias followed by `search <query>`
- **THEN** at most five sessions matching metadata or indexed visible user-message content are returned
- **AND** the result order is deterministic

#### Scenario: Empty search query

- **WHEN** the user sends an alias followed only by `search`
- **THEN** compact usage guidance is returned
- **AND** the agent conversation and current binding are unchanged

### Requirement: Stable numbered selection

Numeric selection SHALL resolve against the most recently displayed result snapshot shared by all aliases.

#### Scenario: Select a displayed session

- **GIVEN** a list or search displayed a session as number 3
- **WHEN** the user sends any alias followed by `3`
- **THEN** that exact displayed session is resumed if needed and becomes the sole remote-control binding

#### Scenario: Select before listing

- **WHEN** a positive number is sent before any list snapshot exists in the current daemon lifetime
- **THEN** the system first builds the default newest-ten snapshot and resolves the number against it

#### Scenario: Invalid number

- **WHEN** the number is zero, malformed, or outside the current snapshot
- **THEN** the system returns a clear error
- **AND** the current binding remains unchanged

### Requirement: 命令首响与切换结果

每条被当前 RC 绑定接受的会话命令 SHALL 在耗时工作开始前复用 Hub 已排队的
`思考中...` 首响，并在命令仍属于当前绑定时返回明确的最终结果。

#### Scenario: 列表或搜索需要较长时间

- **WHEN** 会话目录扫描或消息索引刷新尚未完成
- **THEN** 用户先收到 `思考中...`
- **AND** 扫描结果随后作为同一命令的最终响应返回

#### Scenario: 数字切换成功

- **WHEN** 用户选择的目标会话成功恢复并完成绑定替换
- **THEN** 用户只收到一条切换成功结果，不额外收到通用的新连接成功消息
- **AND** 结果包含目标会话标题，标题为空时回退 session id
- **AND** 结果按 `最近一次 prompt_tokens / 当前有效 context_window` 展示上下文

#### Scenario: 上下文统计不完整

- **WHEN** 目标会话没有最近一次 provider 用量
- **THEN** 上下文行明确显示“暂无用量”，而不是伪造 0 用量
- **AND** 当有效上下文窗口也不可用时明确显示“暂不可用”

### Requirement: Cross-workspace resume and switching

Selection SHALL use persisted target metadata to resume inactive workspace and no-workspace sessions before replacing the current binding.

#### Scenario: Inactive workspace target

- **GIVEN** the selected session is persisted under a different workspace and is not active
- **WHEN** selection runs
- **THEN** it is resumed with its own cwd and workspace hash
- **AND** binding replacement occurs only after resume succeeds

#### Scenario: Replacement fails

- **WHEN** target resume or channel activation fails
- **THEN** the user receives an error
- **AND** the previous usable binding remains authoritative whenever replacement has not committed

### Requirement: Optional frontend follow navigation

A successful numeric selection SHALL broadcast a secret-free generic navigation hint to connected Web/Desktop clients.

#### Scenario: Frontend is open

- **WHEN** a numeric RC selection succeeds while a frontend is connected
- **THEN** the frontend opens the selected conversation
- **AND** the target sidebar row visibly runs the existing remote-control lightning surge
- **AND** the persistent bound background moves to the target row

#### Scenario: Frontend is closed

- **WHEN** no frontend is connected
- **THEN** selection and remote-control message routing still succeed

### Requirement: Selection is lifecycle safe

RC session commands SHALL preserve binder generation filtering, inbound callback lifetime, and shutdown safety.

#### Scenario: Selection originates in the old inbound callback

- **WHEN** a numeric command is accepted through the currently bound route
- **THEN** replacement does not wait on its own still-held binding-context lease
- **AND** stale callbacks cannot access a destroyed binder or forward into the replacement session

#### Scenario: Shutdown races a command

- **WHEN** daemon shutdown overlaps listing or selection
- **THEN** owned work is cancelled or joined deterministically
- **AND** no callback accesses destroyed binder, session, hub, or WebServer state

#### Scenario: Catalog or search is slow

- **WHEN** a session command requires an all-project scan or message-index refresh that stalls
- **THEN** the RC HTTP inbound callback returns promptly after queueing the control operation
- **AND** the existing immediate acknowledgement is not delayed by catalog or search work
