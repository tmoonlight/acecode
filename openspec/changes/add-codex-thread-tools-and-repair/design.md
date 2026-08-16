## Context

ACECode 已经具备多会话 `SessionRegistry`、本地 `SessionClient`、追加式 JSONL、会话 fork/archive/title、Web 端 pin 与 purge，以及 compact checkpoint 和 provider history recovery。缺口不是底层完全没有能力，而是这些能力没有形成模型可调用、名称与 Codex 对齐的 thread 工具；会话损坏与上下文溢出处理也分散在 resume、compact 和 Web 路由中。

当前普通模型请求遇到上下文溢出时会直接结束回合。现有 compact 依赖一次模型总结请求；当部署端实际限制低于本地配置、或固定上下文本身过大时，总结请求也可能被同一个限制拒绝。由于 provider 在模型输出前就拒绝请求，故障会话无法依靠模型在本会话中调用工具自救。

## Goals / Non-Goals

**Goals:**

- 提供一组一工具一动作、名称和核心字段尽量对齐当前 Codex App 的 thread 工具。
- 让 daemon、TUI 和 headless 的模型回合共享相同的 thread 工具后端。
- 提供不调用模型、不重放工具、只修改 provider 投影的确定性修复能力。
- 同一修复引擎同时服务 `repair_thread`、磁盘会话修复和普通请求的上下文溢出自动恢复。
- 自动恢复最多经历有限的单调阶段，并对是否能够安全重试作出明确判断。

**Non-Goals:**

- 不实现 Codex 的多主机 `hostId`、云端 target、handoff、页面导航或工作树调度协议。
- 不新增通用授权层、会话 ACL、模型预算、任意 JSONL 编辑器或批量清理工具。
- 不使用第二次模型调用生成“救援摘要”，也不保证在固定系统提示和当前输入本身已超过服务端硬限制时继续完成原任务。
- 不把历史工具调用重新执行，也不把缺失工具结果假定为成功。

## Decisions

### 1. 每个 Codex 动作使用独立工具

新增 `create_thread`、`fork_thread`、`list_threads`、`read_thread`、`send_message_to_thread`、`wait_threads`、`set_thread_title`、`set_thread_pinned`、`set_thread_archived`、`delete_thread` 和 `repair_thread`。参数优先沿用 Codex 的 camelCase 核心字段，例如 `threadId`、`turnLimit`、`includeOutputs`、`afterCursor` 和 `timeoutMs`。

所有工具共用一个会话域服务，并直接使用 `SessionRegistry`、`SessionClient`、`SessionManager` 和 `SessionStorage`；不经 ACECode 自己的 HTTP 接口绕行。只读工具标记为只读，其余沿用现有工具确认机制，不新增另一套权限协议。

替代方案是延续 `session_query(action=...)` 与 `session_control(action=...)`。该方案减少 schema 数量，但与用户要求和 Codex 的模型操作习惯不一致，也让单个工具承担过多无关动作，因此不采用。

### 2. 以调用会话的工作区作为自然作用域

ACECode 的会话文件按工作区项目目录存储，thread 服务据调用会话的 `cwd` 合并磁盘会话与当前进程内的活跃会话。TUI 主会话不在子会话 registry 中，因此服务同时接受调用方 `SessionManager`，以正确支持省略 `threadId` 的 rename/archive/fork 等当前会话动作。

`hostId`、Codex project target 和跨主机语义不出现在 ACECode schema 中。`create_thread` 在当前工作区创建并发送首条 `prompt`；`fork_thread` 默认 fork 调用线程，复制已完成的持久化历史；`send_message_to_thread` 把 prompt 排入目标会话。

### 3. 读取与等待返回有界、可续读结果

`list_threads` 返回全部 pinned thread（保持 UI pin 顺序）以及受 `limit` 限制的非 pinned thread。`read_thread` 将可见记录组织为最近 turn，并使用不透明 cursor 读取更旧 turn；工具输出只有在 `includeOutputs=true` 时返回，且每项受 `maxOutputCharsPerItem` 限制。

`wait_threads` 一次订阅至多八个目标。它只在目标完成、出错、等待权限/回答或用户中止工具时提前返回；普通 token/commentary/message 事件只更新快照，不唤醒等待。`timeoutMs=0` 返回即时快照。

### 4. 删除复用现有硬删除能力

`delete_thread` 中止并移除活跃目标，然后调用 `SessionStorage::purge_session_files` 删除 JSONL、meta 和会话附件目录，同时清理 pin 和用户消息搜索索引。若目标存在持久化子会话，按子到父顺序一并删除，与 Codex App Server 的 thread delete 行为保持一致。

工具执行期间不允许删除调用线程本身，因为其工具结果和回合收尾仍需要该 writer/loop；调用方可从另一个健康线程删除它。这个约束来自运行时生命周期，而不是额外授权模型。

### 5. 修复引擎只生成新的 provider 投影

修复引擎先读取最新有效 compact checkpoint 及其后缀，再调用现有 `recover_provider_history`：畸形或重复 tool call/result 从 provider 投影移除，缺失 tool result 被补成 `outcome=unknown`，任何工具都不会重新执行。

若需要减小上下文，消息按真实 user turn 划分为完整逻辑组。引擎从最旧组开始整体移除，保留最后一个真实 user turn（即当前输入所在组），直到达到目标或已无可删旧组。完成后追加一个 `trigger=repair-*` 的 compact checkpoint；原 JSONL 的用户可见行不改写、不删除。活跃会话同时在其 worker 边界替换内存中的 provider history，非活跃会话直接对 canonical JSONL 追加 checkpoint。

### 6. 自动恢复使用有限状态机

每个普通用户回合维护以下单调状态：

1. `normal`：首次明确上下文溢出且没有任何 assistant 正文、reasoning 或 tool call 时，执行确定性历史修复；若没有旧组可删，直接进入下一阶段。
2. `history_repaired`：修复后的请求仍被明确拒绝时，启用 emergency request profile。
3. `emergency_profile`：保留基础系统指令、修复后的历史与当前 user turn，省略 skills/memory/git/todo/hook 等可选请求局部上下文，并只保留核心文件/命令工具 schema。该请求仍失败即返回不可恢复错误，不再自动重试。

若 provider 已产生任何 assistant 输出或 tool call，修复可以记录诊断，但当前回合 MUST NOT 自动重放。429、5xx、超时和一般网络错误不进入该状态机，继续走既有 provider retry/错误路径。

### 7. 修复结果可诊断但不暴露原始编辑能力

`repair_thread` 返回 `status`、发现的问题计数、裁剪的逻辑组/消息数、修复前后估算 token、checkpoint id 和不可恢复原因。参数只有目标 `threadId`；工具不接受替换消息、JSONL 路径或任意 JSON patch。

## Risks / Trade-offs

- [确定性裁剪会丢失旧语义上下文] → 原始 transcript 始终保留，结果记录被裁剪组数，用户仍可 fork/读取原历史；只有 provider 投影改变。
- [服务端真实限制可能远低于配置] → 每次 repair 至少移除一个旧逻辑组，随后还有一次更小的 emergency profile；仍失败则明确停止，避免无限循环。
- [活跃会话与磁盘写入竞争] → 活跃目标的修复、标题与归档变更通过目标 AgentLoop control 边界执行；非活跃目标才直接写磁盘。
- [工具 schema 数量增加固定上下文] → emergency profile 会过滤非核心 schema；常态下保持独立工具以换取 Codex 对齐和可发现性。
- [TUI 主会话不在多会话 registry] → 从 `ToolContext` 传入调用方 `SessionManager` 作为显式兜底，不创建第二个同 id writer。

## Migration Plan

1. 先抽取 Web pin 持久化为 session 域共用 helper，保持现有文件格式不变。
2. 加入 thread 服务与工具，并在 daemon、TUI、headless 运行时注册。
3. 加入 repair 引擎和 active/offline 适配；旧 JSONL 无需迁移，旧版本会把新 checkpoint 当作已有 compact meta 读取。
4. 最后接入 AgentLoop 自动恢复状态机和 emergency profile。

回滚时可移除工具注册和自动恢复调用；已经追加的 checkpoint 仍符合现有 compact checkpoint 格式，不需要反向迁移。

## Open Questions

无。本次不实现多主机、handoff 或专门的会话管理 UI；后续若 ACECode 增加这些宿主能力，再扩展相应 Codex 字段。
