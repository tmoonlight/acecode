## Purpose

定义用户如何为一个会话选择「蜂群模式（星型）」或「蜂群模式（网状）」，模式如何持久化、恢复并被子 agent 继承，以及星型与网状两套协作工具如何按模式互斥地暴露给模型。

## ADDED Requirements

### Requirement: 会话级蜂群模式三态且互斥
每个会话 SHALL 持有且只持有一个蜂群模式值：`off`、`star`（蜂群模式（星型））或 `mesh`（蜂群模式（网状））。选择其中一种 MUST 替换当前值；两种蜂群模式 MUST NOT 同时生效。

#### Scenario: 从星型切换到网状
- **WHEN** 会话当前模式为 `star`，用户选择「蜂群模式（网状）」
- **THEN** 会话模式变为 `mesh`
- **AND** 星型模式的引导与工具在后续回合中不再生效

#### Scenario: 关闭蜂群模式
- **WHEN** 用户移除已选中的蜂群模式标签或执行 `/swarm off`
- **THEN** 会话模式变为 `off`
- **AND** 后续回合不注入任何蜂群引导

### Requirement: 模式随会话持久化并可恢复
蜂群模式 SHALL 写入会话元数据；恢复会话 SHALL 还原该值；会话列表与状态接口 SHALL 暴露当前模式。缺失该字段的旧元数据 SHALL 读取为 `off`。

#### Scenario: daemon 重启后恢复网状会话
- **WHEN** 一个模式为 `mesh` 的会话在 daemon 重启后被恢复
- **THEN** 该会话的模式仍为 `mesh`
- **AND** 恢复后的第一个回合即可使用网状协作工具

#### Scenario: 读取旧元数据
- **WHEN** 加载一个没有蜂群模式字段的旧会话元数据
- **THEN** 会话模式读取为 `off`
- **AND** 序列化时 `off` 省略该字段

### Requirement: 子 agent 继承派生时的模式
网状模式派生的子 agent SHALL 继承 `mesh` 并获得同一套网状协作工具。星型模式派生的子代理 SHALL 保持现有行为：不注入蜂群引导，不能再派生子代理。

#### Scenario: 网状 root 派生子 agent
- **WHEN** 模式为 `mesh` 的会话调用 `agent_spawn`
- **THEN** 新子 agent 的模式为 `mesh`
- **AND** 子 agent 可以调用 `agent_spawn`、`agent_list`、`agent_send_message`、`agent_followup_task`、`agent_wait`、`agent_interrupt`

#### Scenario: 星型 root 派生子代理
- **WHEN** 模式为 `star` 的会话调用 `spawn_subagent`
- **THEN** 子代理的模式为 `off`
- **AND** 子代理调用 `spawn_subagent` 被以「子代理不能再派生」拒绝

### Requirement: 星型模式行为保持不变
模式为 `star` 的回合 SHALL 注入现有的主动委派引导（早期有界扇出、`wait=false` 配合 `wait_subagent`、不重叠的范围、主 agent 负责整合验证），协作工具 SHALL 仅为 `spawn_subagent` 与 `wait_subagent`，MUST NOT 暴露任何 `agent_*` 工具。除显示名称外，星型模式 MUST NOT 与本变更之前的「蜂群模式」有任何行为差异。

#### Scenario: 星型回合的工具与引导
- **WHEN** 模式为 `star` 的会话发起一个回合
- **THEN** 模型请求包含现有的 Swarm Mode 引导段
- **AND** 工具列表包含 `spawn_subagent` 与 `wait_subagent`，不包含任何 `agent_*` 工具

### Requirement: 网状回合只暴露网状工具
模式为 `mesh` 的回合 SHALL 暴露 `agent_spawn`、`agent_list`、`agent_send_message`、`agent_followup_task`、`agent_wait`、`agent_interrupt` 六个工具，MUST NOT 暴露 `spawn_subagent`、`wait_subagent` 以及全部 thread 工具（`create_thread`、`fork_thread`、`list_threads`、`read_thread`、`send_message_to_thread`、`wait_threads`、`set_thread_title`、`set_thread_pinned`、`set_thread_archived`、`delete_thread`、`repair_thread`）。非网状回合 MUST NOT 暴露六个 `agent_*` 工具。对当前模式下被隐藏工具的调用 SHALL 被拒绝，并在结果中说明该工具在当前蜂群模式下不可用。

#### Scenario: 网状回合的工具列表
- **WHEN** 模式为 `mesh` 的会话发起一个回合
- **THEN** 模型请求的工具列表包含六个 `agent_*` 工具
- **AND** 不包含 `spawn_subagent`、`wait_subagent` 与任何 thread 工具

#### Scenario: 网状回合调用星型工具
- **WHEN** 模型在 `mesh` 回合中调用 `spawn_subagent`
- **THEN** 调用被拒绝
- **AND** 结果说明该工具在蜂群模式（网状）下不可用，应改用 `agent_spawn`

#### Scenario: 非网状回合调用网状工具
- **WHEN** 模型在 `off` 或 `star` 回合中调用 `agent_send_message`
- **THEN** 调用被拒绝
- **AND** 结果说明该工具仅在蜂群模式（网状）下可用

### Requirement: 树内仍有工作时禁止关闭网状模式
当会话树内仍有运行中的子 agent，或仍有未投递的后续任务时，把模式从 `mesh` 改为其它值的请求 SHALL 被拒绝并说明原因；会话模式保持 `mesh`。Web 消息接口 SHALL 以 HTTP 409 拒绝，`/swarm` 命令 SHALL 以命令错误拒绝。

#### Scenario: 子 agent 运行中关闭网状
- **WHEN** 会话树内有一个子 agent 正在运行，用户提交把模式改为 `off` 或 `star` 的请求
- **THEN** 请求被拒绝并说明仍有子 agent 在运行
- **AND** 会话模式保持 `mesh`

#### Scenario: 子 agent 全部结束后关闭网状
- **WHEN** 会话树内没有运行中的子 agent 且没有待投递的后续任务，用户把模式改为 `off`
- **THEN** 请求被接受
- **AND** 会话模式变为 `off`

### Requirement: 各表面提供模式入口
Web/Desktop 输入框的 `+` 菜单 SHALL 提供「蜂群模式（星型）」与「蜂群模式（网状）」两个互斥单选项，选中后输入区 SHALL 显示对应的可移除标签，选择 SHALL 随下一条消息提交并成为会话模式。TUI SHALL 提供 `/swarm` 显示当前模式，`/swarm star|mesh|off` 切换模式，无效参数 SHALL 报错并列出合法值。headless 打印模式 SHALL 提供 `--swarm star|mesh`，缺省为 `off`，无效值 SHALL 以 usage 错误退出。

#### Scenario: 菜单单选
- **WHEN** 用户在 `+` 菜单中已选中「蜂群模式（星型）」，再点击「蜂群模式（网状）」
- **THEN** 菜单中只有「蜂群模式（网状）」处于选中态
- **AND** 输入区标签文案变为「蜂群模式（网状）」

#### Scenario: TUI 切换模式
- **WHEN** 用户在 TUI 输入 `/swarm mesh`
- **THEN** 会话模式变为 `mesh` 并回显当前模式
- **AND** 输入 `/swarm foo` 时报错并列出 `star`、`mesh`、`off`

#### Scenario: headless 指定模式
- **WHEN** 用户运行 `acecode -p --swarm mesh "<prompt>"`
- **THEN** 新会话以 `mesh` 模式运行
- **AND** 运行 `acecode -p --swarm foo "<prompt>"` 时以 usage 错误退出且不执行 prompt

### Requirement: 消息接口的 swarm_mode 契约
`POST /api/sessions/:id/messages` 的可选字段 `swarm_mode` SHALL 接受 `"star"`、`"mesh"`、`false`，并为兼容旧客户端接受 `true`（等价于 `"star"`）。其它取值 SHALL 以 HTTP 400 拒绝。接受后 SHALL 更新会话模式，并把生效的模式记录到该用户消息的元数据中。省略该字段的消息 SHALL 沿用会话当前模式。

#### Scenario: 旧客户端发送布尔 true
- **WHEN** 请求体包含 `"swarm_mode": true`
- **THEN** 会话模式变为 `star`
- **AND** 该用户消息的元数据记录 `swarm_mode` 为 `"star"`

#### Scenario: 非法取值
- **WHEN** 请求体包含 `"swarm_mode": "ring"`
- **THEN** 接口返回 HTTP 400
- **AND** 错误说明合法取值为 `"star"`、`"mesh"` 或 `false`

#### Scenario: 后续消息省略字段
- **WHEN** 会话已处于 `mesh`，下一条消息不包含 `swarm_mode`
- **THEN** 该回合仍以 `mesh` 模式运行
- **AND** 会话模式保持 `mesh`
