# diagram-guidance Spec (delta)

## Purpose

在系统提示里常驻一段按运行面取舍的画图说明,让模型知道当前界面能渲染哪些图、什么时候该画、什么时候不画,并按需加载详细的画图规范技能。

## ADDED Requirements

### Requirement: mermaid 说明
TUI 与 daemon(Web / Desktop)会话的静态系统提示 SHALL 含一段画图说明的 mermaid 部分:说明 mermaid 代码块会被画成图;只列两端都能渲染的类型(flowchart 且必须写方向、sequenceDiagram、stateDiagram / stateDiagram-v2、classDiagram、erDiagram);列出会让图退回源码的写法(其它图类型、`%%{init}%%` 指令、front matter、除 `<br>` 外的 HTML 标签、链接与 URL、`click` 行);并给出什么时候画、什么时候不画。这部分不受「可视化图表」开关影响。

#### Scenario: TUI 会话
- **WHEN** 在 TUI 里开启一个普通会话
- **THEN** 系统提示含 mermaid 说明,不含任何 visual 字样

#### Scenario: 用户要架构图
- **WHEN** 用户说「画一下这个模块的调用流程」
- **THEN** 系统提示里已有足够信息让模型用 `flowchart TD` 写 mermaid,而不是 gantt 或带 `click` 的写法

### Requirement: visual 说明
只有 daemon 会话且 `inline_visuals.enabled` 为真时,画图说明 SHALL 再含 visual 部分:说明 visual 代码块是在对话里实时渲染的 SVG / HTML 片段;用于 mermaid 表达不了的内容(原理示意、可交互讲解、数据图表、方案对比卡片);写清核心约束(单个片段、顶层、不写 html/head/body、用框内主题变量、脚本在闭合后执行、只能用允许的 CDN、用 `ace.send` 追问)。画图规范技能可用时,SHALL 要求第一次画 visual 前先用当前模型侧的 skill_view 工具名加载它。

#### Scenario: 开关打开的网页会话
- **WHEN** Web 里开启会话,开关为默认的开启
- **THEN** 系统提示同时含 mermaid 与 visual 两部分,visual 部分点名要先加载 `inline-visuals` 技能

#### Scenario: 开关关闭
- **WHEN** 开关关闭后发消息
- **THEN** 系统提示只含 mermaid 部分

#### Scenario: 工具重写
- **WHEN** 「工具重写」把 skill_view 映射成了别的模型侧名字
- **THEN** visual 部分引用的是映射后的名字

#### Scenario: 技能被停用
- **WHEN** 用户在技能设置里停用了 `inline-visuals`
- **THEN** visual 部分仍给出核心约束,但不再要求加载该技能

### Requirement: 不需要画图说明的会话
headless(`-p`)会话、spawn_subagent 子会话与网状蜂群子 agent、群聊代答会话的系统提示 MUST NOT 含画图说明的任何部分。

#### Scenario: 子代理
- **WHEN** 网页会话通过 spawn_subagent 派出子会话
- **THEN** 子会话的系统提示不含画图说明

#### Scenario: headless
- **WHEN** 运行 `acecode -p "解释一下这个函数"`
- **THEN** 系统提示不含画图说明

### Requirement: 说明字节稳定
画图说明 SHALL 只由运行面、开关、技能是否可用与工具名映射决定,放在可缓存的静态系统提示里。同一会话里这些输入不变时,前后两次请求的系统提示 MUST 逐字节相同;不同会话、不同时间的同一组输入 MUST 得到相同文本。

#### Scenario: 同一回合多次工具往返
- **WHEN** 一个回合里模型连续调用三次工具
- **THEN** 三次请求的系统提示完全相同

#### Scenario: 切换开关
- **WHEN** 两次请求之间开关从开变为关
- **THEN** 只有切换后的那次请求系统提示改变,之后再次稳定

### Requirement: 画图规范技能
默认内置技能包 SHALL 含 `inline-visuals` 技能,正文写明隔离框提供的变量与类名、SVG 布局与配色规则、交互与脚本规则、图表写法与示例。visual 部分不生效的会话(TUI、headless、开关关闭、子会话)MUST NOT 在技能索引里列出它;生效时即使它因长期未用进入休眠,也 SHALL 能按名字加载。

#### Scenario: TUI 的技能索引
- **WHEN** TUI 会话组装技能索引
- **THEN** 索引里没有 `inline-visuals`

#### Scenario: 休眠后仍可加载
- **WHEN** `inline-visuals` 已 30 天未用、不在技能索引里,模型按系统提示用名字加载它
- **THEN** 返回完整的技能正文
