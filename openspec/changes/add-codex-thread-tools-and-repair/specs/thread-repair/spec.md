## ADDED Requirements

### Requirement: 确定性恢复 provider history
系统 SHALL 使用最新有效 compact checkpoint 及其后缀重建 provider history，并 MUST 通过现有 history recovery 修复畸形、重复、孤立或缺失的工具调用/结果；修复过程 MUST NOT 调用模型或重新执行工具。

#### Scenario: 缺少 tool result
- **WHEN** 有效历史包含 tool call 但没有匹配 result
- **THEN** 修复后的 provider 投影包含 `outcome=unknown` 占位结果，且原工具不被重放

#### Scenario: JSONL 尾部损坏
- **WHEN** loader 发现可忽略的 partial tail 或畸形完整记录
- **THEN** 后续有效记录仍参与修复，结果报告对应诊断计数

### Requirement: 追加式 repair checkpoint
每次产生有效修复时，系统 SHALL 追加新的 compact checkpoint 来表达 replacement history，MUST NOT 改写或删除原始用户可见 transcript 行。

#### Scenario: 修复非活跃 thread
- **WHEN** `repair_thread` 指向磁盘上的非活跃 thread
- **THEN** 系统向 canonical JSONL 追加 `trigger=repair-manual` checkpoint，原文件已有记录保持原顺序和内容

#### Scenario: 修复活跃 thread
- **WHEN** 目标 thread 正在 registry 中且可到达 worker control 边界
- **THEN** 系统在该边界追加 checkpoint，并把 AgentLoop 内存 provider history 切换到同一 replacement history

### Requirement: 按完整逻辑组裁剪旧上下文
当修复需要降低 token 时，系统 SHALL 从最旧的完整真实 user turn 组开始裁剪，MUST 保留最后一个真实 user turn 及其当前输入，并 MUST 保持保留区内工具调用与结果配对有效。

#### Scenario: 有多个旧 turn
- **WHEN** 历史超过目标且存在可删除的旧 turn 组
- **THEN** 系统至少删除一个最旧完整组，并继续删除直到达到目标或只剩最后组

#### Scenario: 只有当前 turn 仍超限
- **WHEN** 除最后真实 user turn 外已无可删除组
- **THEN** 修复返回 history exhausted，而不是截断当前用户输入

### Requirement: 手动 repair_thread
系统 SHALL 暴露 `repair_thread({threadId})`，由服务自行诊断并返回 status、问题计数、裁剪组/消息数、修复前后估算 token、checkpoint id 和不可恢复原因；工具 MUST NOT 接受原始路径、替换消息或任意 JSON patch。

#### Scenario: 健康 thread 修复损坏目标
- **WHEN** 一个健康会话调用 `repair_thread` 指向另一个可访问 thread
- **THEN** 修复不依赖目标 thread 再次获得模型输出即可完成

#### Scenario: 无需修改
- **WHEN** history recovery 未发现问题且不存在可裁剪旧组
- **THEN** 工具返回 `noChange`，不追加空 checkpoint

### Requirement: 明确上下文溢出的自动恢复
普通模型请求只有在 provider 返回可确认的上下文溢出错误、且本次响应尚无 assistant 正文、reasoning 或 tool call 时，系统 SHALL 自动尝试修复并重试。429、5xx、超时和一般网络错误 MUST NOT 触发该修复状态机。

#### Scenario: 首次无输出溢出
- **WHEN** provider 在任何模型输出前以明确 400/413/422 上下文错误拒绝请求
- **THEN** 系统执行一次确定性历史修复，并在修复有效时自动重试同一已持久化用户输入

#### Scenario: 已有部分输出
- **WHEN** provider error 前已经产生 assistant 正文、reasoning 或 tool call
- **THEN** 系统 MUST NOT 自动重放该模型步骤或工具活动，并以错误结束当前回合

#### Scenario: 非上下文错误
- **WHEN** provider 返回 rate limit、服务端错误、超时或网络错误
- **THEN** 系统保持既有 retry/error 行为，不追加 repair checkpoint

### Requirement: 有限且单调的紧急请求配置
自动恢复 SHALL 最多依次经历 normal、history repaired 和 emergency profile 三个状态。emergency profile SHALL 保留基础系统指令、修复后的 history 和当前 user turn，省略可选请求局部上下文并缩减非核心工具 schema；该请求仍失败时 MUST 停止重试并返回明确不可恢复原因。

#### Scenario: 修复后仍被拒绝
- **WHEN** history repaired 请求再次收到明确上下文溢出
- **THEN** 下一次且仅下一次重试使用 emergency profile

#### Scenario: emergency profile 仍失败
- **WHEN** emergency profile 也收到上下文溢出
- **THEN** 系统结束回合，报告 fixed context/current input 仍超限或后端限制未知，并且不进入无限循环

#### Scenario: 当前输入只持久化一次
- **WHEN**自动恢复进行了一个或两个重试
- **THEN** JSONL 中原始当前 user message 仍只有一条，重试不会再次追加它
