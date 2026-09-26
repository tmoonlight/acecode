# Design: refactor20260927-adopt-ownership-conventions

> **行号基准**:master `7942011b`。restructure 的 P3 之后,路径按 `refactor20260927-restructure-src-layers/layout-map.md` 换算,例如 `src/session/session_registry.cpp` 会变成 `src/host/session_host/session_registry.cpp`。
>
> **协作约定**:见 restructure design.md §6。提交前缀为 `refactor20260927(ownership/<任务>)`;行为变更的提交在摘要前加 `[行为变更 Dn]`。

## Context

**现状里已经做对、可以直接当范例的写法**:
- SessionRegistry → `shared_ptr<SessionEntry>` → `unique_ptr<SessionManager/PermissionManager/AgentLoop/AskUserQuestionPrompter>`;
- `SessionModelBinding` 用 `shared_ptr<LlmProvider>` 快照发布;
- McpManager 用 `shared_ptr<State>` 加 weak_ptr 闭包;
- WebServer::Impl 的 SubagentTrackerState、SessionChannelBinder 的 ContextLease;
- `worker.cpp:109-120` 的 JoiningThreadGroup;
- `SessionRegistry::resume` 的 InflightGuard;
- tool_protocol_names 与 models.dev 的 `shared_ptr<const>` 快照槽。

**真正的问题**(审计 26 条,按严重度):
1. **退出期的 UAF 链**:
   - `worker.cpp` 的拆除顺序与依赖相反。registry(573 行)先于 server(724 行)声明,所以最后才析构;`server.run()` 返回后的 927-957 行里,没有一步让会话停下来。
   - `SessionEntry` 的成员顺序是 `sm → perm → loop → prompter → ask_prompter`(`session_registry.hpp:104-111`),析构时 ask_prompter 先于 loop,而 loop 持有它的裸指针。
   - `SubagentHost` 的成员顺序是 `registry_ → client_ → deps_ → mu_ → running_`(`subagent_host.hpp:95-100`),析构时 registry_ 最后才走;子会话 listener 捕获裸 this,并且从不退订。
   - `enqueue_control` 的 lambda 捕获 `[entry, loop]`,又被存进这个 loop 自己的队列(`session_registry.cpp:1667/1706/1731/1864`、`thread_service.cpp:1165`),形成自持环。
2. **回调与线程**:
   - WS listener 捕获 `[this, conn_ptr]`,this 没有守卫(`routes_ws.cpp:282-296`);
   - opencode 导入用 `std::thread([this,...]).detach()`(`routes_workspaces.cpp:789-804`);
   - `EventDispatcher::unsubscribe` 不等待正在执行的投递(`event_dispatcher.cpp:106-129`);
   - headless 用 `[&]` 捕获栈变量,684-688 行的早退会跳过退订。
3. **数据竞争**:AgentLoop 经 `set_*_config(&cfg_mut.xxx)` 保存了共享可变 AppConfig 子对象的地址,worker 无锁读取;而 `settings_mutations.cpp:33` 在独占锁下对 AppConfig 整份赋值。
4. **延迟回填**:`SubagentToolDeps` / `ThreadToolDeps` / `WorkspaceToolDeps` 先注册、后回填裸指针。worker 里 ToolExecutor 比 registry 活得久,工具闭包里的指针在 registry 析构后悬垂。
5. **手工资源**:
   - `web_search::Runtime` 与 `cdp_client` 的 `Impl*` 用 new/delete;
   - LspProcess 用 `void*` / `int` 句柄;
   - AuditLog 持裸 `sqlite3*`;
   - sandbox 后端返回 `void*`,由 bash_tool 手工 `CloseHandle`;
   - 写者租约靠 acquire/release 手工配对,散落在 5 处;
   - POSIX 信号处理函数里调用 `notify_all`,不是 async-signal-safe 的。
6. **detach 与只增不减的线程**:
   - models.dev 刷新、区域探测在两个入口里 detach;
   - MCP invoke 与 image_generate 各自复制了一份「detached 线程 + ResultBox」;
   - `title_threads_` / `lifecycle_threads_` 只增不减。

## Goals / Non-Goals

**Goals:**
- 约定写成规则并能度量:所有权棘轮 R15 覆盖五类指标,并达到 §5 的一期目标值。
- 修掉已确认的退出期 UAF 链与数据竞争。
- 四项行为变更(D6–D9)按 specs 落地,每项单独提交。

**Non-Goals:** 见 proposal.md。尤其注意:`SessionRegistryDeps` 与 `WebServerDeps` 改为引用注入、ConfigStore、组合根,都属于二期。本期只修「不改 API」的问题,加上 D6–D9。

## Decisions

### 1. 所有权约定 C1–C14(P0-01 写进 AGENTS.md「所有权与生命周期」一节)

1. **C1 独占所有权**:owning 一律用 `std::unique_ptr`,pimpl 也一样。裸 `new` 只允许出现在私有构造工厂的 `std::unique_ptr<T>(new T)` 里,禁止裸 `delete`。`shared_ptr` 只给真正多方共享寿命的对象,并在声明处注明谁共享、为什么共享。反例:`web_search::Runtime` 的 `Impl* impl_`(runtime.cpp:21-26)。
2. **C2 构造注入**:必填依赖用构造注入的引用成员;可选依赖才用 `T*`,并注明 nullable 与 borrowed。对象构造完成后,依赖不可再换。反例:`SessionRegistryDeps::tools` 可以为空,却在 `session_registry.cpp:1076` 被无条件解引用。正例:`LocalSessionClient` 持有 `SessionRegistry&`。
3. **C3 借用只在一次调用内有效**:形参与 `ToolContext` 里的 `T*`/`T&` 不得存进成员,不得被异步回调或线程捕获;跨调用要用 `shared_ptr` 快照。反例:`spawn_subagent_tool.cpp:463-467` 在 `shared_ptr<SessionEntry>` 作用域结束后,还在用 `child->skill_registry.get()`。
4. **C4 共享只读数据用 `shared_ptr<const T>` 快照**,只有单一发布点;禁止把可变共享对象的子对象地址交给别的线程。反例:`set_project_instructions_config(&cfg_mut.project_instructions)`。
5. **C5 异步回调的捕获规则**:EventDispatcher listener、`enqueue_control`、AgentCallbacks、工具闭包、线程入口,只能捕获值、指向自有状态的 `shared_ptr`、`weak_ptr` 或 `LifetimeRef`,禁止 `[this]`、`[&]` 和裸指针。唯一例外:回调由 this 持有的 JoiningThread 执行,并且在 this 析构前 join。反例:`worker.cpp:846` 的 on_spawn `[&server]`、`session_registry.cpp:1066` 的 `on_turn_finished = [this, id]`。
6. **C6 不得形成自持环**:存进对象自身队列或成员的回调,不得强持有该对象;一律用 `weak_ptr`,执行时 lock 并比对身份。反例:entry → loop → 队列 → lambda → entry。
7. **C7 订阅即资源**:订阅用 move-only 的 `ScopedSubscription` 持有,声明在它捕获的对象之后;析构时退订,并等待正在执行的投递结束。持有 listener 需要的锁时,不得析构订阅。
8. **C8 线程规则**:禁止裸 `std::thread` 成员或局部变量,禁止 `detach`。长期线程用 `JoiningThread`;一次性短任务用 `ReapingThreadSet`;确实需要放弃等待的阻塞调用,只走 `run_abandonable` / `spawn_owned_detached`。反例:`worker.cpp:421/920` 的裸 std::thread、`routes_workspaces.cpp:789` 的 detach、`session_registry.hpp:394/396` 只增不减的 `vector<std::thread>`。
9. **C9 进程级服务**:由组合根的 RAII Scope 负责 init / shutdown。保留全局访问的,只能返回 `shared_ptr` 租约,禁止「`is_initialized()` + `service()`」两步式访问。反例:`lsp_tool.cpp:91-94`。本期只在新代码里遵守,存量改造在二期。
10. **C10 关停顺序**:依次为停入口 → 停回合生产者 → `SessionRegistry::shutdown_all()` → MCP → LSP / web_search → 其余,用成员声明顺序固化。反例:`worker.cpp:927-957`、`main.cpp:3388-3391`。
11. **C11 成员声明顺序就是析构契约**:持有 worker 线程的成员(AgentLoop、JoiningThread、SessionRegistry)必须声明在它引用的全部成员之后;做不到时,就写显式析构函数先停线程。反例:SessionEntry、SubagentHost。
12. **C12 禁止延迟绑定**:依赖 SessionRegistry 的工具,在 registry 构造之后再注册,闭包捕获 `weak_ptr<Service>`。本期只修回填指针的悬垂(O-06),重构注册顺序在二期。
13. **C13 句柄 RAII**:OS 与第三方句柄一律用 move-only 封装,禁止 `void*` 句柄出参和多出口手工 Close。反例:`sandbox_backend.hpp:67/79-82` 返回 `void*`;`computer_use/runtime.cpp:75` 的 Handle 可以被复制。
14. **C14 锁规则**:LifetimeToken、AbortSignal、GoalRuntime 的锁都是叶子锁,持有期间不调用 registry、AgentLoop 或回调;持有 `model_control_mu` 时不获取 AgentLoop 的队列门。

### 2. 基础原语(P2-01,纯新增,放在 `src/utils/`,冻结后位于 `base/utils/`)

| 原语 | 语义 | 替代的现有写法 |
|---|---|---|
| `JoiningThread` + `StopToken` | C++17 版 jthread。析构时 `request_stop()` 再 join。**如果在线程自身上析构或 join,改为 detach 并记日志**,而不是抛出 resource_deadlock(LR-15)。捕获列表必须显式。 | `worker.cpp` 的 watcher 与 owner_monitor;TUI 的动画、更新检查、auth 线程;themes 与 data_dir_migration 捕获 this 的线程 |
| `JoiningThreadGroup` / `ReapingThreadSet` | 前者由 `worker.cpp:109-120` 原样提升。后者每次 spawn 前先回收已结束的线程:保留「每个任务一个线程」的并发语义,但数量不再无限增长。 | `title_threads_`、`lifecycle_threads_`、side question 线程表 |
| `LifetimeToken` / `LifetimeRef<T>` | owner 持有 move-only 的 token,回调捕获可复制的 ref。`ref.with(fn)` 在共享锁下检查 owner 仍然存活才执行;`revoke()` 或析构时取独占锁,从而天然等待正在执行的回调结束。在回调内部 revoke 属于编程错误,由 debug 断言拦截。 | on_turn_finished `[this]`、SubagentHost listener `[this]`、on_spawn `[&server]`、TUI model_pool 回调的引用捕获 |
| `ScopeExit` | 函数内一次性收尾,可 `release()`,不跨对象持有。 | headless 为保证 MCP/LSP 收尾而写的 IIFE;回填指针清理 |
| `AbandonableCall` | `run_abandonable<R>(fn, abort, poll=100ms)`、`spawn_owned_detached(name, fn)`、`wait_for_abandoned_work(deadline)`。闭包自带全部状态,在进程级计数器里登记;组合根在静态析构前做有界等待。 | MCP invoke 与 image_generate 的两份「detached + ResultBox」;models.dev 刷新与区域探测的 detach |
| `AbortSignal` | `request()`、`clear()`、`wait_for(ms)`、const `raw()`,以及非 const 的 `flag_for_legacy_api()`。内部锁是叶子锁。 | AgentLoop 的 `abort_requested_` 与 PA 等待的 50ms 轮询(改造由 split-agent-loop 的 A-05 完成) |

后续任务新增的原语:
- `ScopedSubscription` 与 `EventDispatcher::unsubscribe_and_wait`(O-01,放 domain/session);
- `UniqueHandle` / `UniqueFd` / `UniqueLocalMem` / `UniqueSid` / `UniqueProcess`(O-09,放 base/platform/process);
- `UniqueSqlite`(O-09,放 base/platform);
- `WriterLease`(O-09,放 domain/session);
- `RuntimeFilesGuard`(O-04,放 base/ipc);
- `TerminationSignal`(O-04,放 base/platform)。

### 3. 四项行为变更的精确语义(均已由用户确认,对应 specs)

- **D6 关停顺序**(spec `process-shutdown`,O-04、O-05):
  - **daemon**:在 `server.run()` 返回之后,依次执行:
    1. `channel_runtime.stop`;
    2. `remote_web_proxy.stop`;
    3. `rc_binder.shutdown`,并清除 handler;
    4. `connector_first_start_threads.join_all`;
    5. 唤醒并 join watcher 与 owner_monitor;
    6. `loop_scheduler.stop`;
    7. `task_suggestions->shutdown`;
    8. `registry.shutdown_all()`;
    9. `subagent_deps->on_spawn = {}`;
    10. `mcp_runtime.shutdown`;
    11. `lsp::shutdown`;
    12. model_pool stop;
    13. `heartbeat.stop`;
    14. 清理运行时文件(由 RuntimeFilesGuard 兜底)。
  - **TUI**:在 `TuiShutdownSequence` 里,`agent_loop.shutdown` 之后、`mcp_manager.shutdown` / `lsp::shutdown` 之前,加一步 `subagent_host.shutdown()`。
  - **headless**:现有顺序已经正确(会话先于 MCP/LSP),保持不变,只加测试锁定。
  - **可见差异**:退出时被中断的工具结果,从「MCP/LSP 已关闭」这类错误变成 `[Aborted]`。
- **D7 会话资源释放**(spec `session-lifecycle`,O-02、O-11):
  - `SessionRegistry::shutdown_all()` 置 `shutting_down_`,把 `entries_` swap 出来,对每个 entry 执行与 `destroy()`(2225-2253 行)相同的序列:`abort → loop->shutdown() → sm->end_current_session()`;然后 join 标题线程与生命周期线程。该函数幂等。
  - `~SessionRegistry` 改为调用它;`SessionEntry` 显式析构,先停 loop。
  - `end_current_session` 内部的 `update_meta()` 默认保留磁盘上的 `updated_at`(`session_manager.cpp:1195-1197`),所以不改变排序。
  - O-11:AgentLoop 在 join worker 之后,于调用线程把两条队列 swap 到局部变量,在锁外销毁;`ControlEnqueueReceipt` 的等待方拿到 `completed=false`。**必须先完成 O-03**,把 control lambda 改成 weak_ptr;否则在 shutdown 里释放最后一个 entry 引用,会让 AgentLoop 在自己的成员函数里析构自身。
- **D8 回合级配置快照**(spec `prompt-config-turn-snapshot`,O-10,原计划编号 A-15):
  - 删除 `set_{memory,project_instructions,custom_instructions,git_context}_config` 这四个裸指针 setter;
  - 改用 `PromptConfigProvider`,返回 `SessionPromptConfig` 值快照,在回合开始时捕获一次,存进 TurnContext:
    - daemon 侧的实现:在 `app_config_mu` 的 shared_lock 下拷贝这四个子配置;
    - TUI 侧由 TuiApp 提供。
  - skills 与 expert 改用 `shared_ptr<const>` 快照,`switch_expert` 经 control 发布;
  - side question 的 prime 与压缩的初始上下文,同样各取一次快照;
  - `cached_context_for_api` 的 cache_key pin 语义不变。
- **D9 退出时有界等待**(spec `process-shutdown`,O-07):
  - models.dev 刷新与区域探测改用 `spawn_owned_detached`;
  - McpManager 的连接线程不再捕获 `&executor`,invoke 改用 `run_abandonable`;
  - image_generate 改用 `run_abandonable`;
  - TUI 的 Copilot 认证线程改为持有 `shared_ptr<CopilotProvider>`;
  - 三个入口在静态析构之前调用 `wait_for_abandoned_work(2s)`。

### 4. 执行顺序

1. **P2-01**(原语)必须在 restructure 的 P3 之前合入。
2. **P3 之后可以立即并行开工**:O-01、O-02、O-07(非 TUI 部分)、O-09。它们只依赖 P2-01。
3. **O-06、O-08** 依赖 O-01,因为要用到 ScopedSubscription 与退订等待。
4. **O-02 → O-03 → O-04** 串行:先让 registry 能安全关停,再打破自持环,最后调整 daemon 顺序。
5. **O-05** 依赖 O-01、O-02,以及 split-tui-main 的 B-13(TuiShutdownSequence 成形)。
6. **O-07 的 TUI 部分**依赖 split-tui-main 的 B-12。
7. **O-10** 依赖 split-agent-loop 的 A-14(构造注入就位)。
8. **O-11** 依赖 A-14 与 O-03。

本变更中的任务同一时刻不得与 split-agent-loop 或 split-tui-main 修改同一个文件,以 tasks.md 的认领为准。

### 5. 所有权指标(由 `check_ownership.py` 统计,src/apps/web 豁免)

| 指标 | 基线 | 一期结束目标 |
|---|---|---|
| 裸 `delete` / `new`(不含 make_unique、make_shared) | P0 实测 | `delete` 为 0;`new` 只允许出现在白名单中(FTXUI Make 等第三方惯用法、私有构造工厂) |
| `.detach()` | P0 实测 | 只剩 `abandonable_call.cpp` 与登记过的 waitpid 收尸线程 |
| `std::thread` 成员或局部变量 | P0 实测 | 只剩 joining_thread 内部 |
| 存进长寿对象或跨线程回调里的 `[&]` / `[this]` | P0 实测 | engine/agent、apps/tui/app、host/session_host 下为 0 |
| `set_*(T*)` 延迟注入 | P0 实测 | AgentLoop 中为 0,只保留两个 start 前的 prompter setter |
| 裸句柄(`void*` 句柄、`sqlite3*` 成员) | P0 实测 | 0 |

## Risks / Trade-offs

- **[shutdown_all 在自身 worker 线程上析构]**:最后一个 entry 引用可能在 worker 线程上释放 → JoiningThread 自我 join 保护(detach 并记日志);O-11 的清队列在锁外进行,之后不再访问 this。
- **[unsubscribe_and_wait 死锁]**:调用方持有 listener 需要的锁时退订会死锁 → 在投递线程上调用时不等待(thread_local 记录);C7 与 C14 写进约定;O-01 本身不替换任何调用点,只提供能力,风险隔离到后续任务。
- **[退订等待带来轻微延迟]**:WS 关闭、子会话清理等退订调用,可能要等正在执行的 listener(包括网络发送)返回。对 Crow 的处理线程而言只是轻微延迟,接受。
- **[D9 让退出变慢]**:最多多等 2 秒;没有后台工作时零等待。
- **[sandbox 句柄改动触及 Windows 受限令牌与 Job 路径]** → 只换所有权表达,不换语义:仍然**不设** `KILL_ON_JOB_CLOSE` / `SILENT_BREAKAWAY_OK`,相关测试全绿。
- **[worker.cpp、session_registry.cpp 是高冲突面]** → 改动集中在函数内部;在 P3 之后执行;认领互斥。

## Migration Plan

- 每项行为变更单独提交,提交摘要带 `[行为变更 Dn]`,可以单独 revert。revert 之后对应的 spec scenario 失效,需同步回滚 spec(或暂缓归档)。
- 非行为变更的修复(O-01、O-03、O-06、O-08、O-09)按普通提交合入。

## Open Questions

- 无。D5–D9 已在总纲 §9 决策登记中定稿。
