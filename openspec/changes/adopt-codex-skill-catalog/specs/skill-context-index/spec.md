# Skill 上下文目录

## MODIFIED Requirements

### Requirement: Skill index visible in model context
系统 SHALL 在 Skill registry 至少包含一个可调用 Skill 时，把已安装 Skill 的紧凑目录作为独立、request-local、高优先级 `<skills_instructions>` system 消息随每次 Provider 请求发送。目录 SHALL 包含 Skill 名称、描述及现有可选触发条件，并保持现有 category 分组顺序；该消息 SHALL NOT 写入持久化 session transcript。

#### Scenario: Installed skills appear in an independent high-priority message
- **WHEN** Skill registry 包含 Skill 且系统构建 Provider 请求
- **THEN** 静态 system prompt 之后存在独立的 `<skills_instructions>` system 消息
- **AND** 该消息按现有 category 顺序包含每个已纳入预算的 Skill 名称和描述
- **AND** 通用 user-role session reminder 不再重复包含 Skill 目录

#### Scenario: Empty registry produces no skill message
- **WHEN** Skill registry 为空、为 null 或 `skill_view` 不可用
- **THEN** Provider 请求不包含 `<skills_instructions>` 消息或 Skill 目录标题

#### Scenario: Index is request-scoped and survives compaction setup
- **WHEN** Skill 目录被加入普通 Provider 请求或 compact 初始上下文
- **THEN** 两条路径都使用独立 system 消息承载相同目录
- **AND** 该消息不写入持久化 session transcript

### Requirement: Skill index character budget
系统 SHALL 对已知模型上下文窗口使用窗口大小 2% 的 token 预算，对未知或非法窗口使用 8000 个 UTF-8 字符预算。token 成本 SHALL 使用 `ceil(UTF-8 bytes / 4)` 近似值。每个 Skill 的组合描述 SHALL 最多保留 1024 个 Unicode code point，并在 UTF-8 边界上使用省略号截断。

#### Scenario: Known window produces a two-percent token budget
- **WHEN** 模型上下文窗口为 128000 tokens
- **THEN** Skill 元数据预算为 2560 tokens

#### Scenario: Unknown window uses character fallback
- **WHEN** 模型上下文窗口未知、为零或为负数
- **THEN** Skill 元数据预算为 8000 个 UTF-8 字符

#### Scenario: Long description is truncated per entry
- **WHEN** Skill 描述与现有可选触发条件组合后超过 1024 个 Unicode code point
- **THEN** 目录描述在合法 UTF-8 边界截断并以 `...` 结束
- **AND** 最终组合描述不超过 1024 个 Unicode code point

#### Scenario: Over-budget listing distributes descriptions fairly
- **WHEN** 所有 Skill 的完整目录超过预算，但所有最小名称条目可以容纳
- **THEN** 系统先保留所有 Skill 的名称
- **AND** 剩余描述预算按稳定 round-robin 顺序逐字符分配
- **AND** 任一靠前 Skill 不得在其它 Skill 仍为零描述时独占全部剩余描述预算

#### Scenario: Extreme overflow omits only entries that cannot fit
- **WHEN** 连全部最小名称条目也无法放入预算
- **THEN** 系统按稳定目录顺序保留可容纳的最小条目
- **AND** 目录包含省略 Skill 数量及 `skills_list` 兜底提示

### Requirement: Proactive loading guidance
静态 system prompt 与独立 `<skills_instructions>` 消息 SHALL 共同要求模型在回复前扫描模型可见 Skill 目录；用户显式命名 Skill 或任务明显匹配目录描述时，模型 MUST 先通过 `skill_view` 加载 Skill 再执行任务。`skills_list` SHALL 仅作为目录省略或需要复核时的兜底发现路径。

#### Scenario: High-priority catalog carries trigger rules
- **WHEN** 系统构建包含 Skill 的 Provider 请求
- **THEN** `<skills_instructions>` system 消息说明显式命名或任务明显匹配描述时必须加载 Skill
- **AND** 指令要求在执行任务前读取完整 SKILL.md

### Requirement: Skill index cache key tracks skill set changes
系统 SHALL 为独立 Skill 目录维护专用 cache key 和缓存内容。Skill 集合及索引元数据不变时 SHALL 字节级复用目录；Skill 新增、删除或索引元数据变化时 SHALL 刷新目录，而通用 session context 缓存 SHALL 不再因 Skill 目录内容承担该失效职责。

#### Scenario: Unchanged skill set reuses dedicated cache
- **WHEN** 连续两次请求具有完全相同的 Skill 集合与索引元数据
- **THEN** Skill 目录 cache key 和发送内容逐字节一致

#### Scenario: Skill set change refreshes only the catalog cache
- **WHEN** 请求之间新增、删除 Skill 或修改其目录描述
- **THEN** Skill 目录 cache key 发生变化并发送新内容
- **AND** 其它 session context 的内容不因 Skill 目录拆分而重复包含该索引

## ADDED Requirements

### Requirement: Skill catalog render diagnostics
系统 SHALL 记录 Skill 目录的总条目数、包含数、省略数、截断描述字符数及截断描述条目数，并在省略 Skill 或平均每个 Skill 被截断超过 100 字符时产生警告。相同 Skill cache key 的重复请求 MUST NOT 重复记录同一警告。

#### Scenario: Omitted skills produce one observable warning per catalog revision
- **WHEN** 目录预算导致一个或多个 Skill 被省略
- **THEN** 渲染报告包含准确的省略数量
- **AND** AgentLoop 在该 Skill cache key 首次出现时记录一次警告
- **AND** 后续使用相同 cache key 的请求不重复记录该警告

#### Scenario: Material description shortening produces a warning
- **WHEN** 平均每个 Skill 被截断的描述字符数大于 100
- **THEN** 渲染报告产生描述已缩短但仍保留全部可容纳 Skill 的警告
