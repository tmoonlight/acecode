# Design: refactor20260927-split-agent-loop

> **行号基准**:master `7942011b`,文件 `src/agent_loop.{hpp,cpp}`。restructure P2-08 之后,这两个文件先 R100 移到 `src/agent/`,P3 之后位于 `src/engine/agent/`。行号漂移时,按函数名与代码结构重新定位。
>
> **路径约定**:下文不带前缀的路径都相对 `src/engine/agent/`。
>
> 总纲、协作约定(提交前缀 `refactor20260927(agent-loop/<任务>)`、认领、热点互斥)见 `refactor20260927-restructure-src-layers/design.md` §6。

## Context

现状(调研实测):

- **hpp 1010 行**:约 103 个公开声明;私有字段约 195 行(hpp 813-1007),分属 provider/重试、工具、hooks、sandbox/exec rules/audit、workspace folders、goal、compaction、tool preamble、side question、turn/steer 队列、worker 线程、锁等十余组。
- **cpp 6952 行**,主要函数的行号与规模:

  | 函数 | 行号 | 规模 |
  |---|---|---|
  | 匿名 helper | 67-548 | |
  | 构造 / 析构、`set_session_manager`(安装轨迹 observer) | 552-627 | |
  | `worker_main` / `recover_worker_task_error` | 1158-1279 | |
  | 控制队列与交接 | 1304-1561 | |
  | steer / interject / interrupt | 1563-1803 | |
  | 压缩 | 1858-2296 | |
  | goal | 2298-2737 | |
  | 回合入口 | 2857-3049 | |
  | `build_api_request_messages` | 3051-3261 | 约 210 行,prompt cache 敏感区 |
  | side question | 3263-3375 | |
  | 进度文案 | 3426-3557 | |
  | `call_provider_and_collect` | 3559-3877 | 约 320 行 |
  | `handle_provider_error` | 3880-4024 | |
  | PA rescue | 4027-4207 | |
  | `build_tool_context` | 4209-4321 | |
  | **`execute_tool_calls`** | **4323-5808** | **约 1490 行** |
  | **`run_agent_with_input`** | **5809-6713** | **约 905 行** |
  | `run_compact` | 6715-6801 | |
  | `run_shell` | 6803-6950 | |

- tests 里没有 friend 或 `#define private`,全部经公开 API + `StubLlmProvider` 黑盒驱动。阶段 A 的结构拆分因此不需要改测试。
- 依赖倒置的现状:agent 依赖 `web::compute_message_id` / `message_payload`、`desktop::load_workspace_folders`、`commands/compact`、`computer_use::release_session`。restructure 的 P2 已把这些下沉,或移进 `event_payload/`、`compaction/`。

## Goals / Non-Goals

**Goals:**
- 单文件不超过 1000 行;门面 `agent_loop.cpp` 不超过 900 行,`agent_loop.hpp` 不超过 450 行。
- 每个协作类职责单一、依赖单向,不回指 AgentLoop。
- 所有权在类型里表达清楚:unique_ptr 独占、构造注入引用、回合快照、RAII scope。
- 原文件的每一行都有去处:用 `check_line_coverage.py` 检查,覆盖率 100%。

**Non-Goals:**
- 不改任何用户可见行为,已知疑点原样保留(见 §5 最后一条)。
- 回合级配置快照(D8)与关停后清空队列(D7)不在本变更,由 adopt-ownership-conventions 的 O-10、O-11 负责。
- 不改 `ToolContext` 的结构(D18),不改 side question 的线程模型(D11)。

## Decisions

### 1. 门面保留,对外 API 不变

`AgentLoop` 仍是唯一对外类型,协作类不对外暴露。以下公开签名与语义全部保留:

- **构造与生命周期**:旧 5 参构造(过渡期,内部转发)、新增 `AgentLoop(AgentLoopServices, AgentLoopOptions)` + `start()`、析构、`abort()`、`cancel()`、`clear_stale_abort_request()`、`shutdown()`、`is_aborting()`、`is_busy()`。使用方:`SessionRegistry::make_entry`、TUI、`subagent_host`、全部 tests。
- **提交与控制**:`submit`×3、`submit_shell`、`submit_compact`、`enqueue_control`(`ControlEnqueueReceipt` 语义不变)、`try_run_idle_control`、`retry_last_user_message`、`has_pending_work`、`has_queued_user_work`、`submit_task_suggestion_input`、`has_task_suggestion_input`、`try_start_side_task`、`complete_task_handoff`。
- **回合内 steering**:`steer_input`、`interrupt_turn`、`interject_question`、`active_turn_id`。
- **side question**:`side_question_context_snapshot`、`prime_side_question_context`、`ask_side_question`、`stream_side_chat`、`ask_side_question_async`。
- **历史**:`messages()`(const 引用)、`push_message` / `clear_messages`(委托 ConversationHistory,仍要求空闲)、`inject_shell_turn`、`emit_system_message`、`emit_transcript_system_message`。
- **工作区与安全**:`cwd()`(**改为按值返回 `std::string`**,LR-12;调用方源码不用改)、`set_cwd`、workspace folders 系列、`write_root`、`set_inherited_write_root`、loop execution policy、sandbox 与 exec rules 系列,以及测试钩子 `set_exec_rules_dir_for_tests`、`set_sandbox_availability_for_tests`、`set_audit_sink`、`set_exec_rules`。
- **配置与模型**:`set_context_window` / `context_window`、`set_agent_loop_config`、tool preamble 配置、capability policy、`invalidate_git_snapshot`、`last_turn_failed` / `last_turn_error`、`resolved_question_policy`。
- **goal / hooks / 事件**:goal 四件、`dispatch_session_start_hook`、`dispatch_session_title_changed_hook`、`events()`、`set_callbacks`、`set_permission_prompter`、`set_ask_question_prompter`、`set_ask_question_channel`。
- **唯一删除的公开方法**:`messages_mut()`。唯一调用方 `thread_service.cpp:1167` 改为新增的 `history_on_worker(fn)`。

以下契约也保持不变:

- `AgentCallbacks` 的字段名与触发契约:`on_turn_finished` 每个回合恰好调用一次,且紧挨 busy=false;compact / shell 不调用它。
- SessionEvent 的种类、载荷与顺序。
- `ProviderAccessor` 别名、`ControlEnqueueReceipt`、`TurnSteerResult`、JSONL 落盘形状。
- 所有中文文案的字节。

### 2. 抽取顺序:先无状态,后有状态,最后改注入方式

先做纯函数(A-02),再把同一个类拆到多个 TU(A-03)、把 lambda 提升为成员函数(A-04)。之后按依赖从下往上抽协作类:

1. 基础设施:A-05 RAII 原语 → A-06 队列与回合门 → A-07 历史与转录;
2. 叶子协作类:A-08;
3. A-09 → A-10 → A-11 → A-12 → A-13;
4. 最后 A-14 改注入方式。

基础设施必须排在协作类之前(LR-4)。否则 GoalRuntime、AgentHookBridge 在被抽出时,只能回指 AgentLoop,或者直接持有 `messages_`、`queue_mu_` 的引用。

### 3. 所有权模型

- **协作对象由 AgentLoop 独占**:以 `std::unique_ptr` 持有,hpp 只做前置声明,析构函数在 .cpp 里定义。协作类之间用构造注入的 `T&` 互连,不回指 AgentLoop。
- **需要 AgentLoop 能力的窄接口,不由 AgentLoop 自己实现**(LR-13):
  - `ModelStepSink`、`PaRescueHost` 由 TurnRunner 或持有 TurnContext 的小 adapter 实现;
  - `ToolSessionHost` 由 ToolContextFactory 组合 WorkspaceBoundary 与 SessionExecSecurity 实现;
  - `HookContextProvider` 只提供 cwd / 权限模式 / 会话 / 模型这类查询。
- **成员声明顺序**(LR-2),在 agent_loop.hpp 用注释清单钉死:
  1. atomic 标志、`AbortSignal`、`EventDispatcher events_`、`CallbacksSlot`、`TurnOutcomeRecord` 放在最前,它们被协作对象和 prompter 引用,必须最后析构;
  2. 按下面装配 DAG 排列的协作对象;
  3. prompter;
  4. `JoiningThread worker_` 放在最后,最先析构。
- **装配 DAG**(LR-3),声明顺序与之一致:AbortSignal / events → ConversationHistory → TranscriptWriter → TurnOutcomeRecord → AgentTaskQueue / ActiveTurnGate → WorkspaceBoundary → SessionExecSecurity → AgentHookBridge → GoalRuntime → PromptContextCache / ApiRequestBuilder → CompactionController → PA 接触点 → SideQuestionService → 工具链(ToolContextFactory → approval → tool_exec)→ TurnFinalizer / TurnRunner。
- **AgentLoopServices**:构造注入,生命周期必须长于 AgentLoop。包含:
  - `ToolExecutor&`、`PermissionManager&`;
  - `SessionManager*`、`HookManager*`、`SkillUsageStore*`、`const MemoryRegistry*`:可为空的借用指针;
  - `shared_ptr<const SkillRegistry>`、`shared_ptr<const ExpertDefinition>`;
  - `AuditSink`;
  - `AgentRuntimeEnv`:进程级单例的访问函数,包括 PA 学习器、MtimeTracker、computer-use 租约工厂、prompt_environment、interaction_mode、acecode_dir;默认指向现有全局,测试可替换。
- **AgentLoopOptions**(值):cwd、AgentLoopConfig、context_window、no_model_config_prompt、task_suggestion_compact_threshold、LoopExecutionPolicy、inherited_write_root、ToolCapabilityPolicy、expert_member_id、skill_idle_days、SandboxConfig、exec_rules_dir_override。
- **prompter 不进 services**(D13,LR-1):两个 prompter 都要用 loop 自己的 `events()` 构造(`session_registry.cpp:1124/1141`),放进 services 会形成构造循环。保留 `set_permission_prompter` / `set_ask_question_prompter`,只允许在 `start()` 之前调用。
- **AskUserQuestionPrompter 由 AgentLoop 独占**(D14,LR-11),与 PermissionPrompter 同构;`SessionEntry` 只持有借用别名。AskQuestionBinding 在调用时 lock,失败就返回 cancelled。
- **过渡期**(LR-18):旧构造仍在构造时立即 start,保持现状,不做延迟启动。只有在 busy 或已处理过任务之后调用 setter 才告警,避免每个会话打出一串噪声。
- **`loop_cfg_` 的运行期修改**(LR-M1):只允许经 `enqueue_control` 或原子快照进行。

### 4. 关键机制

- **TurnContext**(LR-20):
  - 由 worker 以成员 `std::unique_ptr<TurnContext>` 持有,不是栈对象。recover 在异常展开后仍要读 usage。
  - 回合开始时构造;由 `worker_main` 在任务结束后统一 reset,TurnFinalizer 不负责析构;recover 对空指针兜底。
  - 收纳的状态:callbacks 快照、UserTurnInfo、swarm_mode、迭代计数、RequestRecoveryState、PA rescue 状态与 skip_auto_compact_once、SynchronizedDoomGuard、`shared_ptr<AgentProgressEmitter>`、ResponseRecovery 预算、turn_timing_status、`optional<TokenUsage>`、SessionLease、ToolBatchOutcome 汇总。
  - 明确**不进** TurnContext 的跨回合状态:`stop_hook_active_`、`compact_generation_`、goal 记账游标、PromptContextCache、`last_api_total_tokens_`。
- **AbortSignal**(LR-5):
  - 提供 `request()`、`clear()`、`wait_for(ms)`、const `raw()`,以及非 const 的 `flag_for_legacy_api()`。后者只给 `chat_stream` / `compact_messages` 这类现有非 const 指针参数使用。
  - 内部锁是叶子锁;interrupt 路径在 `active_turn_mu_ → queue_mu_` 内调用它。
  - 改造清单(写入点与外泄点):

    | 类型 | 位置(原行号) | 改为 |
    |---|---|---|
    | 写入 | 1093(abort)、1100(clear_stale)、1110(shutdown)、1328、1490、1784(interrupt_turn)、5825、6716、6804 | `request()` / `clear()` |
    | 外泄 | 2252、3824、4216、4694、4739、5454(prompter)、6776、6861 | `flag_for_legacy_api()` |
- **shutdown 与 abort 的区别**(LR-6):
  - `shutdown()` 只做 `request()` + `ActiveProviderSlot::wake()`,不调用门面 `abort()`,不访问 SessionManager;
  - `abort()` 仍负责释放 computer-use 租约,shutdown 不负责;这个差异保持不变。
  - shutdown 顺序:side question 停 → 设标志 → request + wake → notify → join worker。
  - **不清空队列**:关停后清空队列属于行为变更,见 O-11。
- **BusyCycleScope**(LR-7):析构时检测 `std::uncaught_exceptions()` 大于构造时记下的值,就跳过终态,交给 recover,保证终态事件只出现一次。compact 发 BusyChanged(true),shell 不发,保持现状。
- **GoalRuntime 的锁**(LR-8、LR-M2):叶子锁,只保护记账游标。不跨 store、不跨 emit、不在持锁时调 `with_locked`;`maybe_continue` 在锁外读 goal,再进队列锁。并行只读工具线程会经 `tool_ctx.account_goal_usage` 等回调进入 GoalRuntime,必须登记进线程归属表。
- **provider 与工具名快照**(LR-10):provider 保持每次迭代现取;`ActiveModelView` 以同一份 provider 快照为参数。工具名映射不在 agent 侧另做快照,ToolExecutor 继续读进程级实时映射。
- **PromptContextCache**(LR-16):`reset_on_cwd_change()` 只清 git 快照,skill / session context 的 pin 由 cache_key 内容驱动,不随 cwd 清空。
- **ConversationHistory 的空闲断言**(LR-17):条件为「!busy 且(在 worker 线程,或调用方持有队列门)」,由 AgentTaskQueue 提供 `held_by_current_thread`。先只打 LOG_WARN。
- **side question 线程**(D11,LR-19):保持一个请求一个线程,只把 `vector<std::thread>` 换成可回收的 JoiningThread 表。`side_chat` 的 `chat_stream` 调用点登记进 R11。
- **EventDispatcher 的 observer 只有一个槽位**(LR-M3):TrajectoryRecorder 独占,类注释写明;若以后有第二个订阅者,需要另行设计。
- **include 瘦身单独一个提交**(LR-14)。门面必须保留的 include:`sandbox::ExecRules`(`set_exec_rules` 按值)、`security::AuditSink` 别名、`SandboxConfig`、`EventDispatcher`(值成员)、side_chat 类型。其余改为前置声明,依赖传递 include 的 TU 逐个补 IWYU include。
- **超大过渡文件**(LR-22):A-03 时权限门那一段(约 5070-5630)直接落到 `approval/tool_permission_gate.cpp`,保证 `tool_batch_scheduler.cpp` 过渡期间不超过 1000 行。

### 5. 目标文件表

来源一栏的行号指 P0-10 之前的原始 cpp;hpp 行号标 `hpp`。

| 目标文件 | 来源 | 预估行数 |
|---|---|---|
| agent_loop.hpp | 公开 API;私有区只放协作对象的 unique_ptr(前置声明)与 §3 的成员顺序 | ≤450 |
| agent_loop.cpp | 构造/析构 552-627、abort/shutdown 1092-1156、set_cwd 扇出、公开方法转发 | ≤900 |
| agent_loop_services.hpp | AgentLoopServices / AgentLoopOptions / AgentRuntimeEnv | 150 |
| agent_callbacks.hpp | hpp 102-187 + CallbacksSlot | 120 |
| control/control_receipt.hpp | hpp 43-89 | 60 |
| turn/turn_types.hpp | hpp 541-811,嵌套名以 using 别名保留 | 140 |
| detail/agent_payloads | JSON helper、431-500 中非 transcript 的 helper | 230 |
| request/provider_history | 69-108(`model_facing_provider_messages`,唯一出口) | 70 |
| request/request_context | 334-429、502-548 | 170 |
| request/prompt_context_cache | hpp 947-954 | 110 |
| request/api_request_builder | 3051-3261;合并 1957-1984 的重复静态 system prompt(输出须逐字节一致);dormant_skill_names 853-870 | 420 |
| transcript/transcript_queries | 过滤表、replace payload、431-500 中 transcript 相关的 helper | 110 |
| transcript/conversation_history | 884-980、1825-1856,加散落的约 30 个 messages_ 写入点 | 220 |
| transcript/transcript_writer | dispatch_message 等 | 240 |
| transcript/trajectory_recorder | 571-616(observer 单槽位) | 130 |
| turn/turn_outcome | last_turn_outcome / last_turn_error | 70 |
| turn/active_turn_gate + turn/turn_steering | 1563-1803、2739-2840 | 280 + 200 |
| turn/turn_context.hpp | 回合级状态 | 150 |
| turn/turn_runner | 5809-6713 的骨架 | ≤450 |
| turn/user_turn_message + turn/turn_lifecycle | 2857-3049 | 150 + 200 |
| turn/turn_finalizer | 6592-6713、5853-5877、1229-1279(三份终态序列合成一张有序步骤表) | 300 |
| turn/response_recovery | 6310-6493 | 250 |
| turn/assistant_output | 6227-6273、6495-6539 | 140 |
| turn/busy_cycle + turn/user_shell_task | busy 开关;6803-6950 | 90 + 200 |
| worker/agent_task_queue + worker/agent_worker | 1158-1302、1425-1447、1805-1823 | 220 + 170 |
| control/agent_control + control/task_handoff | 1304-1561 | 280 + 170 |
| boundary/workspace_boundary | 629-709、826-836 | 230 |
| approval/session_exec_security | 711-824、5568-5629 | 250 |
| approval/permission_payloads、path_access_policy、permission_audit | 4458-4503、5295-5314、5155-5184 | 110 / 150 / 90 |
| approval/exec_permission_gate | 5068-5134、5489-5548 | 220 |
| approval/tool_permission_gate + permission_confirmation | 5136-5566(唯一审批入口) | 420 + 180 |
| hook_bridge/agent_hook_bridge、tool_hook_bridge、hook_events | 982-1090;4584-4637、4833-4854、5357-5411;hook_events 在 P2-06 移入 | 280 / 200 / 原样 |
| progress/activity_narrator、agent_progress_emitter、retry_progress | 3426-3557;5953-6008;3377-3424、4038-4072 | 200 / 130 / 120 |
| model_step/active_provider_slot、active_model_view | 1877-1952 | 70 / 100 |
| model_step/provider_stream_collector、turn_usage_accountant、model_step_recorder | 3559-3877;6046-6226 | 400 / 150 / 200 |
| recovery/context_overflow_recovery、provider_error_report、pa_rescue_host | 3880-3993;3995-4024;4027-4207(过渡) | 200 / 90 / 150 |
| compaction/compaction_window、compaction_controller、compact | 1858-1875、2032-2296、6715-6801;compact 是 P2 拆剩的原 commands/compact | 110 / 430 / 原样 |
| goal/goal_prompts、goal_runtime | 2445-2607;2298-2443、2609-2737 | 190 / 340 |
| side_question/side_question_service、side_chat | 3263-3375、1143-1152;side_chat 是原 session/side_chat | 220 / 原样 |
| guards/doom_guard(含 SynchronizedDoomGuard)、safe_edit_guard | 原 agent_loop_doom_guard;5256-5293 | 600 / 120 |
| tool_exec/tool_batch_types、tool_call_message、tool_invoker | 4400-4456;4323-4398;4505-4551、4986-5066 | 110 / 120 / 120 |
| tool_exec/tool_call_lifecycle、tool_lifecycle_events、tool_stream_progress | 4580-4918;4680-4824 | 260 / 130 / 130 |
| tool_exec/ask_question_binding、tool_result_presenter | 4691-4761 | 120 / 120 |
| tool_exec/tool_batch_scheduler、tool_result_commit、tool_context_factory | 4952-5047;5643-5807;4209-4321(同时收编 run_shell 手工拼的残缺 ToolContext) | 200 / 220 / 220 |
| event_payload/message_payload、tool_event_payload | P2-08 移入 | 原样 |
| **engine/agent 之外** | `src/adapters/pa/pa_rescue_driver`(4027-4207 的纯逻辑)、`src/adapters/computer_use/session_lease.hpp`(DesktopTurnLease)、`tests/agent/agent_loop_fixture.hpp` | 250 / 70 / 150 |

子目录名都避开了模块名(R9):用 `hook_bridge` 而不是 hooks,`approval` 而不是 permissions,`boundary` 而不是 workspace。

### 6. PA 接触点表

与 restructure R3/R11 共用,**A-11 完成后回填 `layers.tsv`**。

| 位置(原行号) | 内容 |
|---|---|
| `adapters/provider/retry_policy.cpp:3` | include |
| `recovery/context_overflow_recovery`(3919) | `pa::is_context_overflow` 决定是否进入兜底链 |
| `recovery`(3930 / 3887) | `note_pa_context_rejection` / `note_pa_context_accepted`;通用溢出分支也会调用 rejection |
| `recovery`(3889、5916) | 请求被收下时、回合开始时复位 `pa_rescue_state_` |
| `recovery/pa_rescue_host`(4008-4202) | 调用 `adapters/pa/pa_rescue_driver` |
| `model_step/active_model_view`(1872、1893、1899、1904、1932、2195) | effective_window / compaction_context_window |
| `compaction/compact.cpp`(原 `commands/compact.cpp:5`、`:450`) | include 与 `pa::is_context_overflow` 兜底判定 |

验收:演练删除 `src/adapters/pa/`,确认只需要改表中列出的位置。

## 7. 不变量清单(每个任务都要逐条对照)

**请求与缓存**
1. prompt cache 前缀字节稳定。`build_api_request_messages` 每次迭代都会重跑,注入到最后一条真实 user 或摘要之前的内容只能由输入决定。静态 system prompt 只含 cwd 与按天的日期。`sandbox_prompt_snapshot` 重置与 `refresh_workspace_folders` 只在回合开头进行,且先于首次组装。合并两处 system prompt 构造后,输出必须逐字节一致。
2. 消息排列:[system prompt, 可选的独立 skill system 消息, provider 历史]。session → swarm → hook → plan → todo 这些可变上下文,插在最后一条真实 user 或摘要之前。`cached_context_for_api` 按 cache_key 钉住内容。git 快照只在 worker 上惰性采集,stale 标记只在非紧急档消费。
3. 发给 provider 的历史只有一个入口 `model_facing_provider_messages`(recover → sanitize → rewrite 工具名)。主请求、side question、PA 兜底共用它;压缩目前直接用原始 messages_,保持原样。
4. 工具表:常规路径保持 `std::map` 顺序;紧急档核心工具名经 `model_tool_name_for_native` 获取;`filter_tool_definitions_for_model` 在两条分支之后都执行。
5. `drain_hook_request_context` 有副作用,只在非紧急档消费一次;拆分后由调用方 drain,再作为输入传给 builder。

**锁与并发**
6. `active_turn_mu_ → queue_mu_`:worker 从不在持有 `queue_mu_` 时取 `active_turn_mu_`。其余几条:
   - `active_turn_mu_ → AskUserQuestionPrompter` 内部锁;
   - 跨 loop 交接只允许 `source.queue → target.queue`;
   - `try_run_idle_control` 的回调持有队列门时,不得 submit、不得等 worker、不得取 `active_turn_mu_`;
   - 持有 `model_control_mu` 时不取队列门;
   - 其余锁都是叶子锁,持锁不 emit、不回调。
7. worker 调度:priority 队列永远先于普通队列,各自 FIFO;`queued_behind_turn` 在同一把 queue 锁下计算。
8. interrupt 先关闭接收再置 abort;已接受的软插话按 FIFO 转成 priority 任务,只提交一次;`turn_interrupt_requested_` 区分 steering 中断(goal 保持 active)与手动停止(goal paused)。
9. 提问插话:先 `notify_response` 成功才压入输入,否则返回 NoPendingQuestion 且不提交。模型看到的顺序是 tool_call → tool_result → 插话 user。不得改走 interrupt_turn。
10. `drain_active_turn_inputs(close_if_empty=true)` 在同一把锁里判空并关闭接收;steering 队列上限 128。

**回合生命周期**
11. `last_turn_outcome` 必须在 busy_ 翻 false 之前写入;`dispatch_message("error")` 是回合级错误文案唯一的收集点。
12. worker 永不因任务异常退出。recover 的每个上报步骤独立 try/catch;终态 BusyChanged + Done 必达;usage 取自 worker 在栈展开后仍能访问的状态。
13. 用户回合落盘时序:
    - 回合前压缩先于用户消息落盘,重试回合不压缩;
    - 之后依次是:ensure identity → push → on_message → checkpoint(非 hidden)→ session_updated{summary} → Message → turn_start/busy 轨迹 → begin_active_turn → on_busy_changed → BusyChanged;
    - hidden_goal_context 跳过 UserPromptSubmit、不计时、不建检查点、不发 Message。
14. 每次迭代的顺序:begin_model_turn → 代际检查 → 自动压缩 → drain(false) → goal steering → build → publish side question → 刷新模型侧工具名 → provider 快照(为空则在 ModelStepStart 之前 break)→ ModelStepStart → record_model_request → call。
15. 每个 ModelStepStart 恰好配一个 Finish;BusyChanged / Done 先 `record_terminal_trajectory_events` 再 emit。
16. 迭代计数:provider 重试、文本调用纠正、空回复重试都不计入 max_iterations,且防下溢;0 表示无限。
17. 流式输出:
    - Retry 时整体清空临时正文、推理、工具、usage,并发出 TranscriptReplace;
    - `model_first_output` 每次尝试只记一次;
    - UserCancelled + abort 时忽略 Error;
    - scanner flush 只在成功路径执行;
    - `<text_preamble>` 只从 UI 流里剥,落盘保留原文。
18. 用量只在 `!provider_error && !abort && has_data` 时入账,且在消费者可能抛异常之前写进 worker 可读的状态;没有 provider usage 时,每步(含纯工具步)都做估算入账。
19. 事件顺序 `token… → usage → message(assistant) → tool_start` 不变。中断残留的输出只作为 transcript_only + interrupted_output,不进 history。
20. 收尾步骤逐项保持:max_iter 通知 → abort 两分支 → goal 入账 → TurnDiff → timing → Interjected / Interrupted → reset activity → computer-use 释放 → on_turn_finished → 终态轨迹 → on_busy_changed(false) → close_and_discard → record_outcome → busy=false → BusyChanged → Done → terminate / continue goal。三份终态序列之间的差异保留:hook 拦截的回合没有 BusyChanged(true)、diff、timing;recover 的 Done 只有 chat 任务带 turn_id 和 usage。
21. `on_turn_finished` 每个提交回合恰好调用一次。compact / shell 的 busy 周期不调用它;run_shell 不发 BusyChanged(true);落盘形状 `"!cmd"` + tool_result 固定;`inject_shell_turn` 只进 history 不落盘。

**审批与工具**
22. AgentLoop 是唯一审批入口。bash_tool 只执行注入的 exec_sandbox,不自行取消沙盒;规则 forbidden 优先于 yolo / dangerous;active goal 下越权与额外权限申请一律 Forbidden(`escalation_unattended`)。
23. 决策链顺序逐字不变:硬拒 → auto_allow → Yolo 硬拒 → bash 写边界与安全编辑守卫 → 路径校验与危险路径降级 → 自动放行审计 → goal 无人值守放行 → PermissionRequest hook → headless → 交互确认 → exec 无通道拒绝 → 隐式放行。
24. apply_patch:按路径集合逐条过全部检查;Plan 模式要求全部是计划文件;任一路径不过整份不执行;确认只弹一次。
25. 审计:
    - 只在「决定已作出」的分支记恰好一条,只读工具的自动放行不记;
    - `audit_sandbox` 早绑定,`audit_detail` 晚绑定;
    - PermissionRequest 与 PermissionResolved 严格成对,Resolved 只报一次。
26. PreToolUse 拒绝时不发 ToolStart / ToolEnd 实时事件,只补两条轨迹。ToolStart 靠事件订阅自动落轨迹;ToolEnd 手工记录,且每个调用恰好一条。
27. 调用行与结果行成对相邻:
    - 并行只读工具按提交顺序「调用行 → get → 结果行」;批内不检查 abort,批间与每个写工具之前检查;
    - 未执行的调用在 Phase 3 落盘为 `[Interrupted]`。
28. 模型历史的追加顺序逐字节不变:assistant(tool_calls) → 按原始下标的 tool 结果 → 紧随的 post_user_prompt → content replacement 元消息。task_complete 成功时,ToolEnd 推迟到 canonical 落盘之后。
29. AskUserQuestion 回调的求值时机:daemon 分支在回调被调用时才求 `goal_unattended`(30s override),TUI 分支在装配时求 timeout 与子代理来源标注。两种时机不可统一。
30. 写边界优先级:worktree > LOOP > 继承;Yolo 不豁免。`set_cwd` 的扇出完整且顺序不变:重建 PathValidator → git 快照作废 → clear_session_allows → sandbox grants 清空 → 违规清空 → reload_exec_rules → 重算可写附加根。

**其它**
31. 压缩:
    - 先建 checkpoint 再替换历史;
    - compact_generation 每次自增都重置 doom guard;
    - 失败时原子不改历史;
    - 手动 /compact 没有机械兜底。
32. 通用溢出恢复只按 Normal → HistoryRepaired → EmergencyProfile → 放弃 推进。PA 的观测要么可信要么整条丢弃;身份不明时既不记录也不查表;紧急档不记录;随机 400 不终止回合。
33. goal:先入账再停止;429 → usage_limited;用户 abort → paused;Plan 模式下不续跑;预算提示每个 goal 只提示一次。
34. 进度文案只在 `emit_agent_progress` 一处替换:750ms 节流,key 变化强制发送,中文字节原样。
35. 消息 id:非 user 消息为 `sha1(role+content)`,timestamp 为空;user 消息走 `ensure_user_message_identity`。
36. `abort()` 能唤醒 provider 重试等待(`AbortWakesTwentyMinuteRetryWaitPromptly`),以及 PA 等待。
37. **纯搬迁阶段原样保留的已知疑点**(归二期 P8):
    - 进度 key 用 `"\0"` 拼接;
    - 回合收尾里 `goal_store()` 与 `existing_goal_store()` 混用;
    - hook 拦截的回合不发 BusyChanged(true);
    - estimate breakdown 用裁剪前的工具表;
    - `build_hook_common_fields` 用 cwd_ 计算 transcript_path;
    - `stop_hook_active_` 跨回合残留。

## Risks / Trade-offs

- **[热点文件的大提交与并行分支冲突]** → A-03 这类大搬迁提前在 tasks.md 认领并公告;提交说明附「原行号 → 新文件」映射表,方便其它分支 rebase;当天合入。
- **[`[&]` 改为显式参数时漏字段或改变求值时机]** → A-04 逐条对照不变量 29(`audit_sandbox` 早绑定、daemon ask 晚求值);P0-11 的黄金序列兜底。
- **[锁序被新类拆散后出现 AB-BA]** → 锁层级写进类型注释;新增 `active_turn_gate` / `agent_task_queue` 单测;跑 TSan;P0-11 覆盖跨 loop 交接的并发。
- **[messages_ 写入点漏收口]**(约 30 处)会造成只进内存不落盘、或只落盘不进内存的分叉 → R11 白名单限定写入文件;A-07 以测试兜底。
- **[prompt cache 前缀被打穿]** → byte-stable 用例 + P0-11 的静态 system prompt 一致性用例;A-10 单独验收。
- **[门面超过 900 行]** → 转发清单写在门面头注释里;超出时优先把转发块移到按主题拆分的成员 TU,而不是放宽上限。

## Migration Plan

- 每个任务一组提交,可以单独 revert。结构拆分(A-01 到 A-13)不改变行为,revert 只影响代码组织。
- A-14 引入新构造时,旧构造作为兼容壳保留到 A-17。若 A-14 出问题,可以回退到旧构造路径。

## 8. 验收(P6A 完成)

- `agent_loop.cpp` 不超过 900 行,`agent_loop.hpp` 不超过 450 行,`engine/agent/**` 下没有超过 1000 行的文件(目标 200–600 行)。
- `check_line_coverage.py`:原 `agent_loop.{hpp,cpp}` 的每一行都有去处,覆盖率 100%。
- R11 白名单通过;`layers.tsv` 已回填 PA 接触点与单出口文件。
- 所有权指标:`set_*(T*)` 延迟注入在 AgentLoop 中为 0,只保留两个 start 之前的 prompter setter;`engine/agent` 下存入长寿对象的 `[&]` / `[this]` 为 0(配合 ownership 的 O-11)。
- 用例清单与 G0 相同,外加本变更新增的单测;Linux CI 与 Windows 本地都构建并跑通 `acecode_unit_tests`;CLAUDE.md 点名的守护测试全部通过。
- CLAUDE.md 中指向 `agent_loop.cpp` 的行号锚点已改为指向新文件。

## 9. 评审修正对照(对抗评审 LR-n 的落点)

| 评审 | 严重度 | 问题 | 落点 |
|---|---|---|---|
| LR-1 | blocker | prompter 依赖 loop 的 events,无法放进 services | §3,A-14(D13) |
| LR-2 | blocker | 值成员声明在后导致 events_ 先析构 | §3 成员顺序,A-14 |
| LR-3 | major | 装配顺序与依赖方向相反 | §3 装配 DAG,A-14 |
| LR-4 | major | 协作类先于基础设施抽取 | §2,A-06/A-07 先于 A-08 |
| LR-5 | major | AbortSignal 的 const 与写入点、PA 等待唤醒 | §4,P2-01,A-05 |
| LR-6 | major | shutdown 调 abort 会访问已销毁的 SessionManager | §4,A-05 |
| LR-7 | major | BusyCycleScope 异常路径重复发终态 | §4,A-05 |
| LR-8 | major | GoalRuntime 锁与队列锁 AB-BA | §4,A-08 |
| LR-9 | major | PA 调用点不止一个;成员 TU 放进 pa 会反向依赖 | §6,A-03、A-11 |
| LR-10 | major | 回合开头钉 provider 与实时查询不一致 | §4,A-10 |
| LR-11 | major | shared_ptr 共享 prompter 可能活得比 events_ 久 | §3,A-14(D14) |
| LR-12 | major | cwd() 返回悬空引用 | §1,A-08 |
| LR-13 | major | 窄接口仍由门面实现;门面预算不现实 | §3,A-12、A-13、A-14 |
| LR-14 | minor | include 瘦身需单列 | §4,A-01 |
| LR-15 | minor | JoiningThread 自我 join | P2-01(ownership) |
| LR-16 | minor | cwd 变化时不应清 pin | §4,A-10 |
| LR-17 | minor | 空闲断言误报 | §4,A-07 |
| LR-18 | minor | 过渡期告警噪声 | §3,A-14 |
| LR-19 | minor | side question 单线程队列会互相阻塞 | §4,A-09(D11) |
| LR-20 | minor | TurnContext 寿命 | §4,A-13 |
| LR-21 | minor | tests 镜像 | restructure P2-08(D15) |
| LR-22 | 超限 | tool_batch_scheduler 过渡期超过 1000 行 | §4,A-03 |
| LR-M1 | — | `loop_cfg_` 运行期修改通道 | §3,A-14 |
| LR-M2 | — | GoalRuntime 的跨线程入口 | §4,A-08 |
| LR-M3 | — | observer 单槽位 | §4,A-07 |
| LR-M4 | — | 跨 loop 交接的 AB-BA 无测试 | P0-11 |
