# ACECode expert.json 规范

## 通用规则

- 文件固定为专家目录根部的 `expert.json`。
- `name` 必须与专家目录名完全一致。
- ID 只能包含小写英文字母、数字和连字符，不能以连字符开头或结尾，
  最长 64 字符。
- 展示字段优先使用普通字符串。ACECode 可以读取本地化对象，但当前界面只需要
  一个最终展示值。
- `quickPrompts` 最多 24 条，每条不超过 4096 字节。
- `avatar`、`stateAvatars`、Agent 路径和 Skill 路径必须是包内相对路径，
  且目标真实存在。
- Agent frontmatter 不声明工具、权限或 MCP。Agent 清单可以通过
  `capabilities` 引用 ACECode 已知的 Skill、MCP 服务器和本地工具，但不保存
  凭据、连接器配置或权限模式。
- 不写 `author`。ACECode 只使用 `displayName` 作为专家对外名称；来源和许可
  信息保留在 `NOTICE.md`、`license` 与 `homepage` 中。

## 采样参数（可选）

Agent 和 Team 的 `expert.json` 都可包含：

```json
"sampling": { "temperature": 0.3, "top_p": 0.95, "top_k": 20, "min_p": 0 }
```

每个字段独立可选，`null` 表示继承：temperature 为 0 到 2，top_p 大于 0 且不超过 1，
top_k 为 1 到 2147483647 的整数，min_p 为 0 到 1。数值 0 必须保留。
包内 `agents` 对象项也可声明 `sampling`，覆盖包级默认值。
团队引用专家时使用各专家的有效预设，团队默认值补充未设置字段。
子 agent 发起时传入的 `sampling` 具有最高优先级，模型设置作为最终默认值。

实际运行仍受所选 provider/protocol 限制。Responses 不支持 top_k/min_p，Anthropic
不支持 min_p 且 temperature 不超过 1。无法生效的设置会在会话启动或切换时被拒绝。
管理 API 更新时省略 sampling 保留原预设，传 null 或空对象清除全部预设。
单专家详情返回包含 lead 覆盖后的有效值；显式更新 sampling 时一并清除 lead 的旧覆盖。
专家团的更新只替换团队默认值，被引用专家的预设保持独立。
`batch_create.py` 接受同名字段，`validate_expert.py` 校验类型、范围和未知键。

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
  "stateAvatars": {
    "working": "avatars/researcher-working.gif",
    "needs_attention": "avatars/researcher-attention.png",
    "idle": "avatars/researcher-idle.png"
  },
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
  "avatar": "avatars/team.png",
  "stateAvatars": {
    "working": "avatars/team-working.gif",
    "needs_attention": "avatars/team-attention.png",
    "idle": "avatars/team-idle.png"
  }
}
```

### 状态头像

- `stateAvatars` 只允许 `working`、`needs_attention`、`idle` 三个可选键。
- `working` 用于执行、编程或处理任务；`needs_attention` 用于向用户提问或等待
  确认；`idle` 用于无操作或等待下一项任务。
- 任一状态未配置时使用主 `avatar`；主头像也不存在时沿用无头像表现。
- 支持 PNG、JPEG、GIF、WebP、BMP 和 ICO，单文件不得超过 8 MiB。
- GIF 按原始字节保存和提供，不缩放、转码、抽帧或静态化。
- 只有真实图片文件已经位于包内时才能写路径；不得生成占位文件或失效引用。

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
