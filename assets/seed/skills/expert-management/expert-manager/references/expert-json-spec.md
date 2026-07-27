# ACECode expert.json 规范

## 通用规则

- 文件固定为专家目录根部的 `expert.json`。
- `name` 必须与专家目录名完全一致。
- ID 只能包含小写英文字母、数字和连字符，不能以连字符开头或结尾，
  最长 64 字符。
- 展示字段优先使用普通字符串。ACECode 可以读取本地化对象，但当前界面只需要
  一个最终展示值。
- `quickPrompts` 最多 24 条，每条不超过 4096 字节。
- `avatar`、Agent 路径和 Skill 路径必须是包内相对路径，且目标真实存在。
- Agent frontmatter 不声明工具、权限或 MCP。Agent 清单可以通过
  `capabilities` 引用 ACECode 已知的 Skill、MCP 服务器和本地工具，但不保存
  凭据、连接器配置或权限模式。
- 不写 `author`。ACECode 只使用 `displayName` 作为专家对外名称；来源和许可
  信息保留在 `NOTICE.md`、`license` 与 `homepage` 中。

## Agent 型必需字段

```json
{
  "name": "researcher",
  "version": "1.0.0",
  "expertType": "agent",
  "displayName": "研真",
  "profession": "研究分析师",
  "displayDescription": "把零散资料整理成有来源、有判断边界的研究结论。",
  "quickPrompts": [
    "帮我研究这个方向",
    "比较这两个方案",
    "检查这份结论"
  ],
  "defaultInitPrompt": "帮我研究这个方向",
  "agentName": "lead",
  "agents": [
    {
      "id": "lead",
      "path": "agents/lead.md",
      "displayName": "研真",
      "profession": "研究分析师"
    }
  ]
}
```

可选字段：

```json
{
  "skills": ["skills/research-workflow"],
  "capabilities": {
    "skills": ["web-research"],
    "mcp_servers": [],
    "tools": ["file_read", "grep"]
  },
  "avatar": "avatars/researcher.png",
  "license": "Apache-2.0",
  "homepage": "https://example.com"
}
```

### Agent 规则

- `agents` 必须有 1 到 32 个条目。
- 推荐一个专家只使用 `lead` 一个 Agent；多角色协作优先拆成独立专家再组 Team。
- `agentName` 必须等于 `agents[].id` 中的一项。
- 每个 Agent 文档正文不能为空，单文件不得超过 256 KiB。
- `skills` 中每一项必须指向包内已存在目录。

### 高级能力作用域

`capabilities` 只允许以下三个可选数组，每项使用 ACECode 运行时目录中的稳定
标识：

- `skills`：全局 Skill 名称，不是包内目录路径。
- `mcp_servers`：已配置 MCP 服务器 ID，不是工具显示名。
- `tools`：ACECode 已注册的本地工具名，例如 `file_read`、`file_write`、
  `grep`、`ask_user_question`。

每个数组最多 256 项，每项 UTF-8 不超过 256 字节且不得重复。语义为：

- 键缺失：该能力类别继承全局配置。
- 键存在且为空数组：该类别明确全部禁用。
- 键存在且非空：仅允许所列标识。

专家显式选择的已知能力优先于全局启用/禁用状态，但不能安装缺失 Skill、创建
MCP 配置或凭据、注册不存在的工具，也不能绕过权限确认、Plan、沙箱、路径和
其他安全限制。包内专属 Skill 仍由顶层 `skills` 声明，不受
`capabilities.skills` 代替。

## Team 型必需字段

ACECode 的标准专家团引用已经存在的 Agent 型专家：

```json
{
  "name": "research-team",
  "version": "1.0.0",
  "expertType": "team",
  "displayName": "研究专家团",
  "profession": "协作研究团队",
  "displayDescription": "由主理人调度现有专家，完成分工研究、交叉检查和最终汇总。",
  "quickPrompts": [
    "帮我做一次完整研究",
    "让团队比较这两个方案",
    "让团队复核这份报告"
  ],
  "defaultInitPrompt": "帮我做一次完整研究",
  "teamInfo": {
    "leadExpert": "researcher",
    "memberExperts": [
      "reviewer",
      "fact-checker"
    ]
  }
}
```

可选头像：

```json
{
  "avatar": "avatars/team.png"
}
```

### Team 规则

- `leadExpert` 和 `memberExperts` 填专家包 ID，不是 Agent 文档 ID。
- `memberExperts` 至少一项；主理人与成员合计最多 32 位。
- 主理人不能同时出现在成员列表中。
- 不能引用自己、重复成员、缺失专家或另一个 Team。
- 引用的专家必须在 ACECode 当前扫描范围内可用。全局 Team 应引用全局 Agent，
  避免只在某个工作区可用。
- 引用式 Team 不包含 `agents`、`agentName` 或复制来的 `skills`。
- Team 不声明 `capabilities`；主理人与成员各自使用被引用专家的高级能力作用域。
- 主理人和成员的指令、头像及 Skill 始终来自各自专家包。

## 路径与体积

- `expert.json` 不得超过 128 KiB。
- `displayName`、`profession` 不得超过 512 字节。
- `displayDescription`、`defaultInitPrompt` 不得超过 64 KiB。
- 相对路径不能是绝对路径，解析后不能逃出专家包。
