# 采用 Codex Skill 目录策略

## Why

当前 Skill 索引在较小上下文窗口下会从“全部名称和描述”整体退化为“仅名称”，并且索引混在 user-role 的 session reminder 中。模型因此经常看不到用于自然语言匹配的描述，只在用户显式输入 Skill 名称时稳定调用。Codex 已有一套更稳健的生产策略，可以在受控预算内尽量保留每个 Skill 的匹配信息。

## What Changes

- 将 Skill 目录从通用 user-role session reminder 中拆出，作为独立的 request-local 高优先级指令消息发送；在 ACECode 的多 Provider 协议中使用 `system` 角色承载 Codex 的 developer-priority 语义。
- 按 Codex 规则把已知上下文窗口的 Skill 元数据预算改为 2% token；上下文窗口未知时回退到 8000 字符。
- 把单个 Skill 的目录描述上限改为 1024 个 Unicode code point，并让每条目录记录像 Codex 一样始终携带 `(file: <SKILL.md>)` 来源定位信息；只有绝对路径目录发生截断时才评估更省预算的 root alias 形式。
- 移植 Codex 的公平分配算法：完整目录超预算时，先保留所有 Skill 的“名称 + 来源定位”最小条目，再逐字符轮询分配剩余描述预算；仅在最小条目也放不下时才按稳定顺序省略放不下的条目。
- 对描述截断和 Skill 省略生成可观察的渲染报告与警告；host/local 目录与 Codex 一样不在模型可见目录中伪造省略条目。
- 移植 Codex 的显式选择链：识别 `$SkillName` 与 `[$SkillName](SKILL.md path)`，避开常见环境变量，按 registry 顺序去重，并把命中的完整 `SKILL.md` 以 user-role `<skill>` 指令片段自动注入当前轮。
- 把 ACECode 的 `/<skill-name>` 选择映射为同一显式 Skill mention 链，使 TUI、Web、子 Agent 首轮都获得一致的完整指令，而不是等待模型自行调用 `skill_view`。
- 保持 `skill_view`、`skills_list`、Skill 扫描根、启停策略及工作区作用域不变。Codex 的词法/BM25 逻辑仅在 shadow experiment 中记录指标、不参与模型可见召回，因此不把它误接成生产筛选器。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `skill-context-index`: 对齐 Codex 的生产召回链，包括高优先级带来源目录、预算分配、显式 mention 解析及完整 Skill 指令注入。

## Impact

- 主要代码：`src/prompt/system_prompt.{hpp,cpp}`、`src/skills/skill_activation.{hpp,cpp}`、`src/skills/skill_registry.{hpp,cpp}`、`src/agent_loop.{hpp,cpp}`、TUI/Web Skill command 展开路径。
- 主要测试：`tests/prompt/skills_index_prompt_test.cpp`、Skill mention/激活测试、Web/TUI command 回归测试，以及必要的 AgentLoop 请求装配测试。
- Provider 兼容：不新增 Provider 协议角色；独立目录统一使用所有现有 Provider 都能处理的 `system` 角色。
- 上下文成本：已知窗口上限从现有约 1% 字符预算调整为 Codex 的 2% token 预算；未知窗口仍保持 8000 字符兜底。
- 不增加配置项、外部依赖、网络请求或数据迁移；显式选择会按 Codex 语义把完整 Skill 指令写入模型会话上下文。
