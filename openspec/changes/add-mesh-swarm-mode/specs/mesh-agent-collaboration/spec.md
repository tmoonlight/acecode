## Purpose

定义蜂群模式（网状）下 agent 的身份与路径寻址、六个协作工具的输入、输出与错误条件、邮箱投递语义（普通消息、后续任务、最终答复）、上下文继承、专家团映射以及注入给模型的角色与模式提示。

## ADDED Requirements

### Requirement: agent 拥有 canonical 路径并支持相对寻址
根会话的路径 SHALL 为 `/root`。`agent_spawn` 创建的子 agent 路径 SHALL 为调用者路径加 `/` 加 `task_name`，可无限嵌套。`task_name` MUST 匹配 `[a-z0-9_]+`，MUST NOT 为 `root`、`.` 或 `..`；同一父 agent 下重复的 `task_name` SHALL 返回错误，MUST NOT 静默改名。所有接受目标的工具参数 SHALL 接受 canonical 路径、相对调用者路径的相对名或底层会话 id；相对名 SHALL 相对调用者自身路径解析。星型模式的子会话（只有 `parent_session_id`、没有路径）MUST NOT 被并入任何网状树。

#### Scenario: 嵌套路径
- **WHEN** `/root/task1` 以 `task_name="task_3"` 调用 `agent_spawn`
- **THEN** 新 agent 的 canonical 路径为 `/root/task1/task_3`
- **AND** `/root/task1` 可以用 `task_3` 或 `/root/task1/task_3` 互换地引用它

#### Scenario: 兄弟子树间寻址
- **WHEN** `/root/task2/task_3` 要给 `/root/task1/task_3` 发消息
- **THEN** 它 MUST 使用 canonical 路径 `/root/task1/task_3`
- **AND** 使用相对名 `task_3` 会解析到 `/root/task2/task_3/task_3` 而找不到目标

#### Scenario: 同父重名
- **WHEN** `/root` 下已存在 `review`，`/root` 再次以 `task_name="review"` 调用 `agent_spawn`
- **THEN** 调用返回名称冲突错误
- **AND** 不创建新的会话

#### Scenario: 非法任务名
- **WHEN** `agent_spawn` 收到 `task_name="Review-Tests"` 或 `task_name="root"`
- **THEN** 调用返回参数错误并说明合法字符集
- **AND** 不创建新的会话

### Requirement: agent_spawn 异步创建同能力的子 agent
`agent_spawn(task_name, message, agent_type?, fork_turns?, model?, reasoning_effort?)` SHALL 创建一个独立的子会话，把 `message` 以 `NEW_TASK` 信封作为首条任务投递，并在提交成功后立即返回子 agent 的 canonical 路径、会话 id 与初始状态；MUST NOT 阻塞等待子 agent 完成。子 agent SHALL 继承调用者的工作目录、权限模式、蜂群模式与专家绑定，SHALL 拥有与调用者相同的协作工具，并且 SHALL 可以继续派生自己的子 agent；派生深度 MUST NOT 被固定上限拒绝，只受驻留上限约束。`model` SHALL 接受 saved_models 中的名称，未知名称 SHALL 返回错误；`reasoning_effort` 仅在所选模型档案支持推理力度设置时生效，否则 SHALL 在结果中注明被忽略。

#### Scenario: 派生立即返回
- **WHEN** `/root` 调用 `agent_spawn(task_name="explore", message="...")`
- **THEN** 工具在子会话创建并接收首条任务后立即返回路径 `/root/explore`、会话 id 与状态 `running` 或 `pending_init`
- **AND** `/root` 的回合继续执行，不等待 `/root/explore` 完成

#### Scenario: 二级派生
- **WHEN** `/root/explore` 调用 `agent_spawn(task_name="tests", message="...")`
- **THEN** 创建路径为 `/root/explore/tests` 的子 agent
- **AND** 它拥有与 `/root/explore` 相同的六个协作工具

#### Scenario: 未知模型名
- **WHEN** `agent_spawn` 的 `model` 不在 saved_models 中
- **THEN** 调用返回错误并列出问题
- **AND** 不创建新的会话

### Requirement: fork_turns 决定继承多少父上下文
`fork_turns` SHALL 接受 `none`、`all`（默认）或正整数字符串。`none` 时子 agent 从空对话开始；`all` 时子 agent 继承父会话到派生时刻的全部用户消息与 assistant 最终回复；正整数 N 时只继承最近 N 个用户回合。任何取值下 MUST NOT 继承工具调用与工具结果、推理内容、压缩记录以及跨 agent 信封。非法取值 SHALL 返回参数错误。

#### Scenario: 默认继承全部
- **WHEN** 父会话已有 3 个用户回合且含多次工具往返，`agent_spawn` 未提供 `fork_turns`
- **THEN** 子 agent 的初始上下文包含这 3 条用户消息与对应的 assistant 最终回复
- **AND** 不包含任何工具调用、工具结果或推理内容

#### Scenario: 只继承最近两轮
- **WHEN** `agent_spawn` 提供 `fork_turns="2"`
- **THEN** 子 agent 的初始上下文只包含最近 2 个用户回合
- **AND** 更早的回合不出现在子 agent 上下文中

#### Scenario: 不继承
- **WHEN** `agent_spawn` 提供 `fork_turns="none"`
- **THEN** 子 agent 的上下文只有系统提示与首条 `NEW_TASK` 信封

### Requirement: agent_list 列出整棵树
`agent_list(path_prefix?)` SHALL 返回调用者所属 root 树内全部已知 agent（含 root 与已被换出的 agent）的 canonical 路径、会话 id、状态、模型、任务名或标题以及最近更新时间，按路径排序；提供 `path_prefix` 时只返回路径以其开头的 agent。结果 MUST NOT 包含任何 agent 的对话内容。

#### Scenario: 兄弟发现彼此
- **WHEN** `/root/a` 调用 `agent_list()`
- **THEN** 结果包含 `/root`、`/root/a` 与 `/root/b` 及各自状态
- **AND** 不包含任何对话内容

#### Scenario: 前缀过滤
- **WHEN** `/root` 调用 `agent_list(path_prefix="/root/a")`
- **THEN** 结果只包含 `/root/a` 及其后代

### Requirement: agent_send_message 只入箱不唤醒
`agent_send_message(target, message)` SHALL 把 `MESSAGE` 信封写入目标邮箱并返回投递回执；目标可以是树内任意 agent 包括 root。目标运行中时，信封 SHALL 在目标下一次模型请求之前进入其上下文；目标空闲时，信封 SHALL 保持待投递直到目标下一次回合开始，MUST NOT 为此启动新回合。目标已被换出时 SHALL 先恢复目标再入箱。

#### Scenario: 发给运行中的兄弟
- **WHEN** `/root/a` 对运行中的 `/root/b` 调用 `agent_send_message`
- **THEN** `/root/b` 在下一次模型请求前看到该 `MESSAGE` 信封
- **AND** `/root/b` 没有被启动第二个并发回合

#### Scenario: 发给空闲的 root
- **WHEN** `/root/a` 对空闲的 `/root` 调用 `agent_send_message`
- **THEN** `/root` 保持空闲
- **AND** 用户下一次向 `/root` 发送消息时，该信封出现在 `/root` 的上下文中

#### Scenario: 发给已被换出的 agent
- **WHEN** 目标 agent 已被换出（未加载）
- **THEN** 系统先恢复该 agent，再把信封写入其邮箱
- **AND** 回执与普通投递相同（空输出）

### Requirement: agent_followup_task 恰好一次地触发目标
`agent_followup_task(target, message)` SHALL 把 `NEW_TASK` 信封写入目标邮箱并保证目标处理它：目标空闲、已完成或已被换出时 SHALL（必要时恢复后）启动一个新回合；目标运行中时 SHALL 在当前回合的下一处消息边界投递，MUST NOT 并发启动第二个回合。目标从运行到空闲的竞态窗口内，同一条后续任务 MUST 恰好被投递一次，不得丢失或重复。目标 MUST NOT 为 root，否则 SHALL 返回错误 "Follow-up tasks can't target the root agent"。

#### Scenario: 对空闲子 agent 派后续任务
- **WHEN** `/root` 对已完成的 `/root/a` 调用 `agent_followup_task`
- **THEN** `/root/a` 启动一个新回合，首条输入为该 `NEW_TASK` 信封
- **AND** `/root/a` 的路径与既有上下文保持不变

#### Scenario: 对运行中的兄弟派后续任务
- **WHEN** `/root/a` 对运行中的 `/root/b` 调用 `agent_followup_task`
- **THEN** 信封在 `/root/b` 当前回合的下一处消息边界进入其上下文
- **AND** 不启动第二个并发回合

#### Scenario: 目标恰在完成时收到后续任务
- **WHEN** 后续任务入箱与目标回合结束同时发生
- **THEN** 该信封要么被当前回合在边界处消费，要么成为新回合的首条输入
- **AND** 两者恰好发生一个

#### Scenario: 对 root 派后续任务
- **WHEN** `/root/a` 对 `/root` 调用 `agent_followup_task`
- **THEN** 调用返回错误 "Follow-up tasks can't target the root agent"
- **AND** `/root` 的邮箱与状态不变

### Requirement: 完成结果投递给父 agent 且不唤醒空闲父
子 agent 回合结束时，系统 SHALL 向其父 agent 邮箱投递一条 `FINAL_ANSWER` 信封（与 Codex 一致）：正常结束时 Payload 为该回合的最终 assistant 回复（没有则为空）；因错误结束时 Payload 为 `Agent errored: <错误>` 加上「可用协作工具给它派新任务」的下一步提示；被中断的回合 MUST NOT 投递。该投递 MUST NOT 唤醒空闲的父；父正在 `agent_wait` 中 SHALL 被唤醒；父正在运行中 SHALL 在下一处消息边界收到；父未加载时 SHALL 暂存到它被恢复后再投递。

#### Scenario: 父正在等待
- **WHEN** `/root` 阻塞在 `agent_wait`，`/root/a` 完成回合
- **THEN** `/root` 的 `agent_wait` 返回 "Wait completed."
- **AND** `/root` 的下一次模型请求包含来自 `/root/a` 的 `FINAL_ANSWER` 信封

#### Scenario: 父已空闲
- **WHEN** `/root` 已结束回合并空闲，`/root/a` 完成回合
- **THEN** `/root` 保持空闲，不自动开始新回合
- **AND** `FINAL_ANSWER` 信封留在 `/root` 邮箱等待其下一回合

#### Scenario: 子 agent 出错结束
- **WHEN** `/root/a` 因 provider 错误结束回合
- **THEN** `/root` 收到的 `FINAL_ANSWER` 信封说明 `/root/a` 以错误结束及错误类别
- **AND** `/root/a` 仍可被寻址与派后续任务

### Requirement: agent_wait 等待自身邮箱
`agent_wait(timeout_ms?)` SHALL 阻塞直到调用者自身邮箱出现新活动、用户向调用者提交新输入或超时，并只返回 "Wait completed."、"Wait interrupted by new input." 或 "Wait timed out." 之一；MUST NOT 返回消息正文（正文以信封形式出现在调用者的下一次模型请求中）。`timeout_ms` 低于最短 10 秒时 SHALL 按最短值等待并在结果中注明被夹取，超过最长 1 小时 SHALL 返回错误 "timeout_ms must be at most <最大值>"，缺省 30 秒；三者 SHALL 可通过配置调整。调用者被用户中止时等待 SHALL 立即结束，且 MUST NOT 影响任何其它 agent。

#### Scenario: 邮箱活动唤醒
- **WHEN** `/root` 调用 `agent_wait(timeout_ms=600000)`，30 秒后 `/root/a` 发来消息
- **THEN** `agent_wait` 在消息到达时返回 "Wait completed."
- **AND** 该消息在 `/root` 的下一次模型请求中以信封出现

#### Scenario: 用户新输入打断等待
- **WHEN** `/root` 在 `agent_wait` 中，用户向 `/root` 的当前回合追加引导输入
- **THEN** `agent_wait` 返回 "Wait interrupted by new input."

#### Scenario: 超时钳制
- **WHEN** `agent_wait(timeout_ms=1000)` 被调用
- **THEN** 实际等待下限为 10 秒
- **AND** 超时后返回 "Wait timed out." 并注明请求值被夹取到最小值

#### Scenario: 调用者被中止
- **WHEN** 用户停止正在 `agent_wait` 的 `/root`
- **THEN** 等待立即结束
- **AND** 所有子 agent 继续运行不受影响

### Requirement: agent_interrupt 中止目标当前回合但保留目标
`agent_interrupt(target)` SHALL 中止目标 agent 当前回合（若有）并返回中止前的状态；目标 SHALL 保持可寻址、可接收消息与后续任务。目标为 root 或调用者自身时 SHALL 返回错误。目标空闲时 SHALL 返回其状态而不报错。

#### Scenario: 中止运行中的子 agent
- **WHEN** `/root` 对运行中的 `/root/a` 调用 `agent_interrupt`
- **THEN** `/root/a` 的当前回合停止，返回值报告中止前状态为 `running`
- **AND** `/root/a` 随后仍能接收 `agent_followup_task`

#### Scenario: 中止空闲的 agent
- **WHEN** 目标没有正在运行的回合
- **THEN** 调用成功返回目标当前状态
- **AND** 目标状态不变

#### Scenario: 中止 root 或自身
- **WHEN** `/root/a` 对 `/root` 或对 `/root/a` 自身调用 `agent_interrupt`
- **THEN** 调用返回错误说明不能中断 root 或自身

### Requirement: 跨 agent 消息使用统一信封并以 user 角色进入上下文
跨 agent 消息 SHALL 以 `user` 角色消息进入收件方上下文，正文为 `<inter_agent_message>` 包裹的四个字段：`Message Type`（`MESSAGE`、`NEW_TASK` 或 `FINAL_ANSWER`）、`Task name`（收件人 canonical 路径）、`Sender`（发件人 canonical 路径）与 `Payload`。该消息 SHALL 带有可被界面识别的跨 agent 元数据并持久化到收件方会话记录中；它 MUST NOT 计入可见用户回合数，MUST NOT 触发用户提示提交类钩子。

#### Scenario: 信封内容
- **WHEN** `/root` 对 `/root/a` 调用 `agent_send_message(message="请先跑测试")`
- **THEN** `/root/a` 上下文中出现一条 user 角色消息，正文包含 `Message Type: MESSAGE`、`Task name: /root/a`、`Sender: /root` 与 `Payload:` 后的 "请先跑测试"
- **AND** 该消息的元数据标识它为跨 agent 消息

#### Scenario: 信封不计入用户回合
- **WHEN** 一个子 agent 只收到过 `NEW_TASK` 与 `MESSAGE` 信封
- **THEN** 其会话元数据的可见用户回合数为 0
- **AND** 用户提示提交钩子未被触发

### Requirement: 网状模式注入角色与主动委派提示
模式为 `mesh` 的回合 SHALL 向模型注入：root 的角色提示（声明自身为 `/root`、团队成员同等能力、六个工具的用途、`fork_turns` 的选择、当前并发槽数）或子 agent 的角色提示（自身路径、最终回复自动回父、信封三种类型的含义），以及适用于任何层级的主动委派模式文案。这些内容 MUST 位于随请求变化的上下文段而不是静态系统提示前缀，且在同一回合内逐次模型请求 MUST 逐字节相同。

#### Scenario: root 的提示
- **WHEN** `mesh` 模式的 root 发起回合
- **THEN** 模型请求包含以 `/root` 身份撰写的角色提示与主动委派文案
- **AND** 静态系统提示前缀与 `off` 模式下逐字节相同

#### Scenario: 子 agent 的提示
- **WHEN** `/root/a` 发起回合
- **THEN** 模型请求包含声明其身份为 `/root/a`、说明最终回复将回送父 agent 的角色提示
- **AND** 同一回合内两次相邻模型请求的该提示逐字节相同

### Requirement: agent_type 映射到专家团成员
当会话绑定专家团时，`agent_spawn` 的 `agent_type` SHALL 接受该团已选成员的 id，子 agent SHALL 以该成员身份运行（成员指令、技能与能力范围）；网状模式下树内任意层级的 agent 均可指定 `agent_type`。无效成员 id SHALL 返回错误并列出可选成员；会话未绑定专家团而提供 `agent_type` SHALL 返回错误。

#### Scenario: 子 agent 指定成员
- **WHEN** 绑定专家团的 `/root/plan` 调用 `agent_spawn(task_name="impl", agent_type="backend_dev", ...)`
- **THEN** `/root/plan/impl` 以成员 `backend_dev` 的身份运行
- **AND** 其上下文包含该成员的指令

#### Scenario: 无效成员
- **WHEN** `agent_type` 不是团内已选成员
- **THEN** 调用返回错误并列出可选成员 id
- **AND** 不创建新的会话
