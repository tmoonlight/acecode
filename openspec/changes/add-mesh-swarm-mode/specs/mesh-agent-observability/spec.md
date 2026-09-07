## Purpose

定义用户在 Web/Desktop 与 TUI 上如何看到网状 agent 树、跨 agent 消息以及来自任意层级子 agent 的权限请求与提问，并规定子 agent 的标题、隐藏与通知规则。

## ADDED Requirements

### Requirement: 后台任务面板按树展示网状 agent
root 会话的后台任务面板 SHALL 按 canonical 路径深度缩进展示全部子 agent，按状态分组（运行中、空闲、被换出、出错/中断），每项 SHALL 显示路径或任务名、状态、最后一条消息摘要、token 用量与耗时。被换出的 agent SHALL 可打开只读对话记录，且打开 MUST NOT 触发恢复。

#### Scenario: 二级子 agent 的缩进
- **WHEN** 面板展示 `/root/explore` 与 `/root/explore/tests`
- **THEN** `/root/explore/tests` 显示在 `/root/explore` 之下并多一级缩进
- **AND** 两者各自显示状态与最后一条消息摘要

#### Scenario: 查看被换出的 agent
- **WHEN** 用户点开状态为 `inactive` 的子 agent
- **THEN** 面板显示其对话记录
- **AND** 该 agent 的状态保持 `inactive`

### Requirement: root 空闲时显示未读的 agent 消息数
root 空闲且邮箱中存在尚未进入上下文的 `FINAL_ANSWER` 或 `MESSAGE` 信封时，后台任务入口与面板 SHALL 显示未读数；root 下一回合消费邮箱后 SHALL 清零。

#### Scenario: 子 agent 在 root 空闲后完成
- **WHEN** `/root` 已空闲，随后 `/root/a` 与 `/root/b` 相继完成
- **THEN** 后台任务入口显示未读数 2
- **AND** 用户向 `/root` 发送下一条消息后未读数归零

### Requirement: 跨 agent 消息在 transcript 中以独立行呈现
收件方的 Web/Desktop 与 TUI transcript（含恢复回放）SHALL 把跨 agent 信封渲染为独立的消息行，显示消息类型与发件人路径，MUST NOT 渲染为用户气泡。会话摘要（侧栏与列表使用的最后一条用户消息截断）MUST NOT 取自跨 agent 信封。

#### Scenario: 收件方看到来源
- **WHEN** `/root/a` 收到来自 `/root` 的 `MESSAGE` 信封
- **THEN** `/root/a` 的 transcript 出现一条标注「来自 /root · 消息」的独立行
- **AND** 该行不使用用户消息的气泡样式

#### Scenario: 恢复后的呈现一致
- **WHEN** 用户恢复一个曾收到信封的子 agent 会话
- **THEN** 信封行以相同的独立样式回放
- **AND** 该会话的摘要仍为其最后一条真实用户消息

### Requirement: 任意层级的权限请求与提问冒泡到 root 界面
任意深度子 agent 的权限请求与 AskUserQuestion SHALL 出现在 root 会话所在的界面（Web/Desktop 全局弹窗、TUI 确认/提问浮层），来源标签 SHALL 为该 agent 的 canonical 路径；用户的回答 SHALL 路由回发起请求的 agent。

#### Scenario: 二级子 agent 请求权限
- **WHEN** `/root/explore/tests` 请求执行写文件工具的权限
- **THEN** root 界面弹出权限确认并标注来源 `/root/explore/tests`
- **AND** 用户允许后该工具在 `/root/explore/tests` 中执行

#### Scenario: 子 agent 向用户提问
- **WHEN** `/root/a` 调用 AskUserQuestion
- **THEN** 提问出现在 root 界面并标注来源 `/root/a`
- **AND** 用户的回答返回给 `/root/a`

### Requirement: 网状子 agent 的标题与隐藏规则
网状子 agent 的会话标题 SHALL 为其 `task_name`，MUST NOT 为其触发自动起标题的模型调用；用户仍可手动改名。任意层级的子 agent 会话 MUST 继续从常规会话列表、侧栏与全局搜索中隐藏，只在其 root 会话的后台任务面板中出现。

#### Scenario: 派生不触发起标题
- **WHEN** `/root` 派生 `task_name="review_tests"` 的子 agent
- **THEN** 该子会话标题为 `review_tests`
- **AND** 没有发起隐藏的标题生成请求

#### Scenario: 二级子 agent 不出现在侧栏
- **WHEN** `/root/explore/tests` 存在
- **THEN** 侧栏与全局搜索结果均不包含它
- **AND** 它出现在 root 会话的后台任务面板中

### Requirement: 子 agent 完成不触发桌面通知
子 agent 回合完成 MUST NOT 触发桌面完成通知；root 的回合完成与提问通知按现有规则处理。

#### Scenario: 子 agent 完成
- **WHEN** `/root/a` 完成回合而 root 仍在运行
- **THEN** 不产生桌面通知
- **AND** root 回合完成时按现有规则通知

### Requirement: TUI /tasks 展示网状 agent
TUI 的 `/tasks` 与 `/tasks list` SHALL 为网状 agent 显示 canonical 路径与统一状态；`/tasks abort <id>` 对网状 agent SHALL 等价于中止其当前回合并保留该 agent。

#### Scenario: 列出网状 agent
- **WHEN** 用户在 `mesh` 模式的 TUI 会话中输入 `/tasks`
- **THEN** 每个子 agent 一行，显示路径与状态
- **AND** 被换出的 agent 标注为 `inactive`
