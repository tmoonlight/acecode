## Purpose

定义蜂群模式（网状）下每棵 agent 树同时驻留在内存中的 agent 数量上限、超限时的换出与透明恢复行为、agent 状态的统一枚举，以及 daemon 重启后树状态如何重建。

## ADDED Requirements

### Requirement: 每棵树的驻留上限只约束已加载的 agent
每个 root 会话树 SHALL 最多同时驻留 `swarm.mesh.max_concurrent_agents` 个已加载的 agent（含 root），默认 4，配置值 MUST 不小于 2。root MUST NOT 被换出。逻辑树的规模 MUST NOT 受该上限限制：已被换出的 agent 不占驻留槽，仍保留在树目录中。系统 MUST NOT 施加跨会话树的 daemon 全局上限。

#### Scenario: 超限时换出空闲 agent
- **WHEN** 上限为 4，root 与三个空闲子 agent 已驻留，root 调用 `agent_spawn`
- **THEN** 最久未活动的空闲子 agent 被换出
- **AND** 新 agent 创建成功，驻留数保持不超过 4

#### Scenario: 全部驻留 agent 都在运行
- **WHEN** 上限为 4，root 与三个子 agent 均在运行，root 调用 `agent_spawn`
- **THEN** 调用返回上限错误，说明当前没有可换出的空闲 agent，建议先等待
- **AND** 不创建新的会话

#### Scenario: 多棵树互不影响
- **WHEN** 同一 daemon 内两个不同 root 会话各自驻留 4 个 agent
- **THEN** 两棵树都不被拒绝
- **AND** 任一树的驻留数不影响另一树的上限判定

### Requirement: 换出只针对空闲 agent 且不丢失记录
换出 SHALL 只选择没有运行中回合、且邮箱中没有待触发后续任务的非 root agent，按最久未活动优先。换出 SHALL 先确保该 agent 的会话记录与身份已落盘，再卸载其运行实例；换出后该 agent 的状态 SHALL 为 `inactive`，其历史、路径与身份 MUST 保持可读。

#### Scenario: 换出后仍可查看
- **WHEN** `/root/a` 被换出
- **THEN** `agent_list` 仍列出 `/root/a` 且状态为 `inactive`
- **AND** 用户仍能在后台任务面板打开其只读对话记录

#### Scenario: 有待触发任务的 agent 不被换出
- **WHEN** `/root/a` 空闲但邮箱中有一条尚未开始处理的 `NEW_TASK`
- **THEN** 换出选择跳过 `/root/a`
- **AND** 若无其它可换出对象则返回上限错误

### Requirement: 被换出的 agent 在被寻址时透明恢复
对 `inactive` agent 的 `agent_send_message` 与 `agent_followup_task` SHALL 先为其取得驻留槽并从会话记录恢复运行实例，再执行投递；恢复后的 agent SHALL 保有原路径、历史、蜂群模式与专家身份。被中断后换出的 agent 同样 SHALL 可恢复。`agent_interrupt` 与 `agent_list` MUST NOT 为了执行而恢复 `inactive` agent。

#### Scenario: 后续任务恢复被换出的 agent
- **WHEN** `/root` 对 `inactive` 的 `/root/a` 调用 `agent_followup_task`
- **THEN** `/root/a` 被恢复并以该任务启动新回合
- **AND** 其上下文包含换出前的全部历史

#### Scenario: 恢复受上限约束
- **WHEN** 驻留已满且无可换出对象，`/root` 对 `inactive` 的 `/root/a` 调用 `agent_followup_task`
- **THEN** 调用返回上限错误
- **AND** `/root/a` 保持 `inactive`，任务不丢失地留在回执中要求稍后重试

### Requirement: 统一的 agent 状态枚举
所有报告 agent 状态的接口 SHALL 使用同一枚举：`pending_init`（已创建尚未开始首个回合）、`running`、`idle`（回合正常结束）、`interrupted`、`errored`、`inactive`（已换出或 daemon 重启后未加载）、`not_found`。`agent_list`、协作工具回执、会话状态接口与后台任务面板 MUST 使用相同取值。

#### Scenario: 状态一致
- **WHEN** `/root/a` 因用户中止而结束回合
- **THEN** `agent_list` 报告 `interrupted`
- **AND** 后台任务面板与会话状态接口报告同一取值

### Requirement: 驻留槽的并发占用不得超限
驻留槽 SHALL 在创建会话或启动恢复之前原子预留，成功后提交、失败时释放；并发的 `agent_spawn` 与恢复请求 MUST NOT 使已加载 agent 数超过上限。

#### Scenario: 争夺最后一个槽
- **WHEN** 两个子 agent 同时调用 `agent_spawn` 且只剩一个可用槽、无可换出对象
- **THEN** 恰好一个调用成功
- **AND** 另一个返回上限错误且不创建会话

### Requirement: daemon 重启后重建树状态
daemon 重启后 SHALL 从会话元数据重建每棵树的目录（路径、父子关系、蜂群模式），所有节点初始为 `inactive`；邮箱内容为运行时状态，重启后 MAY 丢失。`agent_followup_task` SHALL 能恢复重建的节点；`agent_list` 与 `agent_wait` MUST NOT 为此自动恢复任何节点。

#### Scenario: 重启后列出树
- **WHEN** daemon 重启后用户恢复 `mesh` 模式的 root 会话并调用 `agent_list`
- **THEN** 结果按原路径列出全部子 agent 且状态为 `inactive`
- **AND** 没有子 agent 因该调用被恢复
