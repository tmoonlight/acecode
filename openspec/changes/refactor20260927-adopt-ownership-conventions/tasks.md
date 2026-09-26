# Tasks: refactor20260927-adopt-ownership-conventions

> **开工前必读**:
> - `refactor20260927-restructure-src-layers/design.md` 的 §6「提交与协作约定」;
> - 本变更 design.md 的 §1(约定 C1–C14)和 §3(行为变更语义)。
>
> **提交**:
> - 前缀写 `refactor20260927(ownership/<任务编号>): …`;
> - 行为变更的提交摘要前加 `[行为变更 Dn]`,并且单独提交,方便单独回滚。
>
> **认领**:开工前在任务行末尾追加 `〔认领: <代理名> <日期>〕`,单独提交到 master。
>
> **执行时机**:除 1.1(P2-01)外,本变更的任务都在 restructure 的 P3 之后执行;不得和 split-agent-loop / split-tui-main 同时修改同一个文件。
>
> **每个任务的通用验证**:
> - 用例清单不变,外加新增用例;
> - Linux CI 与 Windows 本地构建 `acecode_unit_tests` 并全量通过;
> - `check_ownership.py` 指标不上升。
>
> **测试要求**:新增用例一律写中文注释,说明触发场景、期望行为,以及修复前的表现。

## 1. 基础原语(冻结前完成)

- [ ] 1.1 【P2-01】【子】新增 RAII 与并发原语,纯新增文件,放在 `src/utils/`,冻结后随目录进入 `base/utils/`。〔认领: Codex-raii 2026-09-27〕
  - 新增文件:
    - `joining_thread.hpp`:JoiningThread + StopToken、JoiningThreadGroup(从 `worker.cpp:109-120` 原样提升,worker 改为 include 新头)、ReapingThreadSet;在线程自身上析构时 detach 并记日志;
    - `lifetime_token.hpp`:LifetimeToken / LifetimeRef;
    - `scope_exit.hpp`;
    - `abandonable_call.{hpp,cpp}`:`run_abandonable`、`spawn_owned_detached`、`wait_for_abandoned_work`;
    - `abort_signal.hpp`:提供 `request/clear/wait_for/raw`,以及非 const 的 `flag_for_legacy_api()`,内部是叶子锁。
  - **必须在 restructure 的 P3 冻结之前合入。**
  - 验证:新增 `tests/utils/*_test.cpp`,覆盖:
    - JoiningThread 析构时 request_stop 加 join;在自身线程上析构不抛异常;
    - ReapingThreadSet 回收已结束线程,数量有界;
    - LifetimeToken 的 revoke 会等待正在执行的回调,revoke 之后回调不再执行;
    - `run_abandonable` 在 abort 后 100ms 内返回,迟到的结果被丢弃;
    - `wait_for_abandoned_work` 有界;
    - AbortSignal 的 `wait_for` 能被 `request()` 唤醒。
  - 可选:Linux 上用 ASan/TSan 跑一遍这些用例。

## 2. 不改 API 的修复

- [ ] 2.1 【O-01】【子】`EventDispatcher::unsubscribe_and_wait` + `ScopedSubscription`。
  - Subscription 增加在途计数与 cv;`drain_subscription` 在投递前后增减计数;
  - 在投递线程自身上调用时不等待,用 thread_local 记录当前线程;
  - `SessionClient` 增加虚函数 `unsubscribe_and_wait`,默认实现退化为 unsubscribe;`LocalSessionClient` 实现它;
  - 新增 `domain/session/scoped_subscription.hpp`;
  - 本任务不替换任何现有调用点。
  - 前置:1.1;restructure 4.2(P3-02)。
  - 验证:
    - 新增 `event_dispatcher_test` 用例:listener 阻塞时 `unsubscribe_and_wait` 会等到它返回;在 listener 内对自身退订不死锁;返回之后不再有投递;
    - 既有回放与顺序用例全部通过。
- [ ] 2.2 【O-02】【主】【行为变更 D7】`SessionRegistry::shutdown_all` + `SessionEntry` 显式析构。
  - `shutdown_all()`:
    - 置 `shutting_down_`,把 `entries_` swap 出来;
    - 对每个 entry 执行与 `destroy()` 相同的 `abort → loop->shutdown() → sm->end_current_session()`;
    - join 标题线程与生命周期线程;
    - 函数幂等;
    - 调用之后,create 与 resume 请求被拒绝。
  - `~SessionRegistry` 改为调用 `shutdown_all()`。
  - `SessionEntry` 显式析构:先停 loop,再让成员析构;头注释改为与实现一致。
  - `on_turn_finished` 改为捕获 `LifetimeRef<SessionRegistry>`。
  - `title_threads_` / `lifecycle_threads_` 换成 `ReapingThreadSet`。
  - 前置:1.1;restructure 4.2(P3-02)。不需要 2.1,可以和它并行。
  - 验证:
    - 新增 `session_registry_shutdown_test`,5 条用例:
      - 析构时有挂起的 AskUserQuestion:提问以取消收尾,不崩溃;
      - 析构时有运行中的回合:worker 被 join;
      - `shutdown_all` 之后 create / resume 被拒;
      - 连续创建 N 个会话后,标题线程集合的大小有界;
      - 析构之后写者租约文件被删除,`updated_at` 保持不变;
    - ASan(Linux)通过;
    - `session_title_test`、`web_server_smoke_test` 通过;
    - spec `session-lifecycle` 中的前三个 scenario 有对应测试。
- [ ] 2.3 【O-03】【主】control lambda 改为捕获 `weak_ptr<SessionEntry>`。
  - 涉及位置:`session_registry.cpp` 原 1667、1706、1731、1864 行,**以及 `thread_service.cpp:1165`**;
  - lambda 执行时先 lock,再与 `entries_` 中的对象比对身份。
  - 前置:2.2。
  - 验证:
    - 新增用例:会话忙时排入 sandbox / exec-rules / MCP 控制项,然后 destroy,`weak_ptr<SessionEntry>` 立即失效。修复前的表现是成环,SessionManager 与 AgentLoop 永不析构;
    - grep 确认所有 `enqueue_control` 的 lambda 都不再强捕获 entry。
- [ ] 2.4 【O-04】【主】【行为变更 D6】daemon 拆除顺序(`apps/daemon/worker.cpp`)。
  - 关停顺序按 design.md §3「D6」逐条执行;
  - watcher 与 owner_monitor 改为 JoiningThread,并把捕获列表写成显式形式;
  - POSIX 信号处理改用 TerminationSignal(self-pipe),`g_term_*` 只保留桥接作用;
  - 清理运行时文件的逻辑包进 RuntimeFilesGuard;
  - `WebServerDeps::provider/provider_mu` 改传 nullptr,删除 worker 自己的 provider_mu。只改 worker 侧,src/web 零改动。
  - 前置:2.3。
  - 验证:
    - 把关停序列提成一个可注入记录器的函数,单测断言顺序:`loop_scheduler.stop` 早于 `shutdown_all`,`shutdown_all` 早于 MCP 与 LSP 的 shutdown;
    - 实机:Desktop 关闭时,同时存在运行中的回合(含慢 MCP 工具、LSP 诊断)、挂起的提问、运行中的子代理,daemon 正常退出,不出现 0xC0000409,也没有残留的 lease;
    - POSIX 下 `kill -TERM` 响应正常;
    - spec `process-shutdown` 中的前两个 scenario 有对应测试或实机记录。
- [ ] 2.5 【O-05】【主】【行为变更 D6】TUI 的 `SubagentHost` 析构安全与关停顺序。
  - `~SubagentHost()` 先调用 `registry_.shutdown_all()`;
  - 子会话 listener 改为 LifetimeRef + ScopedSubscription;`deps_` 不再重复保存 registry_deps;
  - `TuiShutdownSequence` 在 `agent_loop.shutdown` 之后、MCP 与 LSP 关闭之前,加一步 `subagent_host.shutdown()`,单独提交;
  - TUI 的 model_pool 回调改为捕获 LifetimeRef。
  - 前置:2.1(用到 ScopedSubscription)、2.2;split-tui-main 的 4.3(B-13)。
  - 验证:
    - 新增 `subagent_host_shutdown_test`:子回合运行中析构 SubagentHost 时,`remove_task` / `publish_tasks` 不再被调用。修复前的表现是回调锁住已析构的 mu_;
    - 实机:子代理运行中、有挂起提问时,`/exit` 与 Ctrl+C 都能正常退出(split-tui-main 手工清单第 10 小节)。
- [ ] 2.6 【O-06】【子】headless 订阅与回填指针收尾。
  - `headless_runner.cpp:642-722` 改用 ScopedSubscription,声明在被捕获的等待状态之后;
  - IIFE 末尾用 ScopeExit 清空 `subagent_deps->registry/client/config` 与 `thread_deps->service`;
  - 保持 IIFE 结构与现有收尾顺序不变。
  - 前置:2.1。
  - 验证:
    - 新增用例:`send_input` 失败时走早退路径,进程正常退出,没有 UAF;
    - `--output-format json`、`-c` / `--resume` 的既有测试通过;
    - 退出码 0 / 1 / 64 / 130 的语义不变;
    - spec `process-shutdown` 中「headless 退出顺序保持」这个 scenario 有测试锁定。
- [ ] 2.7 【O-08】【主】src/web 内两处 UAF 最小点修(D5,各自单独提交)。
  - `routes_workspaces.cpp:789-804` 的 opencode 导入线程,改为 `shared_ptr<OpencodeImportRuntime>` + `weak_ptr<GlobalSessionSearchService>`,写法仿照 routes_misc 里的升级任务;
  - WS subscribe listener(`routes_ws.cpp:282-296`)改为捕获 Impl 级令牌;`~Impl` 在 `app.stop()` 之后逐一退订。
  - 除这两处外,不做任何 web 重构。
  - 前置:2.1。
  - 验证:`web_server_smoke_test` 新增两条用例:导入进行中析构 WebServer 不崩溃;WS 已连接时析构 WebServer,之后会话 emit 不访问已释放的 Impl。前端零改动。
- [ ] 2.8 【O-09】【子】句柄 RAII 化。
  - `lsp_process` 改用 UniqueHandle / UniqueFd / UniqueProcess;
  - sandbox 后端的 `void*` 改为 UniqueHandle:bash_tool 与 `sandbox_backend_win.cpp` 多出口的释放改由删除器完成;
  - `audit_log` 改用 UniqueSqlite;
  - `computer_use/runtime.cpp:75` 与 `upgrade/executable_version.cpp:38/149` 改为 move-only 封装;
  - `web_search::Runtime` 与 `cdp_client` 的 `Impl*` 改为 unique_ptr;
  - `SessionManager` 改为持有 `optional<WriterLease>`;
  - pty 后端的句柄在二期再换。
  - 前置:1.1;restructure 4.2(P3-02)。不需要 2.1。
  - 验证:
    - `lsp_service_test`(含 spawn 失败路径)、sandbox 相关测试、`audit_log_test`、会话写者租约相关测试通过;
    - Windows 上对比测试前后的进程句柄数,确认没有泄漏;
    - sandbox 的 Job 约束保持不变:不设 `KILL_ON_JOB_CLOSE` / `SILENT_BREAKAWAY_OK`。

## 3. 行为变更:有界等待

- [ ] 3.1 【O-07】【主】【行为变更 D9】收口 detached 线程,退出时有界等待。
  - models.dev 刷新与区域探测改为 `spawn_owned_detached`,闭包只持有自有状态;
  - McpManager 的连接线程不再捕获 `&executor`,改为捕获 `shared_ptr<State>`;invoke 改为 `run_abandonable`;
  - `image_generation_client` 改为 `run_abandonable`;
  - TUI 的 Copilot 认证线程改为持有 `shared_ptr<CopilotProvider>`;
  - 三个入口在静态析构之前调用 `wait_for_abandoned_work(2s)`。
  - 前置:1.1;restructure 4.2(P3-02),不需要 2.1;若涉及 TUI 部分,需要 split-tui-main 的 4.2(B-12)。
  - 验证:
    - `mcp_manager_test::AbortDuringSlowToolCallReturnsQuickly` 与 image_generate 的取消用例通过;
    - 新增用例:「McpManager 的连接线程运行时 shutdown,ToolExecutor 先析构也不崩溃」;
    - 实机:TUI 启动后立即退出、daemon 启动后立即 stop,都不在静态析构期崩溃;记录退出耗时,确认不超过「原耗时 + 2 秒」,没有后台工作时不增加等待;
    - spec `process-shutdown` 的「有界等待」三个 scenario 有对应测试或实机记录。

## 4. 行为变更:依赖 AgentLoop 构造注入

- [ ] 4.1 【O-10】【主】【行为变更 D8】回合级配置快照(原计划编号 A-15)。
  - 删除 `set_{memory,project_instructions,custom_instructions,git_context}_config` 四个裸指针 setter,改用 `PromptConfigProvider`,在回合开始时捕获一次:
    - daemon 侧:在 `app_config_mu` 的 shared_lock 下拷贝;
    - TUI 侧:由 TuiApp 提供。
  - skills 与 expert 改为 `shared_ptr<const>` 快照,`switch_expert` 经 control 发布;
  - side question 的 prime 与压缩的初始上下文,各自取一次快照。
  - 前置:split-agent-loop 的 4.1(A-14)。
  - 验证:
    - `RequestPrefixIsByteStableAcrossIterationsInATurn`、`system_prompt_test`、`git_status_prompt_test`、`skills_index_prompt_test`、`session_registry_test`(switch_expert、resume)通过;
    - 新增用例:「回合进行中修改 custom_instructions,本回合的请求前缀不变,下一回合生效」,中文注释写明这是有意为之的回合级快照语义;
    - Linux 上用 TSan 跑「保存设置」与「回合执行」并发;
    - spec `prompt-config-turn-snapshot` 的三个 scenario 有对应测试。
- [ ] 4.2 【O-11】【主】【行为变更 D7】跨线程回调守卫与关停清队(原计划编号 A-16)。
  - LifetimeToken 守卫 side question 的异步回调与 `post_turn_action`;
  - `ToolStreamProgress` 与 ask 回调改用 weak_ptr;
  - `shutdown` 在 join worker 之后,于调用线程把两条队列 move 到局部变量,在锁外销毁,之后不再访问 this;
  - `ControlEnqueueReceipt` 的等待方拿到 `completed=false`。
  - 前置:split-agent-loop 的 4.1(A-14)、2.3(O-03)。
  - 验证:
    - ASan 下三条析构用例:挂起的提问、运行中的回合、排队的 control;
    - 新增用例:「shutdown 之后排队的控制项不执行,等待方拿到 completed=false,不挂死」;
    - spec `session-lifecycle` 的第四个 scenario 有对应测试。

## 5. 验收

- [ ] 5.1 【P7-O 完成】
  - 按 design.md §5 核对所有权指标,达到一期目标值;
  - ASan / TSan 相关用例通过;
  - 关停序列单测通过;
  - Desktop 与 TUI 的退出路径完成实机回归;
  - 每项行为变更(D6 / D7 / D8 / D9)都有对应的单独提交,提交摘要带 `[行为变更 Dn]`,并且都能对应到 spec 的 scenario;
  - AGENTS.md「所有权与生命周期」一节与最终实现一致。
  - 验证:把核对结果写进提交说明,作为勾选依据。
