# Codex Skill 目录移植设计

## Context

ACECode 原先通过 `build_session_context_prompt()` 把 Skill 索引与项目规则、用户记忆、自定义指令和 Git 状态合并成一个 request-local user-role `<system-reminder>`。上一阶段已经完成独立高优先级目录与 Codex 预算算法，但目录仍只有名称/描述，没有 Codex 的来源定位；显式 `/<skill-name>` 也仍只是注入一段“请模型调用 `skill_view`”的轻量提示，没有进入 Codex 的自动 Skill 指令注入链。

Codex 的生产路径并不使用语义搜索器预选自然语言请求，而是把完整 Skill 目录作为 developer-priority 上下文交给模型，并用 2% token 预算及 round-robin 描述分配保持目录可匹配。ACECode 的 `ChatMessage` 与 Anthropic/OpenAI-compatible 适配层目前不支持通用 `developer` 角色，但所有 Provider 都支持并合并 request-local `system` 消息，因此需要做等价的协议映射。

## Goals / Non-Goals

**Goals:**

- 让 Skill 目录以高优先级、request-local、不可持久化的独立消息进入每次 Provider 请求和 compact 初始上下文。
- 按 Codex 的 2% token/8000 字符规则限制目录成本。
- 在完整描述放不下时公平保留所有 Skill 的部分描述，避免单个 Skill 或列表前部独占预算。
- 目录每条记录始终包含 Skill 名称、description 和 `SKILL.md` 来源定位，并在值得时使用 root alias 节省目录预算。
- 仅在连所有“名称 + 来源定位”最小条目都无法容纳时省略 Skill，并记录截断/省略诊断。
- 显式 `$SkillName`、Skill 链接和 `/<skill-name>` 选择自动注入完整 `SKILL.md`，且每轮重新按本轮输入选择。
- 保持现有 Skill 扫描根、禁用/允许策略、`skill_view` 与 `skills_list` 能力。

**Non-Goals:**

- 不引入 Grok 的 `paths:` 条件激活、动态邻近目录发现或独立 `Use when:` 格式。
- 不把 Codex 的 shadow lexical/BM25 experiment 当作生产召回器；它只记录实验指标，不改变模型可见目录或选择结果。
- 不改变 `skill_view`、`skills_list` 或 SKILL.md frontmatter 兼容性。
- 不新增用户配置开关。

## Decisions

### D1：用独立 request-local `system` 消息映射 Codex developer-priority

`build_skills_index_context_prompt()` 继续负责生成稳定内容与 cache key，但 AgentLoop 不再把它合入通用 user-role session context。请求装配时按以下顺序发送：静态 system prompt、独立 `<skills_instructions>` system 消息、对话历史及其它 request-local user context。

选择 `system` 而不是直接增加 `developer`，是因为 Anthropic Messages API 只接受顶层 system 文本，若在通用 `ChatMessage` 中透传 `developer`，当前 Anthropic 与部分 OpenAI-compatible 服务会丢弃或拒绝消息。各 Provider 已有合并多条 system 消息的逻辑，因此该映射在所有现有后端上保持高优先级。相比把目录拼入静态 system prompt，独立消息不会把可变 Skill 集伪装成静态缓存前缀。

### D2：预算类型与 Codex 保持一致

增加可区分 `Tokens` 与 `Characters` 的 Skill 元数据预算：

- 已知上下文窗口：`max(1, context_window_tokens * 2 / 100)` tokens。
- 未知或非法窗口：8000 个 UTF-8 字符。
- token 成本复用项目已有的 Codex 兼容 `approx_token_count()`，即 `ceil(UTF-8 bytes / 4)`。
- 单个 description 最多保留 1024 个 Unicode code point，尾部使用 `...`，不切断 UTF-8。

预算只约束 Skill 条目区域；固定的 `<skills_instructions>`、标题和使用说明与 Codex 一样不参与条目分配。

### D3：按 Codex 的来源目录与 round-robin 算法渲染

每条完整记录为 `- <name>: <description> (file: <locator>)`，最小记录为 `- <name>: (file: <locator>)`。目录使用 registry 的稳定顺序，不再额外渲染 ACECode category 标题或把非 Codex 的 `whenToUse` 拼进 description。渲染器先计算所有条目的完整成本与最小成本：

1. 完整条目总成本不超过预算时，保留全部描述。
2. 全部最小条目可以容纳时，先为每个 Skill 保留名称及来源定位，再逐 Unicode 字符轮询增加描述，直到剩余预算不足。
3. 连全部最小条目也无法容纳时，按稳定目录顺序逐条尝试保留最小条目；前一条放不下不妨碍后续较短条目利用剩余预算。

绝对路径版本已经完整容纳全部描述时直接使用。只有发生描述截断或条目省略时，才根据 `scan_root` 构造 `r0`、`r1` root alias 表，扣除 alias 表本身的成本后重新分配，并按“包含条目更多、截断描述更少、总成本更低”的 Codex 顺序决定是否采用 alias 版本。

### D4：渲染报告与警告不污染目录预算

渲染结果记录总数、包含数、省略数、被截断描述字符数和被截断描述条目数。若省略 Skill，生成包含省略数量的高优先级警告；若平均每个 Skill 被截断超过 100 字符，生成描述缩短警告。AgentLoop 仅在 Skill cache key 变化时写日志，避免每轮重复刷屏。host/local 目录不添加模型可见省略 marker；目录警告和 `skills_list` 仍提供可观察与人工复核路径。

### D5：独立缓存、compact 与上下文统计同步迁移

AgentLoop 增加 Skill 目录专用 cache key/content，session context 构建增加“是否包含 Skill”参数，生产请求与 compact 初始上下文均关闭旧的 user-role 合并路径并单独注入 system 消息。上下文用量估算仍把该消息计入 `skills` 分类；Skill 消息不会写入 `messages_` 或 session JSONL。

### D6：显式 mention 自动注入完整 Skill 指令

增加与 Codex 同构的 ASCII mention 解析器：支持 `$name` 以及 `[$name](path)`，名称字符限定为字母、数字、`_`、`-`、`:`，并忽略 `PATH`、`HOME`、`USER`、`SHELL`、`PWD`、`TMPDIR`、`TEMP`、`TMP`、`LANG`、`TERM`、`XDG_CONFIG_HOME` 等常见环境变量。链接路径优先按规范化的 `SKILL.md` 绝对路径匹配，普通 `$name` 再按唯一名称匹配；结果按 registry 发现顺序输出并去重。

AgentLoop 在建立本轮 user message 时读取每个显式命中的完整 `SKILL.md`（包含 frontmatter），追加 Codex 兼容的 `<skill><name>...<path>...` user-role 指令片段，并用 `display_text` 保留界面中的原始用户输入。注入只由本轮输入触发；工具循环不会重复注入，同名 Skill 在后续轮只有重新 mention 才会再次注入。`/<skill-name>` 只改写成规范 `$SkillName` mention，再复用同一解析与注入路径，避免 TUI、Web、子 Agent 各自维护不同逻辑。

## Risks / Trade-offs

- **[风险] 2% token 预算高于当前 1% 字符预算，目录可能占用更多上下文** → 复用 Codex 上限，且实际只渲染现有条目，不会为了填满预算生成内容。
- **[风险] `ceil(bytes/4)` 对中文的 token 估计仍是近似值** → 与项目 compact 及 Codex 当前实现保持一致，预算结果确定且可测试。
- **[风险] 多条 system 消息在不同 Provider 的线格式不同** → OpenAI-compatible 适配层统一合并 system 消息，Anthropic 适配层统一拼接顶层 system 文本；增加针对两类序列化路径的回归测试或沿用现有覆盖。
- **[风险] 来源路径增加单条最小成本** → 与 Codex 一样在必要时使用 root alias，并优先保住可匹配描述和条目数量。
- **[风险] 显式重复 mention 会增加会话上下文** → 这是 Codex 的逐轮显式选择语义；同一轮只注入一次，工具循环不重复，且界面仍显示用户原文。
- **[风险] 更完整的描述可能提高边缘相关 Skill 的误调用** → 使用 Codex 原始触发规则和 Skill 自身 description，不增加生产筛选器或二次分类器。

## Migration Plan

无需数据或配置迁移。发布后新请求自动使用独立 Skill system 消息；旧 session 在下一次 Provider 请求时得到新目录。回滚只需恢复旧的 session reminder 合并和旧格式化器，不影响持久化会话或 SKILL.md。

## Open Questions

无。
