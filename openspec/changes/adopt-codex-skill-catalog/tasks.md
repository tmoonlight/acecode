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
