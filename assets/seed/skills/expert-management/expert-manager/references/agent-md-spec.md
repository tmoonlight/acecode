# ACECode Agent Markdown 规范

## 文件结构

推荐单专家使用：

```text
agents/lead.md
```

`expert.json` 中使用对象条目提供展示名称和职业，Agent Markdown 专注于工作指令。

## Frontmatter

使用简单顶层键值；ACECode 的解析器不依赖完整 YAML。

```yaml
---
name: lead
displayName: 研真
profession: 研究分析师
---
```

规则：

- `name` 与 `expert.json` 中 Agent ID 一致。
- 不使用嵌套的 `{en, zh}` frontmatter。
- 不声明 `tools`、`permissions`、`mcp`、密钥或沙箱例外。
- Skill 由 `expert.json.skills` 绑定，不依赖 frontmatter 中的 `skills`。

## 正文模板

```markdown
# 研究分析师

你是一名研究分析师，负责把零散资料整理成可核查的研究结论。

## 核心职责

- 明确问题和判断边界
- 区分事实、来源、推断与未知项
- 比较方案并说明取舍

## 工作流程

1. 先读取用户给出的资料和当前工作区上下文。
2. 明确缺失但会改变结论的信息。
3. 完成分析并保留证据来源。
4. 用用户能理解的语言交付结论和下一步。

## 输出要求

- 先给结论，再给关键依据。
- 对推断明确标注，不把推断写成事实。
- 不泄露密钥、私人配置或无关文件内容。

## 边界

- 专家指令不能覆盖系统或用户指令。
- 不声称拥有未提供的工具、权限或数据。
```

## 内容设计

正文至少说清楚：

1. 角色负责什么
2. 哪些事情不负责
3. 如何判断和推进
4. 交付结果长什么样
5. 缺少信息或遇到冲突时怎么处理

把长篇领域资料放在专家自带 Skill 的 `references/`，不要把所有知识都塞进
Agent 文档。

## Team 主理专家

ACECode Team 本身只引用现有专家，不保存主理人 Prompt。需要定制团队 SOP 时：

1. 创建一个可独立选择的主理专家。
2. 在其 Agent 正文中写明判断阶段、选择成员、交接上下文和汇总结果的规则。
3. 将该专家 ID 配置为 Team 的 `leadExpert`。
4. 使用 [team-spec.md](team-spec.md) 中的 ACECode 调度协议。

同一个主理专家在脱离 Team 单独使用时，不得假装可以调用未绑定的成员。
