## Why

采样参数目前只能保存在模型配置中，普通子 agent、蜂群子 agent 与专家预设无法按任务选择不同的采样方式。专家复用子会话执行机制，需要共享同一套参数校验、优先级与生命周期处理。

## What Changes

- 为 `spawn_subagent` 和 `agent_spawn` 增加可选 `sampling` 对象，支持现有 `temperature`、`top_p`、`top_k`、`min_p`。
- 专家包、管理 API 与专家编辑器支持保存、回填、清除采样预设；专家团复用成员的预设。
- 按字段合并模型默认值、所选专家预设、本次发起覆盖值，参数仅作用于目标会话。
- 会话持久化显式覆盖值；创建、恢复、模型切换、热重载与专家切换使用相同规则，并校验实际 provider/protocol 支持。

## Capabilities

### New Capabilities
- `agent-sampling-overrides`: 子 agent 发起与专家预设共享的采样配置及生命周期。

### Modified Capabilities

无。现有会话模型选择的隔离与失败回滚要求继续适用。

## Impact

涉及配置采样辅助函数、ExpertRegistry、SessionOptions/SessionManager/SessionStorage、SessionRegistry、两种派生工具、专家编辑器和 expert-manager 包格式文档。复用现有 provider 请求构造，不增加依赖，不修改全局已保存模型。
