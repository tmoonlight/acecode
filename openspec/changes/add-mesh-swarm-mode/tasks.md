## 1. 模式枚举、身份字段与路径

- [x] 1.1 新增 `SwarmMode { Off, Star, Mesh }` 及字符串互转，扩展 `SessionMeta`（实际只加 `swarm_mode`、`agent_path` 两个字段，见 design.md「实现记录」）、`SessionOptions`、`SessionInfo`；验证 meta 往返与旧 meta 读取为 `off`
- [x] 1.2 新增 `src/domain/session/agent_path.{hpp,cpp}`：`root`/`is_root`/`join`/`resolve`/任务名校验（不合成 legacy 路径）；验证 `tests/session/agent_path_test.cpp` 覆盖嵌套、相对解析、非法名、`root`/`.`/`..` 拒绝
- [x] 1.3 `SessionRegistry` 从 `SessionOptions` 与恢复的 meta 写入模式与身份（`apply_swarm_identity`）；验证 `mesh_agent_service_test.cpp::RestartRebuildsTreeFromIndexAndRestoresOnDemand` 恢复出相同路径与模式
- [x] 1.4 消息路由 `routes_sessions.cpp` 把 `swarm_mode` 解析为 `"star"|"mesh"|"off"|true(=star)|false`，非法值 400，接受后更新会话模式并写入用户消息 metadata，省略时沿用会话模式；验证 `web_server_smoke_test.cpp::PostMessageQueuesInputInDaemonSession`

## 2. 工具互斥与模式切换

- [x] 2.1 `ToolCapabilityPolicy` 增加 `hidden_builtin_tools` 并在 `ToolExecutor::is_allowed` / 执行入口生效
- [x] 2.2 按模式计算隐藏集（`off`/`star` 隐藏 `agent_*`；`mesh` 隐藏 `spawn_subagent`、`wait_subagent` 与全部 thread 工具）；AgentLoop 每回合从 SessionManager 读模式（不经 `enqueue_control`），切换只影响下一回合；验证 `characterization_lifecycle_test.cpp::SwarmModeFollowsSessionContextBetweenTurns`
- [x] 2.3 被隐藏工具的调用返回带模式说明的拒绝文案；验证 `mesh_swarm_domain_test.cpp::ToolFamiliesAreMutuallyExclusive`
- [x] 2.4 退出 mesh 的守卫（`MeshAgentService::tree_busy_reason`，经 `SessionRegistry::set_swarm_mode_guard`）：消息路由 409，`/swarm` 返回错误；网状子 agent 不能改模式；验证 `mesh_agent_service_test.cpp::LeavingMeshIsRefusedWhileAgentsRun`

## 3. AgentLoop 邮箱与投递

- [x] 3.1 `engine/agent/mailbox/AgentMailbox`：信封一律入箱，`trigger_turn` 在空闲时经 `mailbox_wake` 任务唤醒；验证 `agent_loop_mesh_mailbox_test.cpp` 覆盖运行中、空闲触发、空闲不触发
- [x] 3.2 `TurnRunner::drain_inputs` 先 steering 后邮箱，回合第一次请求前也并入；验证 `QueueOnlyMailWaitsForNextTurn`、`MailArrivingMidTurnJoinsTheNextRequest`
- [x] 3.3 回合以最终回答收尾时只排队的邮件留到下一回合，已被并入的触发邮件不再多跑一回合；验证 `MailArrivingMidTurnJoinsTheNextRequest`（触发 / 非触发两种）
- [x] 3.4 steer 通知邮箱；`wait_for_mailbox_activity` 不出队；验证 `agent_mailbox_test.cpp` 与 `WaitForMailboxActivityOutcomes`（邮件、steer、abort、超时）
- [x] 3.5 信封以 `role=user` + `metadata.inter_agent` 持久化；NEW_TASK 计为真实用户消息，MESSAGE / FINAL_ANSWER 是内部上下文；不计可见回合、不入摘要、不触发 UserPromptSubmit；验证 `RequestPrefixIsByteStableAcrossIterationsInATurn` 仍通过
- [x] 3.6 `AgentLoopDoomGuard` 每次模型响应开头 `begin_model_turn()` 清空重复窗口，跨回合的重复 `agent_wait` 不会被拦，无需豁免

## 4. MeshAgentService 与驻留

- [x] 4.1 `host/session_host/mesh/MeshAgentService`（取代设计稿的 `MeshTreeControl`）：按根的 `Tree`、目标解析（会话 id 优先、再路径）、重名检测；验证 `mesh_agent_service_test.cpp`
- [x] 4.2 `reserve_slot`：预留 → 换出 LRU（已有结果、无待办、空邮箱）→ 上限错误；验证 `FullTreeEvictsLeastRecentlyActiveAgentAndRestoresOnDemand`、`ThreadLimitReachedWhenNoAgentCanBeEvicted`
- [x] 4.3 `ensure_loaded`：未加载 → 预留槽 → `registry.resume` → 冲刷暂存邮件 → `on_agent_loaded`；验证同上与 `RestartRebuildsTreeFromIndexAndRestoresOnDemand`
- [x] 4.4 spawn 三阶段（持锁占路径 → 锁外创建 + fork 历史 → 持锁发布并投递 NEW_TASK；失败回滚路径与槽）
- [x] 4.5 状态按 Codex 枚举：`pending_init / running / interrupted / completed / errored / not_found`（未加载 = not_found，不列入 agent_list）
- [x] 4.6 完成回报：completed = 该回合最终回答，errored = `Agent errored: …` + 下一步提示，interrupted 不回报；`trigger_turn=false` 不唤醒父；验证 `SpawnRunsChildAndRoutesFinalAnswerToIdleRoot`、`ErroredChildReportsAgentErroredToParent`、`InterruptAbortsRunningChildWithoutFinalAnswer`
- [x] 4.7 重启后按根会话目录的 `mesh_agents.json` 重建树（不扫全部 meta）；验证 `RestartRebuildsTreeFromIndexAndRestoresOnDemand`
- [x] 4.8 配置 `swarm.mesh.{max_concurrent_agents, min/default/max_wait_timeout_ms, expose_model_overrides}`；验证 `tests/config/config_swarm_test.cpp`

## 5. 六个协作工具

- [x] 5.1 `host/session_host/tools/mesh_agent_tools.{hpp,cpp}`，三处注册（worker、TUI、headless；headless 先注册后 `rebind_mesh_agent_tools`）
- [x] 5.2 `agent_spawn`：输出 `{"task_name": 路径}`，metadata `subagent_session_id` / `agent_path`；验证 `ToolsMatchCodexArgumentAndOutputContracts`、`SpawnValidatesCallerAndArguments`
- [x] 5.3 `agent_list(path_prefix?)`：只列已加载 agent（Codex 原样），根在前；验证 `NestedChildReportsToItsParentNotRoot`
- [x] 5.4 `agent_send_message`：入箱不唤醒，可发根，未加载目标先恢复；验证 `FollowupWakesIdleChildWhileSendMessageOnlyQueues`
- [x] 5.5 `agent_followup_task`：禁止根，空闲唤醒，运行中边界投递；验证同上
- [x] 5.6 `agent_wait(timeout_ms?)`：低于最小值夹取并注明，超过最大值报错，返回 Codex 三种文案；验证 `ToolsMatchCodexArgumentAndOutputContracts`
- [x] 5.7 `agent_interrupt`：禁止根与自身，返回中断前状态；验证 `InterruptAbortsRunningChildWithoutFinalAnswer`
- [x] 5.8 `agent_type` → 团队专家成员（未绑定团队专家报错，未知成员 `unknown agent_type`）
- [x] 5.9 描述文案对照 Codex V2 改为 `agent_*` 名

## 6. fork_turns 与提示词

- [x] 6.1 `fork_turns` 过滤器（`session/agent_fork_history`）：从父会话的有效模型历史保留用户消息与最终回答；验证 `mesh_swarm_domain_test.cpp::KeepsUserTurnsAndFinalAnswersOnly`
- [x] 6.2 `engine/prompt/mesh_swarm_prompts`：根 / 子 agent 角色提示与主动委派文案，注入在可变上下文段，静态前缀不变
- [x] 6.3 网状子会话标题 = `task_name`，投递不走 `send_input` 所以不触发自动标题

## 7. Web/Desktop 入口与面板

- [x] 7.1 `InputBar.jsx` 两个 `menuitemradio`，`ComposerSessionControls.jsx` 芯片按模式；`ChatView.jsx` 芯片 = 服务端模式 + 本地选择，只有不同才提交；验证 `composerSessionControlsArchitecture.test.js`、`swarmMode.test.js`
- [x] 7.2 `chatInputQueue.js` 透传 `"star"|"mesh"|"off"` 并兼容旧布尔；验证 `chatInputQueue.test.js`
- [~] 7.3 `subagentTasks.js` 归一化 `agentPath`，卡片显示路径，`agent_spawn` 打开面板、`agent_spawn`/`agent_wait` 结束时刷新；**未做**按深度缩进、未读计数
- [ ] 7.4 标题栏后台任务入口的根未读数（Codex 无对应物，未做）
- [x] 7.5 信封在 transcript 摄入时转成系统提示行（`web/src/lib/interAgentMessage.js` + `systemNotice` 四个代码），不进用户气泡；验证 `interAgentMessage.test.js`
- [x] 7.6 i18n：`蜂群模式（星型）/（网状）` 等文案已加英文并重新生成目录；`pnpm i18n:audit` 通过

## 8. TUI 与 headless

- [x] 8.1 `/swarm [star|mesh|off]`（TUI `swarm_mode_command.cpp`、daemon 内置命令、Web 斜杠目录）；验证 `swarm_command_test.cpp`、`commands_handler_test.cpp`
- [x] 8.2 `SubagentHost` 在 `on_agent_loaded` 时登记并订阅，侧栏以路径为显示名；TUI 显示信封为系统行（`inter_agent_display_text`，实时与回放）
- [~] 8.3 权限 / 提问的来源标签沿用子会话标题（= task_name），未改成完整路径
- [x] 8.4 headless `--swarm star|mesh|off`（非法值 usage error）；验证 `headless_options_test.cpp::ParsesAndValidatesSwarmMode`
- [x] 8.5 星型路径不变；现有 spawn / swarm 用例原样通过

## 9. 文档与收尾

- [x] 9.1 `docs/subagents.md` §9、`docs/daemon-api.md`（`swarm_mode` 取值、409、`session_updated{swarm_mode}`、列表字段）、帮助页 `swarm`、`CLAUDE.md`
- [x] 9.2 单元测试中文注释；`acecode_unit_tests` fast profile 全绿，`pnpm test` / `pnpm build` 通过
- [x] 9.3 `openspec validate add-mesh-swarm-mode --strict` 通过；变更文档随分支强制加入版本库（`openspec/` 被 ignore）
- [ ] 9.4 帮助页截图 AD-03 仍是旧菜单（只有一个「蜂群模式」），需补拍

## 10. 蜂群通知穿插时的已处理摘要修复（2026-10-03）

- [x] 10.1 复现通知拆分已处理摘要的问题，修正完成回合投影，保留通知正文、活动详情与原有非通知边界
- [x] 10.2 增加持久耗时、实时事件、历史加载及 transcript_replace 回归，覆盖 task_complete 与关闭自动折叠
- [x] 10.3 运行定向测试、Web 全量测试、构建、OpenSpec 严格验证与差异检查，记录结果

验证记录：修复前定向用例复现同一回合 4 条已处理摘要（期望 1 条）；修复后 `transcriptProjection.test.js`、`sessionTranscript.test.js` 通过。`pnpm test` 通过（2981 项），`pnpm build` 及正则兼容检查通过，`openspec validate add-mesh-swarm-mode --strict`、`git diff --check` 通过。Headless Edge 使用实际 TranscriptItems / Message / ActivityLine 组件验证 1280px 浅色折叠态与 720px 深色展开态：1 条已处理、3 条独立通知、全部通知正文和 4 条活动详情可展开，无横向溢出及脚本异常。未重新编译或替换已安装 Desktop。
