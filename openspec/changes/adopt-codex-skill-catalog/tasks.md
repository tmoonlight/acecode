## 1. Codex 目录预算与渲染器

- [x] 1.1 在 prompt API 中加入 Codex 风格的 token/character Skill 元数据预算及渲染报告，并把已知窗口预算改为 2% token、未知窗口回退为 8000 字符。
- [x] 1.2 将组合描述上限改为 1024 个 Unicode code point，保证 UTF-8 安全截断并保留现有 description/whenToUse 组合语义。
- [x] 1.3 移植完整、最小、round-robin 三阶段分配算法，保留 category 稳定顺序，并在极端溢出时给出准确省略提示。

## 2. 高优先级 Skill 指令块

- [x] 2.1 把动态目录包装为独立 `<skills_instructions>` 内容，并加入 Codex 风格的显式命名/描述匹配触发规则及专用 cache key。
- [x] 2.2 让通用 session-context 构建支持排除 Skill 目录，保证生产请求不再在 user-role `<system-reminder>` 中重复注入。

## 3. AgentLoop 请求装配

- [x] 3.1 为普通 Provider 请求增加独立 request-local Skill `system` 消息和专用缓存，并保持静态 system prompt 在最前。
- [x] 3.2 为 compact 初始上下文加入同一独立 Skill `system` 消息，确保消息不进入持久化 transcript。
- [x] 3.3 将独立消息纳入 `skills` 上下文用量统计，并仅在 Skill cache key 变化时记录渲染警告。

## 4. 验证

- [x] 4.1 更新 Skill prompt 单元测试，覆盖 2% token/8000 字符预算、1024 字符 UTF-8 截断、公平分配、极端省略和独立指令块。
- [x] 4.2 增加或更新请求装配/Provider 回归测试，证明独立 Skill system 消息在 OpenAI-compatible 与 Anthropic 路径中不会被丢弃或持久化。
- [x] 4.3 运行针对性单元测试、相关完整测试、代码质量检查、构建及 `openspec validate --strict`，修复全部回归。

## 5. Codex 来源目录完全对齐

- [x] 5.1 把目录条目改为 Codex 的 `name + description + (file: SKILL.md)` 单行格式，移除 category 标题与 `whenToUse` 拼接，并让最小条目始终保留来源定位。
- [x] 5.2 按 Codex 规则在绝对路径目录被截断时评估 root alias 版本，并以包含数、描述保留量、成本决定是否采用。
- [x] 5.3 对齐极端预算行为：逐条尝试保留可放入的最小条目，host/local 模型可见目录不追加省略 marker，诊断仍报告准确数量。
- [x] 5.4 更新高优先级 Skill 使用说明与静态提示，使来源定位、自然语言匹配和 `skill_view` 兜底语义一致。

## 6. Codex 显式 Skill 选择与自动注入

- [x] 6.1 移植 `$SkillName` / `[$SkillName](path)` 解析、常见环境变量排除、路径优先、registry 顺序与去重规则。
- [x] 6.2 为 SkillRegistry 增加完整 `SKILL.md` 读取能力，并生成 Codex 兼容的 user-role `<skill>` 指令片段。
- [x] 6.3 在 AgentLoop 建立本轮 user message 时自动注入显式 Skill，同时通过 `display_text` 保留原文并避免工具循环重复注入。
- [x] 6.4 将 TUI、Web 与子 Agent 的 `/<skill-name>` 展开改为规范 `$SkillName` mention，统一走同一注入路径。
- [x] 6.5 增加 mention、完整正文注入、Windows/Unix 路径、环境变量、重复 mention、逐轮行为及 command 展开的回归测试。

## 7. 完整验证与主分支交付

- [x] 7.1 运行新增及相关 Skill/Prompt/Provider/Session 单元测试，并修复回归。
- [x] 7.2 运行 Release 全量构建、完整 `ctest`、代码质量检查与 `openspec validate --strict`。
- [x] 7.3 审计 Codex 生产召回主链差异，确认 shadow selector 未被误作生产逻辑，验证无关 dirty patch 未变化。
- [x] 7.4 在当前 `master` 提交目标文件、推送并确认本地与 `origin/master` 一致。
