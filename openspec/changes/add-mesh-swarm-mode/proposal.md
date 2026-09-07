## Why

ACECode 现有的「蜂群模式」是星型拓扑：只有主会话能派遣子代理（`spawn_subagent` / `wait_subagent`），子代理之间互不可见、无法通信，结果只能回到父会话；子代理也不能再派生。Codex 最新的 Multi-Agent V2 已经换成网状协作：每个 agent 拿到同一套工具，用文件系统式路径（`/root/task1/task_3`）互相寻址，任意 live agent 之间可以列举、发消息、派后续任务、等待与中断，且可以无限嵌套派生。本变更把这套模型复刻进 ACECode，作为第二种蜂群模式提供，让开发成员可以并排测试两种拓扑。

## What Changes

- **现有蜂群模式改名为「蜂群模式（星型）」**，行为一字不改（同一段主动委派引导、同一对 `spawn_subagent` / `wait_subagent` 工具、深度 1 限制）。
- **新增「蜂群模式（网状）」**，复刻 Codex Multi-Agent V2 的语义：
  - 六个协作工具，按 ACECode 的 `名词_动作` 命名惯例加 `agent_` 前缀，动词沿用 Codex：`agent_spawn`、`agent_list`、`agent_send_message`、`agent_followup_task`、`agent_wait`、`agent_interrupt`。
  - 每个 agent 拥有持久化的 canonical 路径（`/root`、`/root/<task_name>`、`/root/<task_name>/<sub_task>`），相对名相对调用者自身路径解析，任意 live agent 可用 canonical 路径寻址。
  - 每个 agent 一个邮箱：`agent_send_message` 只入箱不唤醒；`agent_followup_task` 对空闲目标开新回合、对运行中目标在下一处消息边界投递；子 agent 完成时把最终答复作为 `FINAL_ANSWER` 投进父邮箱且不唤醒空闲的父；`agent_wait` 等待自己邮箱的活动而不是等待指定目标。
  - 派生深度不限，由每棵树的并发驻留上限（默认 4，含 root）约束；超限时按 LRU 换出空闲 agent（落盘后卸载），再被寻址时透明恢复。
  - `fork_turns` 控制子 agent 继承多少父上下文，默认 `all`（只继承用户消息与 assistant 最终回复，不含工具往返与推理）。
  - `agent_type` 映射到 ACECode 的专家团成员；网状模式下树内任意 agent 都可指定。
  - 根 / 子 agent 的角色提示词与「主动委派」模式文案从 Codex 移植并适配 ACECode 工具名，放在按请求变化的上下文段，不碰可缓存的静态系统提示前缀。
- **模式成为会话级状态**（`off` / `star` / `mesh`），持久化到会话元数据、恢复时保留、派生时由子 agent 继承；Web/Desktop 输入框 `+` 菜单改为两项互斥单选，TUI 新增 `/swarm [star|mesh|off]`，headless 新增 `--swarm`。
- **两套工具互斥**：网状回合隐藏 `spawn_subagent` / `wait_subagent` 与全部 thread 工具；非网状回合隐藏六个 `agent_*` 工具。树内仍有 live agent 时不允许关闭网状模式。
- **跨 agent 消息以 `user` 角色 + `<inter_agent_message>` 标签注入**（有意偏离 Codex 的 `assistant` 角色，因为 Anthropic / DeepSeek 等 provider 不接受连续 assistant 消息），信封四字段（Message Type / Task name / Sender / Payload）照抄 Codex；收件方 transcript 中渲染为独立的「来自 /root/xxx」行，不是用户气泡。
- **观测面**：后台任务面板按树缩进、按状态分组、显示路径/最后一条消息/被换出状态与 root 的未读消息数；任意层级子 agent 的权限请求与提问冒泡到 root 界面并标注路径；网状子 agent 标题直接使用 `task_name`，不再触发隐藏的自动起标题模型调用。
- 消息 API 的 `swarm_mode` 字段从布尔扩展为 `"star" | "mesh" | false`，旧的 `true` 继续视为 `star`，不构成破坏性变更。
- 取代从未实施的 `add-agent-tree-collaboration` 提案（深度 1、祖先专属 follow-up、邮箱限额与共享预算 epoch 均与 Codex 路线冲突）；该提案随本变更归档。

## Capabilities

### New Capabilities

- `swarm-mode-selection`: 两种蜂群模式的会话级选择、持久化与继承，各表面（Web/Desktop、TUI、headless）的入口，消息 API 契约，以及星型 / 网状两套工具的互斥暴露规则。
- `mesh-agent-collaboration`: 网状模式的 agent 身份与路径、六个协作工具的输入输出与错误条件、邮箱投递语义（普通消息 / 后续任务 / 最终答复）、等待与中断、上下文继承、专家团映射与角色提示词。
- `mesh-agent-residency`: 每棵树的并发驻留上限、LRU 换出与透明恢复、agent 状态枚举、daemon 重启后的重建行为。
- `mesh-agent-observability`: 后台任务面板的树形展示与未读提示、跨 agent 消息在 transcript 中的呈现、任意层级的权限 / 提问冒泡、子 agent 标题与通知规则。

### Modified Capabilities

无。星型模式沿用 `add-desktop-swarm-mode` 已交付的行为，仅改显示名称；该变更尚未归档，其 `desktop-swarm-mode` 能力不在主规格中，本变更不对它提出需求变更。

## Impact

- **后端核心**：`src/agent_loop.{hpp,cpp}`（邮箱、投递边界、隐藏内部输入、模式驱动的工具可见性、请求上下文）、`src/session/session_registry.{hpp,cpp}` / `session_client.hpp` / `local_session_client.cpp`（身份字段、驻留控制、恢复重订阅）、`src/session/session_storage.{hpp,cpp}` / `session_manager.{hpp,cpp}`（元数据字段、fork 过滤）、`src/prompt/system_prompt.*` 与新增的网状提示词模块、`src/tool/`（六个新工具、`spawn_subagent` 深度规则不变、thread 工具隐藏）、`src/tool/tool_executor.hpp`（`ToolCapabilityPolicy` 扩展）、`src/config/config.hpp`（`swarm.mesh.*` 配置）。
- **接口**：`POST /api/sessions/:id/messages` 的 `swarm_mode` 取值扩展；`SessionSummary` / `session_status` 增加模式与 agent 身份、状态字段；`docs/daemon-api.md` 同步。
- **Web/Desktop**：`InputBar.jsx`、`ComposerSessionControls.jsx`、`ChatView.jsx`、`chatInputQueue.js`、`SubagentPanel.jsx`、`subagentTasks.js`、`useSubagentTasks.js`、`Message.jsx`、i18n 目录与英文覆盖。
- **TUI / headless**：`builtin_commands.cpp`（`/swarm`、`/tasks` 显示路径）、`subagent_host.{hpp,cpp}`（恢复后重订阅、路径标签）、`headless_options.*` / `headless_runner.cpp`（`--swarm`）、`main.cpp` / `worker.cpp` / `headless_runner.cpp` 三处工具注册。
- **文档**：`docs/subagents.md`、`docs/help/swarm.html` 文案、`CLAUDE.md` 子代理段落。
- **依赖**：无新第三方库。
- **成本提示**：`fork_turns=all` 默认让每个子 agent 起步即携带父对话副本，token 成本随 agent 数线性增长；这是按 Codex 默认值的有意选择，可通过参数改为 `none` 或整数。
