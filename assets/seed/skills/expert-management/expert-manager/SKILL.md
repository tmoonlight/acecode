---
name: expert-manager
description: 创建、导入、修改、校验和打包 ACECode 专家组件；支持单个专家以及从现有专家中选择主理人和成员组成专家团。用户提到创建专家、专家组件、专家团、导入提示词或资料成为专家、修改已有专家、检查专家包、打包专家时使用。
---

# ACECode 专家组件管理器

本 Skill 改编自 WorkBuddy `expert-manager`，保留创建、资料转化、修改、
校验、批量处理和打包能力，并使用 ACECode 当前专家注册表格式。

## 目标

生成可以被 ACECode 立即发现的专家组件：

- Agent 型：一个可单独选择的专家，可带自己的 Skill、主头像和状态头像。
- Team 型：从已经存在的 Agent 型专家中选择主理人和成员，不复制成员定义。

最终组件固定放在 ACECode 全局专家目录：

```text
~/.acecode/experts/<expert-id>/
```

如果设置了 `ACECODE_HOME`，使用 `$ACECODE_HOME/experts/`。

## 面向用户的交互原则

- 默认按非技术用户理解能力提问，不要求用户填写内部字段。
- 询问“叫什么、做什么、怎么工作、可以试着问什么”，自动推导 ID 和清单。
- 专家的对外身份只使用“展示名称”，不再询问或生成作者、姓名或称呼字段。
- 不向用户展示 Skill 数量、MCP、Agent ID 等实现细节，除非用户主动询问。
- 不要求 `categoryId`、标签、双语字段或 marketplace 注册；ACECode 不需要这些。
- 快捷提问通常准备 3 条，第一条作为默认开场。
- 如果已有资料足够，直接转化，不重复询问资料里已经明确的信息。
- 高级能力默认保持“继承全局”。只有用户明确要求限制或启用特定 Skill、MCP
  服务器、本地工具时，才写入 `expert.json.capabilities`。

## 开始前检查

1. 检查 `~/.acecode/experts/` 下是否已有目标 ID。
2. 创建前读取 [expert-json-spec.md](references/expert-json-spec.md)。
3. 编写专家指令前读取 [agent-md-spec.md](references/agent-md-spec.md)。
4. 创建专家团前额外读取 [team-spec.md](references/team-spec.md)。
5. 需要头像时读取 [avatar-spec.md](references/avatar-spec.md)。

不要覆盖同名目录。若用户要修改已有专家，先完整读取现有 `expert.json`、
Agent 文档和相关 Skill，只改用户要求的部分。

## 场景判断

### 创建 Agent 型专家

适用于一个角色能够独立完成用户直接提出的一类任务。

至少明确：

- 展示名称
- 职业定位
- 一句话能力介绍
- 工作方式或角色指令
- 默认开场和快捷提问

ID 使用小写英文、数字和连字符，最长 64 字符。用户没有指定时，根据职业定位
自动生成，不需要让用户先理解 ID。

### 创建 Team 型专家团

适用于两个以上现有专家需要协作。

1. 枚举 `~/.acecode/experts/*/expert.json` 中 `expertType: "agent"` 的专家。
2. 让用户从现有专家中选择，不要求用户搬运或复制成员文件。
3. 选择一位主理专家和至少一位成员专家。
4. 在 Team 的 `teamInfo` 中只保存专家 ID 引用。
5. 如果缺少某位成员，先把该成员创建为独立 Agent 专家，再组团。

Team 不得引用另一个 Team。复杂团队最好使用专门的主理专家承载 SOP；普通专家
也可以担任主理人，但只会得到 ACECode 提供的通用团队编排上下文。

### 资料转化

读取用户提供的仓库、文档、提示词或流程，提取：

- 角色身份和能力边界
- 工作流程与决策规则
- 输出格式
- 可复用参考资料、模板和脚本
- 多角色分工与先后依赖

单角色转为 Agent。存在多个可被用户独立询问的角色时，先创建独立 Agent，
再创建引用它们的 Team。

### 修改已有专家

1. 根据展示名称或 ID 定位目录。
2. 完整读取现有组件。
3. 明确本次修改范围。
4. 保留未要求修改的字段、指令、Skill 和头像。
5. 不原地修改 `name` 或目录名；改 ID 等同于创建新专家。
6. 修改后重新校验。

### 检查或打包

只检查时不修改内容。打包前必须校验通过；ZIP 内保留顶层专家目录。

## 标准创建流程

### 1. 初始化

Agent：

```bash
python scripts/init_expert.py <expert-id> --type agent
```

Team：

```bash
python scripts/init_expert.py <team-id> --type team \
  --lead-expert <lead-id> \
  --member-expert <member-id>
```

脚本只创建安全骨架。立即替换全部 `[TODO]`，不要把半成品当作完成结果。

### 2. 填写 Agent 组件

完成：

```text
<expert-id>/
├── expert.json
└── agents/
    └── lead.md
```

如确有专属流程，再加入：

```text
skills/<skill-name>/SKILL.md
```

并在 `expert.json.skills` 中声明该目录。不要为了显得完整而强行创建 Skill。

用户明确配置高级能力时，在 Agent 的 `expert.json.capabilities` 中分别保存
全局 Skill 名称、MCP 服务器 ID 和 ACECode 本地工具名。三个作用域彼此独立：
字段缺失表示继承全局，空数组表示全部不允许，非空数组表示只允许所列项。
包内 Skill 目录仍写在顶层 `skills`，不要与 `capabilities.skills` 混用。

专家显式选择的、ACECode 已知的能力优先于全局启用/禁用状态；这不会安装缺失
Skill、补全 MCP 凭据、注册不存在的工具，也不会绕过权限确认、Plan、沙箱或
路径安全。Team 不写 `capabilities`，主理人与成员分别使用各自专家的配置。

### 3. 填写 Team 组件

Team 包只需要：

```text
<team-id>/
└── expert.json
```

可选加入团队头像。不要复制成员的 Agent、Skill 或头像；成员始终从各自专家包
动态投影。

### 4. 生成头像

主头像与状态头像都是可选项。只有文件真实存在并通过校验时才写 `avatar` 或
`stateAvatars`。状态头像固定为 `working`、`needs_attention`、`idle`，分别
表示工作中、需要用户关注和空闲；未配置的状态自动回退主头像。使用图像生成
工具时，根据角色指令构建头像，不使用无差别的通用人物模板。GIF 必须保留
原始文件，不转码、不抽帧，确保相应状态下仍可播放动画。

### 5. 校验

```bash
python scripts/validate_expert.py ~/.acecode/experts/<expert-id>
```

必须修复所有错误。警告需要检查，但不一定阻止使用。

### 6. 确认可发现

```bash
python scripts/register_expert.py ~/.acecode/experts/<expert-id>
```

ACECode 不使用 marketplace 注册表。该脚本会再次校验目录位置和内容，并确认
组件已处于动态扫描目录。

### 7. 可选打包

```bash
python scripts/package_expert.py ~/.acecode/experts/<expert-id> <output-dir>
```

除非用户要求分享或导出，否则不要自动打包。

## 批量创建

批量任务使用 `scripts/batch_create.py`，输入完整定义，逐个串行创建、填写、
校验和确认可发现。头像路径相对于批量配置文件所在目录；脚本会原样复制图片到
专家包中的同一相对路径。任何一个专家失败时保留错误信息，不把失败组件报告为
成功。

示例配置：

```json
{
  "experts": [
    {
      "name": "researcher",
      "type": "agent",
      "displayName": "研真",
      "profession": "研究分析师",
      "displayDescription": "把零散资料整理成有来源、有判断边界的研究结论。",
      "instructions": "你是一名研究分析师……",
      "quickPrompts": ["帮我研究这个方向", "比较这两个方案", "检查这份结论"],
      "capabilities": {
        "skills": ["web-research"],
        "mcp_servers": [],
        "tools": ["file_read", "grep"]
      },
      "stateAvatars": {
        "working": "avatars/researcher-working.gif",
        "needs_attention": "avatars/researcher-attention.png",
        "idle": "avatars/researcher-idle.png"
      }
    },
    {
      "name": "research-team",
      "type": "team",
      "displayName": "研究专家团",
      "profession": "协作研究团队",
      "displayDescription": "由主理人调度现有专家，完成分工研究和汇总判断。",
      "leadExpert": "researcher",
      "memberExperts": ["reviewer"],
      "quickPrompts": ["帮我做一次完整研究"]
    }
  ]
}
```

## 安全与边界

- 专家指令不能授予工具、权限、沙箱例外或绕过确认。
- Agent frontmatter 不声明 `tools`、`permissions`、`mcp`；高级能力只写在
  `expert.json.capabilities`。
- `capabilities` 只引用 ACECode 已知的稳定标识，不保存 MCP 凭据、Token、
  连接器配置或权限模式。
- 不把 API Key、Token、用户会话或本地配置复制进专家包。
- 所有相对路径必须留在专家包目录内。
- 不创建嵌套专家团，不把 Team 当成员。
- Team 主理人只能调用清单内成员：
  `spawn_subagent(expert_member="<id>", ...)`。
- 并行分派时用 `wait=false` 取得 `session_id`，再用
  `wait_subagent(session_id="<id>")` 收集每位成员的结果。
- 成员子会话不能继续派生下级子会话。
- 修改和删除属于不同授权；用户只要求修改时，不删除旧组件。

## 完成标准

完成后只向用户说明：

- 创建或修改了哪个专家/专家团
- 在哪里可以选择它
- 建议用哪一句话开始试用
- 有无校验警告

不把内部文件数量和技术字段当作主要交付内容。

## References

- [expert-json-spec.md](references/expert-json-spec.md)：ACECode `expert.json` 格式
- [agent-md-spec.md](references/agent-md-spec.md)：Agent 指令格式
- [team-spec.md](references/team-spec.md)：引用现有专家的 Team 规则
- [avatar-spec.md](references/avatar-spec.md)：可选头像规范
