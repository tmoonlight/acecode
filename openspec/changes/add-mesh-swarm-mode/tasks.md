## 1. 模式枚举、身份字段与路径

- [ ] 1.1 新增 `SwarmMode { Off, Star, Mesh }` 及字符串互转，扩展 `SessionMeta`（`swarm_mode`、`agent_root_session_id`、`agent_task_name`、`agent_path`、`agent_depth`，空值省略）、`SessionOptions`、`SessionEntry`、`SessionInfo`；验证 `tests/session/session_storage_test.cpp` 新增的往返用例通过且旧 meta 读取为 `off`
- [ ] 1.2 新增 `src/session/agent_path.{hpp,cpp}`：`root`/`is_root`/`join`/`resolve`/任务名校验/legacy 路径合成；验证 `tests/session/agent_path_test.cpp` 覆盖嵌套、相对解析、非法名、`root`/`.`/`..` 拒绝
- [ ] 1.3 `SessionRegistry::make_entry_locked` 从 `SessionOptions` 与恢复的 meta 写入模式与身份并透传到 `SessionManager`/`AgentLoop`；验证 `tests/tool/spawn_subagent_tool_test.cpp` 风格的 resume 用例恢复出相同路径与模式
- [ ] 1.4 消息路由 `routes_sessions.cpp` 把 `swarm_mode` 解析为 `"star"|"mesh"|false|true(=star)`，非法值 400，接受后更新会话模式并写入用户消息 metadata，省略时沿用会话模式；验证 `web_server_smoke_test.cpp` 覆盖四种取值与 sticky 行为

## 2. 工具互斥与模式切换

- [ ] 2.1 `ToolCapabilityPolicy` 增加 `hidden_builtin_tools` 并在 `ToolExecutor::is_allowed` 中生效；验证 `tests/tool/` 新增用例：隐藏集内工具从 schema 消失且执行被拒
- [ ] 2.2 按模式计算隐藏集（`off`/`star` 隐藏 `agent_*`；`mesh` 隐藏 `spawn_subagent`、`wait_subagent` 与全部 thread 工具），模式切换经 `enqueue_control` 重设 policy；验证 AgentLoop 测试中切换后下一回合的工具列表变化、当前回合不变
- [ ] 2.3 被隐藏工具的调用返回带模式说明的拒绝文案（星型工具指向 `agent_spawn`，网状工具说明仅在网状可用）；验证单测断言文案
- [ ] 2.4 `MeshTreeControl::has_running_or_pending_children` 作为退出 mesh 的门：消息路由返回 409，`/swarm` 返回命令错误；验证 smoke test 与命令测试

## 3. AgentLoop 邮箱与投递

- [ ] 3.1 在 `AgentLoop` 增加跨 agent 邮箱（信封队列、`Mailbox`/`Steer` 活动、condvar），`deliver(mail)` 在回合锁下判定「排入当前回合 / 开新回合 / 留箱」；验证 `tests/agent_loop_*` 新增用例覆盖运行中、空闲触发、空闲不触发三种路径
- [ ] 3.2 `drain_active_turn_inputs` 在 steering 之后消费邮箱，`run_agent_with_input` 开始时先 drain；验证信封出现在下一次模型请求且顺序为 steering 先、邮箱后
- [ ] 3.3 busy→idle 竞态：`close_if_empty` 与 `deliver` 同锁，后续任务恰好一次；验证状态机测试三种时序（关闭前入箱、关闭后入箱、同时）均恰好投递一次
- [ ] 3.4 `steer_input` 入队时通知邮箱 condvar；`wait_for_mailbox_activity(deadline, abort_flag)` 返回活动类型不出队；验证等待被邮箱、steer、abort、超时四种方式结束
- [ ] 3.5 把 `hidden_goal_context` 泛化为隐藏内部输入（不触发 UserPromptSubmit、不计 `turn_count`、不做技能展开、不入摘要），信封以 `role=user` + `metadata.inter_agent` 持久化；验证 `RequestPrefixIsByteStableAcrossIterationsInATurn` 仍通过且新增用例断言 turn_count 不变
- [ ] 3.6 检查 `AgentLoopDoomGuard` 对重复 `agent_wait` 调用的判定，必要时按工具名豁免；验证连续 5 次同参 `agent_wait` 不触发守卫

## 4. MeshTreeControl 与驻留

- [ ] 4.1 新增 `src/session/mesh_tree_control.{hpp,cpp}`：按 root 的 `TreeState`（目录、状态、LRU、pending 槽）、目标解析（会话 id 优先、再路径、必须同根）、名称冲突检测；验证纯逻辑单测覆盖解析、冲突、跨树拒绝且不泄露信息
- [ ] 4.2 `reserve_slot`：预留 → 换出 LRU 空闲（非 root、`!is_busy()`、邮箱无触发项）→ 上限错误；换出走 `registry.destroy` 并置 `inactive`；验证测试：3 空闲 + spawn 换出最久未用者、全部运行时报错且不创建会话
- [ ] 4.3 `ensure_loaded`：`inactive` → 预留槽 → `registry.resume`（带身份、模式、专家）→ `on_agent_loaded` 回调；验证恢复后路径/历史/模式一致，槽满时返回上限错误且目标保持 `inactive`
- [ ] 4.4 spawn 两阶段（持锁校验与预留 → 锁外 create/fork + 首条 `NEW_TASK` → 持锁提交；失败回滚并清理半成品会话）；验证并发测试：两方争最后一个槽恰好一个成功、失败路径不留幽灵节点
- [ ] 4.5 状态推导（`pending_init/running/idle/interrupted/errored/inactive/not_found`）来自 `BusyChanged`/`Done` 的 outcome 与 `is_aborting()`；验证每种终态映射用例
- [ ] 4.6 完成回调：`on_child_turn_finished` 构造 `FINAL_ANSWER`（正常 = 最后一条 assistant 文本；错误/中断 = 状态说明 + 部分输出）以 `trigger_turn=false` 投父邮箱并 emit 带 `unread_mailbox` 的 `session_status`；验证父空闲不被唤醒、父等待被唤醒、错误结束时信封含状态
- [ ] 4.7 daemon 启动扫描 `agent_root_session_id` 重建目录为 `inactive`；验证重启测试中 `agent_list` 按原路径列出且无自动恢复
- [ ] 4.8 配置 `swarm.mesh.max_concurrent_agents`（默认 4，最小 2）与 `swarm.mesh.{min,default,max}_wait_timeout_ms`（10000/30000/3600000）；验证 `tests/config/` 读取、默认值与越界校验

## 5. 六个协作工具

- [ ] 5.1 新增 `src/tool/mesh_agent_tools.{hpp,cpp}` 与 `MeshAgentToolDeps`（延迟回填，与 `SubagentToolDeps` 同法），三处注册（`worker.cpp`、`main.cpp`、`headless_runner.cpp`）；验证 `--list-tools` 与工具注册测试列出六个工具
- [ ] 5.2 `agent_spawn(task_name, message, agent_type?, fork_turns?, model?, reasoning_effort?)`：校验、预留、创建/fork、首条 `NEW_TASK`、立即返回路径/会话 id/状态，结果带 `summary.icon="agent"` 与 `metadata.subagent_session_id`/`agent_path`；验证二级派生成功、重名与非法名拒绝、未知模型拒绝、不阻塞
- [ ] 5.3 `agent_list(path_prefix?)` 返回含 root 与 `inactive` 节点的有序目录；验证兄弟可见与前缀过滤
- [ ] 5.4 `agent_send_message(target, message)`：入箱不唤醒，可发 root，`inactive` 先恢复；验证运行中目标下一请求可见、空闲 root 保持空闲、恢复路径
- [ ] 5.5 `agent_followup_task(target, message)`：禁止 root，空闲/完成/`inactive` 开新回合，运行中边界投递；验证三种目标状态与 root 拒绝文案
- [ ] 5.6 `agent_wait(timeout_ms?)`：钳制 [10s, 1h] 默认 30s，返回三种固定文案；验证邮箱唤醒、steer 打断、超时、abort 结束且不影响其它 agent
- [ ] 5.7 `agent_interrupt(target)`：禁止 root 与自身，返回中止前状态，空闲目标不报错；验证运行中中止后仍可接后续任务
- [ ] 5.8 `agent_type` → 专家团成员映射（任意层级可用，非法 id 列出可选成员，未绑定专家团报错）；验证以现有 team expert fixture 的用例
- [ ] 5.9 六个工具的描述文案对照 Codex `multi_agents_spec.rs` V2 描述改写为 `agent_*` 名；验证单测断言关键句（相对/canonical 路径说明、不触发回合、禁止 root）

## 6. fork_turns 与提示词

- [ ] 6.1 实现 `fork_turns` 过滤器（保留真实用户消息与无 tool_calls 的 assistant 最终回复；丢弃 tool/system/meta/压缩/信封；N = 最近 N 个用户回合）并接 `fork_session_to_new_id` + `resume`；验证 `none`/`all`/`"2"`/非法值四个用例
- [ ] 6.2 新增 `src/prompt/mesh_swarm_prompts.{hpp,cpp}`：移植 root/子 agent 角色提示、主动委派文案、等待与并发槽提示并改用 `agent_*` 名；`build_mesh_swarm_context_prompt` 在现有 swarm 引导同位置注入，`star` 仍走 `build_swarm_mode_context_prompt`；验证 `tests/prompt/system_prompt_test.cpp`：root/子提示内容、静态前缀逐字节不变、同回合两次请求逐字节相同
- [ ] 6.3 网状子会话标题 = `task_name`，跳过 `maybe_start_auto_title`；验证 registry 测试中派生后无标题生成尝试且标题为任务名

## 7. Web/Desktop 入口与面板

- [ ] 7.1 `InputBar.jsx` `+` 菜单改为「蜂群模式（星型）」「蜂群模式（网状）」两个 `menuitemradio`，`ComposerSessionControls.jsx` 芯片按模式显示文案与关闭；`ChatView.jsx` 的 `composerSwarmMode` 改枚举并从 `SessionInfo.swarm_mode` 恢复，`normalizeComposerPayload` 输出字符串；验证 `composerSessionControlsArchitecture.test.js`、`chatInputQueue.test.js` 更新用例通过
- [ ] 7.2 `chatInputQueue.js` 透传字符串取值并保留旧布尔兼容；验证队列重建/重试/引导保留模式
- [ ] 7.3 `subagentTasks.js` 归一化 `agentPath`、`agentDepth`、`agentStatus`、`unreadMailbox`，分组按统一状态、排序按路径；`SubagentPanel.jsx` 按深度缩进、显示路径/状态/最后一条消息/tokens/耗时，`inactive` 可只读查看；`useSubagentTasks.js` 对恢复后的会话重新 retain；验证 `subagentTasks.test.js` 新增缩进/分组/未读用例
- [ ] 7.4 ChatView 标题栏后台任务入口显示 root 未读数并在 root 回合开始后清零；验证前端测试
- [ ] 7.5 `Message.jsx` 识别 `metadata.inter_agent` 渲染独立行（类型 + 发件人路径），`sessionTranscript.js` 摘要与折叠投影排除信封；验证渲染测试与 resume 回放一致性用例
- [ ] 7.6 `web/scripts/i18n-en-overrides.mjs` 增加 `蜂群模式（星型）`/`蜂群模式（网状）`及提示文案的英文（`Swarm mode (Star)` / `Swarm mode (Mesh)`），运行 `pnpm i18n:catalog`；验证 `pnpm i18n:audit` 无缺失

## 8. TUI 与 headless

- [ ] 8.1 注册 `/swarm [star|mesh|off]`（TUI 命令注册表、daemon `execute_builtin_command`、Web 斜杠目录 `commands_handler.cpp`），无参显示当前模式，非法参数列出合法值；验证命令测试三端
- [ ] 8.2 `SubagentHost` 处理 `on_agent_loaded` 重订阅，任务标题优先路径，`/tasks` 显示路径与统一状态，`/tasks abort` 对网状 agent 等价于中止回合；验证 `tests/tui/subagent_host_test.cpp` 新用例
- [ ] 8.3 确认/提问浮层与 Web 全局弹窗的来源标签使用 canonical 路径，任意深度冒泡到 root；验证二级子 agent 的权限与提问路由测试
- [ ] 8.4 headless `--swarm star|mesh`（缺省 off，非法值 exit 64）透传到 `SessionOptions::swarm_mode`；验证 `tests/headless/` 参数解析与帮助文本用例
- [ ] 8.5 星型子代理的 `SessionOptions::swarm_mode` 为 `off`，`spawn_subagent` 深度规则与 `build_swarm_mode_context_prompt` 文案不变；验证现有 `SwarmModeContextIsRequestLocalAndPolicyGated` 与 spawn 深度用例原样通过

## 9. 文档与收尾

- [ ] 9.1 更新 `docs/subagents.md`（两种模式、六个工具、邮箱与驻留语义）、`docs/daemon-api.md`（`swarm_mode` 取值、`SessionSummary` 新字段、`session_status` 未读字段）、`docs/help/swarm.html` 文案与 `CLAUDE.md` 子代理段落；验证文档中的工具名与代码注册名一致
- [ ] 9.2 单元测试全部使用中文注释并写明触发场景与期望行为；验证 `cmake --build` 与 `acecode_unit_tests` 全绿，`pnpm test`、`pnpm build` 通过
- [ ] 9.3 运行 `openspec validate add-mesh-swarm-mode --strict` 与 `scripts/code_quality_check.bat`；验证无报错，并用 `git add -f openspec/changes/add-mesh-swarm-mode` 把变更文档随分支提交（`openspec/` 在 `.gitignore` 中）
