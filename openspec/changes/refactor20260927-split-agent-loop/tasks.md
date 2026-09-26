# Tasks: refactor20260927-split-agent-loop

> 执行前必读 `refactor20260927-restructure-src-layers/design.md` §6「提交与协作约定」,以及本变更 design.md 的 §7「不变量清单」。
>
> - 提交前缀:`refactor20260927(agent-loop/<任务编号>): …`。
> - 开工前在任务行末尾追加 `〔认领: <代理名> <日期>〕`,单独提交到 master。
> - **第 2–4 组整体串行**:同一时刻只允许一个任务修改 `src/engine/agent/` 下正在拆的文件。
> - 原计划的 A-15 / A-16 是行为变更,已移到 adopt-ownership-conventions,编号为 O-10 / O-11。
> - 路径相对 `src/engine/agent/`;第 1 组在目录搬迁之前执行,使用旧路径 `src/agent_loop.*`。
> - 每个任务的验证都包括:用例清单不变(外加新增用例);Linux CI 与 Windows 本地构建 `acecode_unit_tests` 并全量通过;新文件不超过 1000 行;提交说明附上触碰到的不变量编号(design.md §7)。

## 1. 前置(可在 restructure 的 Phase 0 期间做,不依赖搬迁)

- [ ] 1.1 【P0-10】【子】删除 agent_loop 的死代码与无用参数。〔认领: Codex-root 2026-09-27〕
  - 删除 hpp:`786-797`(`emit_progress_tick` 声明,没有定义)、`653-655`(过时注释)、`545/554-556`(`run_agent` / `run_agent_with_display` 声明);
  - 删除 cpp:`2857-2868`(这两个函数的实现)、`431-434`(匿名 `is_hidden_goal_context_message`,实际调用的是 web:: 版本);
  - 删除 `build_tool_context` 的 3 个无用参数(`emit_progress`、`doom_guard`、`doom_guard_mu`)、`execute_tool_calls` 的 `turn_timing_status` 参数、`ToolCallEntry::is_read_only`。
  - **必须在 restructure 的 P2-08 移动 agent_loop 之前合入。**
  - 验证:diff 只含删除;全仓 grep(含 `.mm`)确认没有其它引用;`acecode_unit_tests --gtest_filter=*AgentLoop*:HookAgentLoop*:SessionRegistry*` 通过。
- [ ] 1.2 【P0-11】【子】agent_loop 表征测试:只锁定现状,不修 bug。在 `tests/agent_loop/` 下新增以下 11 组用例,每组都写中文注释,说明触发场景、期望行为和回归时的表现:〔认领: Codex-raii 2026-09-27〕
  1. 权限门黄金序列:{模式 × 工具类别 × 规则 × goal × headless × hook × 用户决策},断言返回文案、审计条目序列、PermissionRequest/Resolved 序列、会话授权副作用;
  2. apply_patch 多路径权限门:逐路径受 exec 规则保护;Plan 模式要求全部是计划文件;确认只弹一次;
  3. PreToolUse 拒绝时不发 ToolStart/ToolEnd 实时事件,只写两条轨迹;
  4. 并行只读批次 + hooks;
  5. run_shell 实时路径:hook 改写命令、全量输出、不发 BusyChanged(true)、落盘 `!cmd` + tool_result;
  6. `stop_hook_active_` 跨回合残留的现状;
  7. swarm_mode 在回合之间复位;
  8. computer-use 租约释放;
  9. 静态 system prompt 在压缩初始上下文与主请求中逐字节一致;
  10. 压缩请求的消息体是原始历史,不经过 model_facing;
  11. 跨 loop 交接的 AB-BA 并发(LR-M4)。
  - 750ms / 500ms 的节流用可注入的时钟,避免用例不稳定。
  - **必须在 restructure 的 P2-08 之前合入**:P2-08 会把 `tests/agent_loop/` 改名为 `tests/agent/`。
  - 验证:新用例在当前 master 上全部通过;Windows 本地与 Linux CI 各跑一次。

## 2. 结构拆分(restructure 的 P3 之后开始;函数体不改)

- [ ] 2.1 【A-01】【主】头文件切分与 include 瘦身,分两个提交。
  - (a) 拆出 `agent_callbacks.hpp`(hpp 102-187)、`control/control_receipt.hpp`(hpp 43-89)、`turn/turn_types.hpp`(hpp 541-811)。AgentLoop 内用 using 别名保留嵌套名,`agent_loop.hpp` 转 include 这三个新头。
  - (b) 逐个 TU 补齐 IWYU include。门面必须保留的 include 按 design.md §4 列出,其余改为前置声明。
  - 前置:restructure 4.2(P3-02)、1.1。
  - 验证:全新构建目录编译 acecode、acecode_testable、acecode_unit_tests 三个目标加 tests;`AgentLoop::ProviderAccessor` 别名不变。
- [ ] 2.2 【A-02】【子】抽出纯函数。
  - 函数体逐字搬迁到以下文件:
    - `detail/agent_payloads`;
    - `request/provider_history`、`request/request_context`;
    - `approval/permission_payloads`;
    - `transcript/transcript_queries`;
    - `goal/goal_prompts`:入参为 GoalPromptTools,AgentLoop 成员改成一行转发;
    - `turn/user_turn_message`、`recovery/provider_error_report`、`progress/retry_progress` 的纯函数部分。
  - `resolved_question_policy` 改为调用 `tool/question_policy`,删掉 `session_registry.cpp:1137-1144` 的重复实现;
  - 本模块内 `ascii_lower`、`now_epoch_ms`、`trim_ascii_copy` 的副本改用 utils 的版本。替换前核对行为逐字相同,包括大小写与空白集合。
  - 放入 `acecode::agent::detail` 命名空间,避免与其它 TU 的静态函数同名冲突。
  - 前置:2.1。
  - 验证:
    - 新增 `tests/agent/{goal,request,turn,recovery,approval,transcript}/*_test.cpp` 纯函数单测,写中文注释;
    - `RequestPrefixIsByteStableAcrossIterationsInATurn`、`agent_loop_goal_test`、`AgentLoopSkillContext.*`、`system_prompt_test` 全部通过。
- [ ] 2.3 【A-03】【主】把同一个类拆到多个 TU。
  - 成员函数定义按簇整块搬到 design.md §5 表中的文件,函数体不改;
  - 权限门那一段(约 5070-5630)直接落到 `approval/tool_permission_gate.cpp`(LR-22);
  - PA 相关成员 TU 放在 `recovery/pa_rescue_host.cpp`,不放进 pa 目录(LR-9);
  - 一次提交,提交说明附「原行号 → 新文件」映射表,提前在 tasks.md 认领并公告。
  - 前置:2.2。
  - 验证:
    - 三平台构建通过;
    - `check_line_coverage.py` 显示原文件每一行都有去处,覆盖率 100%;
    - `git diff --stat` 显示 agent_loop.cpp 只减不增;
    - `git blame -C` 抽查可追溯;
    - 所有新文件不超过 1000 行(`turn/turn_runner.cpp` 过渡期容纳约 905 行)。
- [ ] 2.4 【A-04】【主】把 `execute_tool_calls` 里的 lambda 提升为成员函数。
  - `run_tool_with_lifecycle`、`execute_single_tool`、`maybe_guard_tool` 与写路径 runner 原样提升为私有成员;
  - `[&]` 捕获的局部改成显式参数结构 `ToolBatchState`(引用传递,暂不改数据流);
  - `current_step_preamble_` 改为显式参数。
  - 前置:2.3、1.2。
  - 验证:
    - P0-11 的黄金序列逐条一致;
    - 以下测试全部通过:
      - `agent_loop_tool_lifecycle_events_test`、`agent_loop_auto_mode_test`;
      - `agent_loop_goal_test`、`agent_loop_plan_mode_test`、`hook_agent_loop_test`;
      - `agent_loop_ask_user_question_parallel_test`、`agent_loop_computer_use_scheduling_test`;
      - `agent_loop_tool_result_storage_test`、`agent_loop_termination_test`;
    - `audit_sandbox` 早绑定与 daemon ask 分支晚求值两处时机,经 code review 逐条确认。
- [ ] 2.5 【A-05】【主】RAII 原语落地,依赖 ownership 的 P2-01。
  - `ActiveProviderScope` 替换四处手工 set/clear,删除 recover 中 `active_provider_.reset()` 这个兜底;
  - `BusyCycleScope`:析构时检查 `std::uncaught_exceptions()`,异常路径跳过终态;
  - `SessionLease` 替换 `DesktopTurnLease` 与 6663 行的手工释放;
  - `SynchronizedDoomGuard` 替换 (guard, mutex) 两件套参数;
  - `ToolStreamProgress` 统一两份 ProgressState;
  - AbortSignal 按 design.md §4 的表改造全部写入点与外泄点;
  - `shutdown()` 只做 `request()` 加 `wake()`;
  - worker 改为最后声明的 `JoiningThread` 成员,仍在构造时启动。
  - 前置:2.4。
  - 验证:
    - `agent_loop_worker_recovery_test`、`agent_loop_termination_test`(`AbortWakesTwentyMinuteRetryWaitPromptly`、`UserAbortShortCircuits`)、`agent_loop_compact_events_test`、`agent_loop_pa_rescue_test`、`agent_loop_doom_guard_test`、`shell_write_guard_test` 通过;
    - 新增两条用例:「回合内 `interrupt_turn` 能立即唤醒 PA 等待」和「异常路径的终态事件只出现一次」。

## 3. 协作类(按依赖从下往上)

- [ ] 3.1 【A-06】【主】队列与回合门(LR-4:排在其它协作类之前)。
  - `AgentTaskQueue`:deque 双 FIFO、priority 优先、迭代器扫描,不再整队拷贝;
  - `ActiveTurnGate`:`interrupt(expected, input, queue&)` 这类「gate 锁内再取队列锁」的原子操作,是唯一允许的方向;
  - `TaskHandoff`:跨 loop 锁序 `source.queue → target.queue`;
  - 锁序写进类型注释。**shutdown 时仍然不清空队列,保持现状**,清空由 O-11 负责。
  - 前置:2.5。
  - 验证:
    - `agent_loop_turn_steering_test`(`InterruptAcceptanceCommitsExactlyOnce`、`EveryAcceptedFinalBoundaryRaceInputIsCommitted`)、`agent_loop_question_interjection_test`、`AgentLoopTaskHandoff.*`、`session_registry_test`、`web_server_smoke_test::QuestionInterjectResolvesPendingQuestionInSameTurn` 通过;
    - 新增 `tests/agent/worker/agent_task_queue_test.cpp` 与 `tests/agent/turn/active_turn_gate_test.cpp`,覆盖优先级 FIFO、128 上限、exactly-once、锁序;
    - Linux 下用 TSan 跑一遍 agent 相关用例。
- [ ] 3.2 【A-07】【主】历史与转录。
  - `ConversationHistory` 成为 `messages_` 的单写者,收口约 30 个写入点;
  - `TranscriptWriter` 收纳 `dispatch_message` 等;
  - `TrajectoryRecorder` 做成 RAII 卸载的单槽 observer,注释写明只有一个槽位;
  - `TurnOutcomeRecord`;
  - 删除 `messages_mut()`,`thread_service.cpp:1167` 改为 `history_on_worker(fn)`;
  - 空闲断言的条件是「!busy 且(在 worker 线程,或调用方持有队列门)」,先只打 LOG_WARN。
  - 前置:3.1。
  - 验证:
    - `agent_loop_trajectory_test`、`agent_loop_session_summary_event_test`、turn_steering 中的重试用例、`session_resume_restore_test`、`builtin_commands_test`(/clear)、termination(消息 id 与 JSONL 重读一致)、thread_service 相关测试通过;
    - 新增 `conversation_history` 单测;
    - R11 检查显示 `messages_` 的写入只出现在 `transcript/conversation_history.cpp`。
- [ ] 3.3 【A-08】【主】低耦合协作类。
  - `GoalRuntime`:叶子锁,只保护游标;`maybe_continue` 在锁外读取;线程归属表登记从并行只读线程进入的入口;
  - `AgentHookBridge`:request context 队列加锁 swap-drain,`stop_hook_active_` 保持跨回合;
  - `ToolHookBridge`:`PermissionHookSession` 做成 RAII;
  - `WorkspaceBoundary`:`cwd()` 按值返回;
  - `SessionExecSecurity`:含 SandboxFeedback 加锁,以及 SafeEditGuard。
  - `set_cwd` 的扇出顺序原样保留在门面里。
  - 前置:3.2。
  - 验证:`agent_loop_goal_test`、`goal_command_test`、`hook_agent_loop_test`、`agent_loop_workspace_folders_test`、`agent_loop_auto_mode_test`、`audit_log_test`、`spawn_subagent_tool_test`、`session_registry_test`(sandbox 命令、刷新)通过;黄金序列一致。
- [ ] 3.4 【A-09】【主】旁路问答与进度(D11)。
  - `SideQuestionService`:保持一个请求一个线程,线程容器换成可回收的 JoiningThread 表,shutdown 后抑制回调;`side_chat` 的 `chat_stream` 调用点登记进 R11;
  - `ActivityNarrator`:叶子锁,修掉重入陷阱但不改变输出;
  - `AgentProgressEmitter`:由 TurnContext 以 shared_ptr 持有;
  - `RetryProgressReporter`:三处共用。
  - 前置:3.3。
  - 验证:`SideQuestionUsesDetachedContextWithoutToolsOrTranscriptMutation`、`side_chat_test`、`PostSideQuestionWorksBeforeFirstMainRequest`、`agent_loop_tool_preamble_test` 六条、`model_retry_status_test` 通过。
- [ ] 3.5 【A-10】【主】请求组装与模型步。
  - `ApiRequestBuilder` 接收 `RequestBuildInputs` 值快照,`drain_hook_request_context` 挪到调用方;
  - 合并静态 system prompt 的重复构造,输出必须逐字节一致;
  - `PromptContextCache::reset_on_cwd_change` 只清 git 快照;
  - provider 保持每次迭代现取,`ActiveModelView` 以同一份快照为参数,工具名映射不在 agent 侧快照;
  - `ProviderStreamCollector`、`TurnUsageAccountant`、`ModelStepRecorder`。
  - 前置:3.4。
  - 验证:
    - `RequestPrefixIsByteStableAcrossIterationsInATurn`;
    - termination 中的 `TransientRetryResetsProvisionalStateAndReportsProgress`、`RecoveryPreservesAccountedUsageWhenConsumerThrows`、空回复与文本调用纠正;
    - `agent_loop_apply_patch_test`、`agent_loop_tool_protocol_names_test`、`agent_loop_plan_mode_test`、`system_prompt_test`、`trajectory_legacy_projection_test`;
    - P0-11 的静态 system prompt 一致性用例。
- [ ] 3.6 【A-11】【主】恢复链与 PA 接触点。
  - `ContextOverflowRecovery` 返回 {决策, timing_status};
  - PA 兜底的纯逻辑移到 `src/adapters/pa/pa_rescue_driver`,经 `PaRescueHost` 接口取副作用;
  - `CompactionController` 持有窗口链与 `compact_generation`,手动压缩路径保持没有机械兜底;
  - 按 design.md §6 的接触点表回填 `layers.tsv`。
  - 前置:3.5。
  - 验证:
    - `agent_loop_pa_rescue_test` 六条、`tests/pa/*`、`agent_loop_compact_events_test`(溢出恢复、`FailedAutoCompactIsAtomicAndRetriesOnNextTurn`、手动 /compact)、`agent_loop_goal_test`、hook 的 PreCompact/PostCompact 通过;
    - **演练删除 `src/adapters/pa/`,确认只需要改表中列出的位置。**
- [ ] 3.7 【A-12】【主】工具执行与权限门,分两个提交:先改数据流,再抽类。
  - 数据流:`ToolBatchState` / `ToolCallSlot` / `ToolCallOutcome` 取代原来的四个平行数组;并行线程只返回值,用 `FutureJoinGuard` 显式 join;`ToolBatchOutcome` 作为返回值交出 terminate 与 post_turn_actions。
  - 抽类:`ToolInvoker`、`ToolCallLifecycle`、`ToolLifecycleEvents`、`AskQuestionBinding`、`ToolResultPresenter`、`ToolResultCommitter`、`ToolCallMessage`、`ToolContextFactory`。其中 ToolContextFactory 组合 WorkspaceBoundary 与 SessionExecSecurity 来实现 `ToolSessionHost`,并提供 `for_user_shell` 变体。
  - 权限门:`ToolPermissionGate::decide` 返回 `PermissionVerdict`,仍是唯一审批入口;另有 `ExecPermissionGate`、`PathAccessPolicy`、`PermissionAuditScope`、`PermissionConfirmation`。
  - 前置:3.6、1.2。
  - 验证:
    - 黄金序列逐条一致;
    - `agent_loop_tool_lifecycle_events_test`、`agent_loop_auto_mode_test`、`agent_loop_goal_test`(`UnattendedGoalAutoApprovesDangerousBashInsideSandbox` 等)、`agent_loop_plan_mode_test`、termination(`TaskCompleteLiveMessageIdMatchesBudgetedCanonicalResult`、`TerminalSessionActionRunsAfterDoneAndStopsLaterWrites`、`UnknownToolErrorListsModelFacingNames`)、`agent_loop_tool_result_storage_test`、`agent_loop_metadata_injection_test`、`agent_loop_ask_user_question_parallel_test`、`agent_loop_computer_use_scheduling_test`、`agent_loop_doom_guard_test`、`hook_agent_loop_test`、`spawn_subagent_tool_test`、`permissions_test`、`worktree_tool_test` 通过;
    - 新增 `path_access_policy`、`permission_audit`、`ask_question_binding` 单测;
    - R11 检查显示审批决策与审计只出现在登记的文件里。
- [ ] 3.8 【A-13】【主】回合编排。
  - `TurnRunner` 骨架不超过 450 行;`ModelStepSink` 与 `PaRescueHost` 由 TurnRunner 或 adapter 实现;
  - `TurnContext` 由 `worker_main` 在任务结束时统一 reset,recover 对空指针兜底;
  - `TurnFinalizer` 把三份终态序列合成一张有序步骤表,差异点逐项保留;
  - `ResponseRecovery`、`AssistantOutput`、`UserShellTask`(BusyCycleScope 不发 BusyChanged(true))。
  - 前置:3.7。
  - 验证:
    - termination 全量(TurnTiming/TurnNetDiff 顺序、max_iterations、`NullProviderPromptsUserToConfigureModel`、`CorrectionDoesNotUnderflowIterationCounter`)、`FailingTaskAndErrorCallbacksDoNotPreventNextTurn`、goal(abort 暂停、`InterruptingTurnSteerKeepsGoalActive`、`PlanModeSuppressesGoalContinuation`)、hook(UserPromptSubmit、Stop)、`session_resume_restore_test`、`web_server_smoke_test`、`spawn_subagent_tool_test`、`loop_scheduler_test` 通过;
    - `wc -l`:agent_loop.cpp 不超过 900 行,hpp 不超过 450 行;`check_line_coverage.py` 覆盖率 100%。

## 4. 注入与收尾

- [ ] 4.1 【A-14】【主】构造注入与装配。
  - 新增 `AgentLoop(AgentLoopServices, AgentLoopOptions)` 与 `start()`;
  - 两个 prompter 保留「只能在 `start()` 之前调用」的 setter(D13);AskUserQuestionPrompter 由 AgentLoop 独占,SessionEntry 只持借用别名(D14);
  - 成员声明顺序与装配 DAG 按 design.md §3 执行,写进 hpp 注释;
  - 旧构造仍在构造时立即 start,只有在 busy 或已处理过任务之后调用 setter 才告警;
  - `loop_cfg_` 在运行期只能经 `enqueue_control` 或原子快照修改;
  - `SessionRegistry::make_entry`、`tui/subagent_host` 与 TUI(split-tui-main 的 B-13 已把 SessionManager 的构造移到 AgentLoop 之前)切换到新构造;
  - 新增 `tests/agent/agent_loop_fixture.hpp`。
  - 前置:3.8,以及 split-tui-main 的 B-13。
  - 验证:
    - acecode、acecode_testable、acecode_unit_tests、desktop 四个目标都能构建;
    - `session_registry_test`、subagent 相关、`web_server_smoke_test`、`headless_ask_result_test` 通过;
    - 手工冒烟:TUI 一轮对话 + `/btw` + `/compact` + `!cmd`;daemon 下 Web 一轮对话 + 权限弹窗 + 提问插话。
- [ ] 4.2 【A-17】【子】删除过渡接口,测试迁到 fixture,更新文档。
  - 17 个以上的用例改用 `agent_loop_fixture.hpp`;
  - 直接删除旧构造,以及已被 `AgentLoopServices` 取代的 setter(`set_session_manager`、`set_hook_manager`、`set_skill_usage_store`、`set_skill_idle_days`、`set_memory_registry` 等),不留 deprecation 周期;
  - 四个 `set_*_config` 由 ownership 的 O-10 删除;
  - CLAUDE.md 中指向 `agent_loop.cpp` 的行号锚点改为指向新文件。
  - 前置:4.1。
  - 验证:grep 确认 src/ 与 tests/ 中已没有被删除的 setter;`check_doc_paths.py` 为 0;全量单测通过。

## 5. 验收

- [ ] 5.1 【P6A 完成】按 design.md §8 逐条核对:
  - 行数:agent_loop.cpp ≤ 900、hpp ≤ 450、engine/agent 下所有文件 ≤ 1000;
  - 行号覆盖率 100%;
  - R11 白名单;
  - `layers.tsv` 已回填;
  - 所有权指标;
  - 用例清单;
  - 守护测试;
  - CLAUDE.md 锚点。
  - 验证:核对结果作为勾选依据,写进提交说明。
