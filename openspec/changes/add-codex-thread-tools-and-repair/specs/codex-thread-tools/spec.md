## ADDED Requirements

### Requirement: 暴露 Codex 对齐的 thread 工具
系统 SHALL 分别暴露 `create_thread`、`fork_thread`、`list_threads`、`read_thread`、`send_message_to_thread`、`wait_threads`、`set_thread_title`、`set_thread_pinned`、`set_thread_archived` 和 `delete_thread`，并在 ACECode 有对应语义时使用 Codex 的核心 camelCase 字段名。

#### Scenario: 模型发现会话工具
- **WHEN** daemon、TUI 或 headless 构建普通模型请求的工具列表
- **THEN** 上述工具以独立工具名出现，而不是被包装为通用 `action` 工具

#### Scenario: 不伪造宿主能力
- **WHEN** ACECode 不支持 Codex 的跨主机或云端 target 语义
- **THEN** 工具 schema MUST NOT 声称支持 `hostId`、handoff 或云端 target

### Requirement: 创建、续聊与 fork thread
`create_thread` SHALL 在调用会话的工作区创建 thread、可应用 title/model，并把必需的 `prompt` 作为首条输入排队；`send_message_to_thread` SHALL 把 `prompt` 排入指定 `threadId`；`fork_thread` SHALL 复制源 thread 已完成的持久化历史，且省略 `threadId` 时使用调用 thread。

#### Scenario: 创建 thread 并开始首轮
- **WHEN** 模型调用 `create_thread` 并提供非空 `prompt`
- **THEN** 系统返回新 `threadId`，持久化新会话并开始处理该 prompt

#### Scenario: 向磁盘 thread 续聊
- **WHEN** `send_message_to_thread` 的目标存在于当前工作区但尚未激活
- **THEN** 系统先恢复该 thread，再把 prompt 排入其 AgentLoop

#### Scenario: fork 当前 thread
- **WHEN** 模型调用 `fork_thread` 且省略 `threadId`
- **THEN** 系统从调用 thread 的已持久化历史创建新 `threadId`，不改写源 thread

### Requirement: 有界列举与读取 thread
`list_threads` SHALL 返回 pin 顺序中的全部 pinned thread，以及按最近更新时间排序且受 `limit` 限制的非 pinned thread。`read_thread` SHALL 返回有界的最近 turn，并支持 cursor 读取更旧 turn；只有 `includeOutputs=true` 时才返回工具输出。

#### Scenario: pinned thread 不受普通 limit 截断
- **WHEN** pinned thread 数量超过 `list_threads.limit`
- **THEN** `pinnedThreads` 仍包含全部 pinned thread，`threads` 单独受 limit 限制

#### Scenario: 分页读取旧 turn
- **WHEN** `read_thread` 的结果仍有更旧 turn
- **THEN** 响应包含可传回下一次调用的 `nextCursor`

#### Scenario: 限制每项输出
- **WHEN** `includeOutputs=true` 且某条消息或工具结果超过 `maxOutputCharsPerItem`
- **THEN** 系统截断该项并明确标记 truncated，不返回无界 transcript

### Requirement: 等待多个 thread 的关键状态
`wait_threads` SHALL 接受一至八个 `{threadId, afterCursor?}` 目标和可选 `timeoutMs`，在第一个目标完成、出错或需要用户处理时返回；普通增量文本 MUST NOT 单独唤醒等待。

#### Scenario: 即时快照
- **WHEN** `timeoutMs` 为 0
- **THEN** 系统不阻塞并返回每个有效目标的当前状态和新 cursor

#### Scenario: 一个目标完成
- **WHEN** 多个目标中任一目标发出完成或 idle 终态
- **THEN** 等待立即返回该变化，并为所有目标附上紧凑状态

#### Scenario: 等待被工具中止
- **WHEN** 调用会话的 abort flag 在等待期间被设置
- **THEN** `wait_threads` 停止订阅并返回 aborted 状态

### Requirement: 修改 thread 元数据
`set_thread_title`、`set_thread_pinned` 和 `set_thread_archived` SHALL 更新目标 thread 的持久化状态；允许 Codex 省略 `threadId` 的工具 SHALL 在省略时作用于调用 thread。

#### Scenario: 重命名当前 thread
- **WHEN** `set_thread_title` 省略 `threadId` 并提供有效 title
- **THEN** 调用 thread 的标题立即持久化并可由列表读取

#### Scenario: 归档 thread
- **WHEN** `set_thread_archived` 将 `archived` 设为 true
- **THEN** thread 从普通列表移除，并同步取消 pin

### Requirement: 硬删除 thread
`delete_thread` SHALL 永久删除目标 thread 的 canonical JSONL、meta、附件/工具结果目录、pin 状态和用户消息搜索索引，并 SHALL 先删除其持久化子 thread。

#### Scenario: 删除非活跃 thread
- **WHEN** 目标 thread 存在且不是调用 thread
- **THEN** 系统按子到父顺序删除全部相关磁盘状态，并返回 `deletedThreadIds`

#### Scenario: 删除活跃目标
- **WHEN** 目标 thread 在 registry 中活跃
- **THEN** 系统先中止并销毁目标 AgentLoop，再执行硬删除

#### Scenario: 拒绝在工具回合内自删
- **WHEN** `delete_thread.threadId` 等于调用 thread
- **THEN** 工具失败并说明必须由另一个 thread 删除，当前回合和 writer 保持有效
