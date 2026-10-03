## Context

动机见 proposal.md。这里只记录决定实现走向的现状：

- **现有蜂群模式是回合级布尔**：Web 把 `swarm_mode: true` 放进消息体，`routes_sessions.cpp` 写入用户消息 metadata，`AgentLoop::run_agent_with_input` 把它读进 `active_turn_swarm_mode_`，`build_swarm_mode_context_prompt`（`src/prompt/system_prompt.cpp`）在按请求变化的上下文段注入引导；回合结束即复位，子会话不知道父处于蜂群模式。
- **子代理 = SessionRegistry 里的普通会话**：`SessionEntry` 有 `subagent_depth` 与持久化的 `parent_session_id`；`spawn_subagent` 靠 `subagent_depth >= 1` 拒绝嵌套；等待靠 250ms 轮询 `is_busy()`。
- **AgentLoop 已有的可复用原语**：`steer_input(expected_turn_id, input)` 把输入并入运行中的回合、在下一次模型请求前由 `drain_active_turn_inputs` 消费（`pending_turn_inputs_` 在同一把锁下与回合状态一起判定，`TurnMismatch` / `NoActiveTurn` 作为原子校验结果）；`submit(UserInput)` 对空闲会话开新回合；`hidden_goal_context` 是「注入一条不显示为用户气泡、不计入可见回合的消息开回合」的先例；`abort()` 只置标记不销毁；`EventDispatcher` 带 seq、订阅与回放，`ThreadService::wait` 已证明 subscribe + condvar 的事件等待可行。
- **工具可见性**：`ToolExecutor::is_allowed(name, &ToolCapabilityPolicy)` 是 schema 过滤与执行前拒绝共用的唯一谓词；`ToolCapabilityPolicy` 目前只有可选的 builtin 允许集与 MCP server 集。
- **专家团**：`ExpertType::Team`、`expert_member`、team lead 提示词已存在，是 Codex `agent_type` 的现成对应物。
- **会话 fork**：`SessionManager::fork_session_to_new_id(retained_prefix, ...)` 写出新会话文件，Web fork 路由随后用 `resume` 把它装进 registry（不自动开回合）。
- **Prompt cache 不变量**：最后一条真实用户消息之前的内容必须内容驱动、同回合逐字节不变（见 CLAUDE.md）。
- **Codex 参照**（`codex-rs/core/src/tools/handlers/multi_agents_v2/`、`agent/control.rs`、`session/input_queue.rs`、`session/multi_agents.rs`）：`send_message` = 入箱不唤醒；`followup_task` = 唤醒空闲目标或在消息边界投递，禁止目标 root；完成结果以 `trigger_turn=false` 投父邮箱；`wait_agent` 等自己邮箱，超时 10s/30s/1h；`interrupt_agent` 禁止 root 与自身；`list_agents` 不做祖先校验；驻留默认 4 含 root，LRU 换出空闲 agent；`fork_turns` 的 `all` 只保留 system/developer/user 消息与 assistant 最终回复；邮箱无容量上限；`RolloutBudget` 未开发完成且默认关闭。

## Goals / Non-Goals

**Goals:**

- 六个 `agent_*` 工具与邮箱语义与 Codex V2 一一对应，差异只在工具名前缀与信封角色。
- 网状模式对现有星型路径零侵入：星型只改显示名与模式枚举，代码路径不变。
- 所有跨会话协作都在进程内经 `SessionRegistry` 完成，不经本机 HTTP。
- 父子 agent 的静态系统提示前缀逐字节一致，让 `fork_turns=all` 的子 agent 能命中 provider 侧的前缀缓存。

**Non-Goals:**

- 不做共享 token 预算、邮箱容量限额、树操作审计（Codex 亦无或未完成）。
- 不复刻 Codex 的加密 payload、多收件人字段、Guardian 审批、OTel 事件、`/morpheus` 保留路径。
- 不为子 agent 提供独立 worktree 隔离（所有 agent 共享 cwd，与 Codex 一致）。
- 不新增 TUI 侧的 agents overview 视图；TUI 首版只加 `/swarm` 与 `/tasks` 的路径/状态显示。
- 不改变星型模式下 `spawn_subagent(wait=true)` 的阻塞语义与 abort 传播行为。

## Decisions

### 1. 蜂群模式升级为会话级三态，通过消息体、命令与 CLI 三个入口写入

`SwarmMode { Off, Star, Mesh }` 存进 `SessionMeta::swarm_mode`（`off` 省略）、`SessionEntry`、`SessionOptions` 与 `AgentLoop`。写入口：消息体 `swarm_mode`（sticky，省略即沿用）、`/swarm` 内置命令（TUI 注册表 + daemon `execute_builtin_command` 白名单 + Web 斜杠目录）、headless `--swarm`。`SessionInfo` 与 `session_status` 事件带出当前值供前端恢复标签态。

- 为什么不保留回合级布尔：子 agent 必须知道自己在网里（角色提示、工具集），而它们收到的从来不是带 metadata 的用户消息。
- 为什么消息体仍是主入口：与现有 Web 数据流零改动地兼容（`chatInputQueue.js` 已经保存并透传 `swarm_mode`），只是取值从布尔扩成枚举，`true` 继续映射为 `star`。
- 有 live 子 agent 时拒绝退出 mesh：由 `MeshTreeControl::has_running_or_pending_children(root)` 判定，消息路由返回 409，命令返回错误。

### 2. 工具可见性用 `ToolCapabilityPolicy` 的模式隐藏集实现互斥

`ToolCapabilityPolicy` 增加 `hidden_builtin_tools`（按模式计算的隐藏集）；`is_allowed` 在现有判定之后再查它。`off`/`star` 隐藏六个 `agent_*`；`mesh` 隐藏 `spawn_subagent`、`wait_subagent` 与全部 thread 工具。模式切换经 `AgentLoop::enqueue_control` 重设 policy，保证在回合之间生效；执行入口复用同一谓词，被隐藏工具的调用返回带模式说明的拒绝文案。

- 替代方案「按模式注册/注销工具」被否决：工具注册在三个启动点一次性完成，`apply_model_to_session` 类的中途切换根本不碰 ToolExecutor（CLAUDE.md 的视觉能力一节记录过同类教训）。

### 3. Agent 身份持久化为四个可选元数据字段，路径解析移植 Codex `AgentPath`

`SessionMeta` 新增 `agent_root_session_id`、`agent_task_name`、`agent_path`、`agent_depth`（空/0 省略）；`parent_session_id` 保留，现有 UI 归属逻辑不动。新增纯逻辑模块 `src/session/agent_path.{hpp,cpp}`：`root()`、`is_root()`、`join(name)`、`resolve(reference)`（以 `/` 开头为 canonical，否则拼到自身之后）、任务名校验 `[a-z0-9_]+` 且排除 `root`/`.`/`..`，可单测。旧子会话在读取层合成 `/root/legacy_<id 前 8 位>`。目标解析顺序与 Codex `resolve_agent_target` 一致：先尝试会话 id，再按路径解析；解析结果必须属于调用者的 root 树，否则返回「目标不在当前 agent 树内」且不泄露其它信息。

- 为什么保留会话 id 作为底层主键：存储、WebSocket、后台任务面板全部以会话 id 工作，路径只是模型侧的稳定名字。

### 4. 邮箱住在 AgentLoop，与 steering 共用同一把锁

新增 `AgentLoop` 内部的跨 agent 邮箱：`std::deque<InterAgentMail>`（信封 + `trigger_turn`）、活动类型（`Mailbox` / `Steer`）与一个 condvar。

- `deliver(mail)` 在持有回合锁的情况下判定：有活动回合且接受投递 → 入箱，随后由 `drain_active_turn_inputs` 在下一次模型请求前与 steering 输入一起消费（先 steering 再邮箱，保持 FIFO）；无活动回合且 `trigger_turn` → 直接 `submit()` 一条以该信封为正文的隐藏内部输入开新回合；无活动回合且不触发 → 留在箱内，`run_agent_with_input` 开始时先 drain。
- 回合刚结束的竞态：回合关闭接受态（`close_if_empty`）与 `deliver` 的判定在同一把锁下，因此一条后续任务要么在关闭前被排进当前回合，要么在关闭后走 `submit()`，恰好一次。
- `steer_input` 入队时同时通知邮箱 condvar，这就是 `agent_wait` 的「被新输入打断」。
- `wait_for_mailbox_activity(deadline, abort_flag)` 供 `agent_wait` 工具阻塞；它只返回活动类型，不出队。

为什么不放在 registry 层：只有 AgentLoop 知道回合是否还接受投递；Codex 也是把 mailbox 放进 `InputQueue` 并在 `active_turn` 锁下判定。

### 5. 信封是 user 角色的隐藏内部输入（有意偏离 Codex）

信封正文：

```
<inter_agent_message>
Message Type: MESSAGE | NEW_TASK | FINAL_ANSWER
Task name: <收件人路径>
Sender: <发件人路径>
Payload:
<正文>
</inter_agent_message>
```

作为 `ChatMessage{role="user"}` 持久化到收件方会话，metadata 带 `inter_agent: {type, sender, recipient}`。`run_agent_with_input` 的 `hidden_goal_context` 泛化为「隐藏内部输入」：不触发 UserPromptSubmit 钩子、不计 `turn_count`、不做技能命令展开、不参与会话摘要；`Message.jsx` / TUI 渲染层按 metadata 画成独立行。

- 为什么不用 Codex 的 assistant 角色：Anthropic 要求 user/assistant 严格交替，DeepSeek 需要回显 `reasoning_content`，连续 assistant 会被合并或拒绝；ACECode 必须跨 provider。
- 追加在对话尾部，不碰任何已缓存前缀。

### 6. 完成投递复用回合结束回调，父空闲不唤醒

`AgentLoop::on_turn_finished(status)` 之后由 `MeshTreeControl::on_child_turn_finished(child_id, status)` 取最后一条非空 assistant 文本（错误/中断时拼状态说明与部分输出），构造 `FINAL_ANSWER` 信封，以 `trigger_turn=false` 投父邮箱；同时 emit 一条带 `unread_mailbox` 计数的 `session_status`，前端据此显示未读数。父在 `agent_wait` 中会被 condvar 唤醒；父空闲则信封留箱，与 Codex control.rs 的 `trigger_turn=false` 一致。

### 7. `MeshTreeControl` 按 root 树串行化目录与驻留

新增 `src/session/mesh_tree_control.{hpp,cpp}`，依赖 `SessionRegistry`、`SessionClient`、`SessionStorage`：

- 每个 `root_session_id` 一个 `TreeState`（互斥锁保护）：`path → session id` 目录、每节点 `AgentStatus`、LRU 驻留链、`pending_slots`。
- `reserve_slot(root)`：循环尝试「预留」→「换出一个 LRU 空闲节点」→「上限错误」，与 Codex `V2Residency::reserve_slot` 同构；换出 = 目标非 root、`!is_busy()`、邮箱无 `trigger_turn` 项 → `registry.destroy(id)`（会话记录本就 canonical 落盘，destroy 只卸载并 join 空闲 worker）→ 状态置 `inactive`。
- `ensure_loaded(target)`：`inactive` → 预留槽 → `registry.resume(id, opts 带身份/模式/专家)` → 触发 `on_agent_loaded` 回调，让 TUI `SubagentHost` 与 Web 面板重新订阅事件。
- spawn 的两阶段：持锁校验名称与预留槽 → 锁外 create/fork + submit 首条 `NEW_TASK` → 持锁提交目录项；失败回滚槽并 destroy 半成品会话。
- daemon 启动时按 `agent_root_session_id` 扫描元数据重建目录，全部 `inactive`。

状态枚举 `pending_init / running / idle / interrupted / errored / inactive / not_found` 从 `BusyChanged`/`Done` 事件的 `outcome` 与 `is_aborting()` 推导，`agent_list`、回执、`session_status`、面板共用。

### 8. `fork_turns` 走现有 fork 流程加过滤器

`all`：取父会话已持久化消息，保留 `role=user` 且非隐藏内部输入、非信封的消息，以及 `role=assistant` 且无 `tool_calls`、非压缩摘要的最终回复；丢弃 tool、system、meta、压缩检查点、信封。正整数 N：从尾部数 N 条用户消息为界。产出经 `fork_session_to_new_id` 写盘后 `resume` 进 registry，与 Web fork 路由同一套路。`none`：直接 `create`。子会话不继承 file checkpoint（既有 fork 决定）。

- 与 Codex 的差异：Codex 还保留 developer 消息；ACECode 的项目指令、记忆等由每个会话按 cwd 自行构建，不需要复制。

### 9. 提示词移植到独立模块并放在可变上下文段

新增 `src/prompt/mesh_swarm_prompts.{hpp,cpp}`：从 `codex-rs/core/src/session/multi_agents.rs` 与 `context/multi_agent_mode_instructions.rs` 移植 root 角色提示、子 agent 角色提示、主动委派文案、等待提示与并发槽提示，把工具名换成 `agent_*`，去掉 `functions.exec` 相关段落（ACECode 无该机制）。`build_mesh_swarm_context_prompt(mode, identity, capacity)` 在 `agent_loop.cpp` 现有 `build_swarm_mode_context_prompt` 的同一位置注入，内容只由（模式、路径、并发上限、工具可用性）决定，同回合逐字节稳定。

- 身份不进静态系统提示：这样父子 agent 的 `# Environment` 与工具 schema 完全一致，provider 前缀缓存可跨 agent 复用。

### 10. 工具命名与结果形态

`agent_spawn` / `agent_list` / `agent_send_message` / `agent_followup_task` / `agent_wait` / `agent_interrupt`，沿用 ACECode `名词_动作` 惯例（`file_read`、`memory_write`），动词保持 Codex 原样以保留模型先验。六个工具 `is_read_only=true`（与 `spawn_subagent` 同理，副作用由子会话自己的权限门把关），结果带 `summary.icon="agent"` 让前端沿用「调用了 N 个智能体」分组。`agent_spawn` 结果与 `metadata.subagent_session_id` 兼容现有面板发现通道，另加 `agent_path`。工具描述文案逐条对照 `multi_agents_spec.rs` 的 V2 描述翻译改写。

### 11. Web 与 TUI 的最小改动面

- Web：`normalizeComposerPayload` 输出 `swarm_mode: 'star' | 'mesh'`；`ChatView` 的 `composerSwarmMode` 由布尔改枚举并从 `SessionInfo.swarm_mode` 恢复；`InputBar` 两个 `menuitemradio`；`ComposerSessionControls` 芯片文案按模式；`chatInputQueue.js` 原样透传字符串；`subagentTasks.js` 归一化新增 `agentPath`、`agentDepth`、`agentStatus`、`unreadMailbox`，面板按 `agentPath` 缩进与分组；`Message.jsx` 识别 `metadata.inter_agent` 渲染独立行；`sessionTranscript.js` 的摘要与折叠投影排除信封。
- TUI：`/swarm` 命令；`SubagentHost` 在 `on_agent_loaded` 时重订阅并以路径为标题；`/tasks` 输出路径与状态；确认/提问浮层的来源标签使用路径。
- headless：`--swarm`，透传到 `SessionOptions::swarm_mode`。

### 12. 星型模式保持字面不变

`build_swarm_mode_context_prompt` 只在 `mode == Star` 时调用，文案不改；`spawn_subagent` 的深度规则不改；星型子代理的 `SessionOptions::swarm_mode` 为 `off`。`add-interrupt-subagent-tool` 提案不在本变更内实施。

## Risks / Trade-offs

- **[`fork_turns=all` 默认使 token 成本随 agent 数线性增长]** → 静态前缀跨 agent 一致以最大化 provider 缓存命中；提示词中说明 `fork_turns` 的取舍；用户可用 `none` 或整数。
- **[换出用 `destroy` 实现，事件订阅随之消亡]** → `on_agent_loaded` 回调统一重订阅；Web 面板已对未知 busy 会话的 `session_status` 帧做 refetch；只读查看走磁盘不触发恢复。
- **[user 角色信封与真实用户输入难以区分]** → metadata 为唯一判据；隐藏内部输入不计回合、不入摘要、不触发钩子；测试覆盖回放渲染。
- **[busy→idle 竞态导致后续任务丢失或双投]** → 判定与回合关闭在同一把锁下（复用 steering 的 `close_if_empty` 路径），用状态机测试覆盖三种时序。
- **[`agent_wait` 可长达 1 小时的工具调用]** → 前端工具行已支持长时运行；等待可被用户 steer 与 abort 打断；检查 `AgentLoopDoomGuard` 是否把重复的 `agent_wait` 调用判为循环，需要时按工具名豁免。
- **[连续 provider 请求中 `FINAL_ANSWER` 与用户消息穿插]** → 信封作为 user 角色与真实用户消息合法相邻；无需合并。
- **[专家团成员在深层被任意指定]** → 仍只接受团内已选成员，非法 id 直接拒绝。
- **[thread 工具在 mesh 下不可用影响既有工作流]** → 这是有意的互斥；`/swarm off` 后立即恢复。

## Migration Plan

1. 先落身份字段、模式枚举、`AgentPath` 与 `MeshTreeControl` 的纯逻辑及状态机测试，不注册新工具。
2. 接入 AgentLoop 邮箱、投递、完成回调、驻留换出/恢复，用并发测试证明恰好一次与不超上限。
3. 注册六个工具与隐藏集，接入提示词与 `fork_turns`。
4. Web/TUI/headless 入口与面板、transcript 渲染。
5. 文档（`docs/subagents.md`、`docs/daemon-api.md`、`docs/help/swarm.html`、`CLAUDE.md`）与 i18n。

回滚：不选择网状模式即回到今天的行为；`swarm_mode` 字段向后兼容；新增元数据字段旧版本忽略。`add-agent-tree-collaboration` 目录只存在于主工作树且未纳入 git，应在主工作树里移入 `openspec/changes/archive/` 并注明由本变更取代。

## 实现记录（与上文设计的差异，以代码为准）

实现时用户拍板「全按 Codex V2」，上文几处设计据此改动：

- **身份字段只加两个**：`SessionMeta.swarm_mode` 与 `agent_path`。根会话 id 直接用 `parent_session_id`（所有网状子 agent 扁平挂在根下），深度与任务名从路径推出；不合成 legacy 路径。
- **`MeshTreeControl` → `host/session_host/mesh/MeshAgentService`**：worker / TUI / headless 各一份，工具与事件监听只捕获 `weak_ptr`；树目录持久化在根会话目录的 `mesh_agents.json`，重启后据此重建，不扫全部 meta。
- **模式切换不经 `enqueue_control`**：AgentLoop 在每回合捕获请求源时从 SessionManager 读模式与路径（会话元数据是唯一事实源），切换天然只影响下一回合。
- **邮箱投递**：信封一律先入箱，不直接 `submit()`；空闲时 `trigger_turn` 经 `mailbox_wake` 任务唤醒，唤醒前再查一次是否仍有触发邮件（已被运行中回合并入则不多跑一回合）。回合第一次模型请求前也会并入邮箱（Codex 要等下一次采样，属有意偏离）。
- **状态与列表按 Codex**：`agent_list` 只列已加载的 agent，状态取 Codex 枚举（`pending_init / running / interrupted / {completed} / {errored} / not_found`），没有 `idle` / `inactive`；被换出的 agent 只在内部目录与 UI 快照里可见。
- **完成回报按 Codex**：completed = 该回合最终回答，errored = `Agent errored: …` + 下一步提示，**interrupted 不回报**；父 agent 未加载时暂存到恢复（根不在则丢弃并记日志）。
- **未做**：未读邮件计数（Codex 无）、面板按深度缩进、来源标签改用完整路径、采样中途因新邮件抢占输出、`<subagents>` 名册注入、`agent_interrupt` 后的 `<turn_aborted>` 标记。
- **Web 芯片**：芯片 = 服务端模式（messages 快照 / `session_updated{swarm_mode}`）+ 未提交的本地选择，只有二者不同才随消息提交 `swarm_mode`，避免普通消息把 `/swarm` 刚切的模式改回去；信封在 transcript 摄入时转成系统提示行。

## Open Questions

- 移植的角色提示词具体措辞（在实现时对照 Codex 源文本逐段改写，不影响规格与任务拆分）。
- `reasoning_effort` 的透传取决于当前模型档案是否暴露该设置；规格已规定不支持时在结果中注明忽略。
