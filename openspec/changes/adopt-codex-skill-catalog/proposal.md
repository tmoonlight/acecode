# 采用 Codex Skill 目录策略

## Why

当前 Skill 索引在较小上下文窗口下会从“全部名称和描述”整体退化为“仅名称”，并且索引混在 user-role 的 session reminder 中。模型因此经常看不到用于自然语言匹配的描述，只在用户显式输入 Skill 名称时稳定调用。Codex 已有一套更稳健的生产策略，可以在受控预算内尽量保留每个 Skill 的匹配信息。

## What Changes

- 将 Skill 目录从通用 user-role session reminder 中拆出，作为独立的 request-local 高优先级指令消息发送；在 ACECode 的多 Provider 协议中使用 `system` 角色承载 Codex 的 developer-priority 语义。
- 按 Codex 规则把已知上下文窗口的 Skill 元数据预算改为 2% token；上下文窗口未知时回退到 8000 字符。
- 把单个 Skill 的目录描述上限改为 1024 个 UTF-8 字符，并保留现有描述与可选触发条件组合方式。
- 移植 Codex 的公平分配算法：完整目录超预算时，先保留所有 Skill 的最小条目，再逐字符轮询分配剩余描述预算；仅在最小条目也放不下时才省略尾部 Skill。
- 对描述截断和 Skill 省略生成可观察的渲染报告与警告，极端溢出时保留明确的省略数量提示。
- 保持 `skill_view`、`skills_list`、`/<skill-name>`、Skill 扫描顺序及持久化行为不变；不引入 Grok 的 `paths:`、独立 `when_to_use` 展示或 Codex 尚处于 shadow 状态的词法/BM25 选择器。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `skill-context-index`: 修改 Skill 目录的消息优先级、预算单位、单条上限和超预算分配规则，使自然语言请求在受限上下文中仍能看到尽可能多的 Skill 描述。

## Impact

- 主要代码：`src/prompt/system_prompt.{hpp,cpp}`、`src/agent_loop.{hpp,cpp}`。
- 主要测试：`tests/prompt/skills_index_prompt_test.cpp`，以及必要的 AgentLoop 请求装配测试。
- Provider 兼容：不新增 Provider 协议角色；独立目录统一使用所有现有 Provider 都能处理的 `system` 角色。
- 上下文成本：已知窗口上限从现有约 1% 字符预算调整为 Codex 的 2% token 预算；未知窗口仍保持 8000 字符兜底。
- 不增加配置项、外部依赖、网络请求或数据迁移。
