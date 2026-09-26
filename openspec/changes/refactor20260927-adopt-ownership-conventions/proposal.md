# Proposal: refactor20260927-adopt-ownership-conventions

> 属于 **refactor20260927 系列**。总纲、协作约定、全局依赖图与决策登记见 `refactor20260927-restructure-src-layers/design.md`。
>
> 本变更是系列中**唯一包含行为变更**的 change。D6–D9 四项行为变更已由用户于 2026-09-27 确认纳入一期,每项单独提交,可以单独回滚。

## Why

调研审计显示,仓库的所有权主干基本正确:裸 `new/delete` 几乎绝迹,`SessionRegistry → shared_ptr<SessionEntry> → unique_ptr<AgentLoop/SessionManager>` 这条链清楚,也已经有不少可以直接当范例的写法,比如 McpManager 的 weak_ptr 闭包、SubagentTrackerState、ContextLease、JoiningThreadGroup。

问题集中在**非拥有引用的生命周期没有在类型或结构上表达出来**:

- **依赖注入靠可空的裸指针**:`SessionRegistryDeps`、`WebServerDeps`、`SubagentToolDeps` 里的必填项也可以为空;先注册工具、后回填指针,拆除期间指针就会悬垂。
- **退出期 UAF 链**:
  - daemon 的拆除顺序与依赖方向相反,会话最后才停,MCP、LSP、WebServer 先被拆掉;
  - `SessionEntry` 的成员顺序导致 `ask_prompter` 先于 `loop` 析构;
  - `SubagentHost` 的 registry 最后析构;
  - control lambda 强持 `shared_ptr<SessionEntry>`,形成自持环。

  Desktop 关闭、切换工作区、headless 退出时都会触发。
- **回调与线程**:WS listener、opencode 导入线程等捕获 `[this]` 或 `[&]` 却没有守卫;`EventDispatcher::unsubscribe` 不等待正在执行的投递;约 15 处 `detach`;`vector<std::thread>` 只增不减。
- **共享可变配置**:AgentLoop 保存着指向共享可变 `AppConfig` 子对象的裸指针,worker 线程无锁读取,而设置路由会整份赋值——这是真实的数据竞争。
- **手工管理的资源**:`sqlite3*`、`void*` 句柄、写者租约靠手工配对释放,多出口路径容易漏。

用户的目标 3 是「数据传递尽量用好 RAII 和智能指针」。本变更把这些约定写成规则,提供对应的基础工具,并修掉已确认的问题。

## What Changes

- **所有权约定 C1–C14**,写进 AGENTS.md「所有权与生命周期」一节。每条约定配一个仓库里的真实反例,并由所有权棘轮 lint 度量(restructure 的 R15)。
- **基础 RAII 与并发原语**(P2-01,纯新增),放在 `base/utils`:`JoiningThread`、`JoiningThreadGroup`、`ReapingThreadSet`、`LifetimeToken` / `LifetimeRef`、`ScopeExit`、`AbandonableCall`(`run_abandonable` / `spawn_owned_detached` / `wait_for_abandoned_work`)、`AbortSignal`。必须在 restructure 的 P3 冻结之前合入。
- **订阅即资源**:新增 `EventDispatcher::unsubscribe_and_wait`,以及 move-only 的 `ScopedSubscription`。
- **不改 API 的修复**:
  - `SessionEntry` 显式析构;
  - control lambda 改捕获 `weak_ptr<SessionEntry>`;
  - headless 的订阅与回填指针收尾;
  - `SubagentHost` 析构安全;
  - src/web 内两处 UAF 最小点修(D5)。
- **句柄 RAII 化**:`UniqueHandle`、`UniqueFd`、`UniqueSqlite`、`UniqueProcess`、`WriterLease`;`Impl*` 改为 `unique_ptr`。
- **四项行为变更**(每项单独提交,提交摘要带 `[行为变更 Dn]`):
  - **D6 关停顺序**:TUI、daemon、headless 退出时,依次停止入口 → 停止回合生产者 → 停止全部会话 → 关闭 MCP → 关闭 LSP(O-04、O-05)。
  - **D7 会话资源真正释放**:会话销毁或宿主关停时,中止回合、完成最终元数据写入、删除写者租约;关停开始后拒绝新建与恢复会话;排队的控制任务被丢弃,等待方不再挂死(O-02、O-11)。
  - **D8 提示词相关配置以回合为单位生效**:自定义指令、项目指令配置、记忆配置、git 上下文配置,以及技能与专家快照,在回合开始时捕获一次;回合中途修改,从下一回合起生效(O-10)。
  - **D9 退出时有界等待**:可放弃的后台工作(models.dev 刷新、区域探测、进行中的 MCP 调用与连接、生图请求)最多等待 2 秒,三个入口都生效(O-07)。

## Capabilities

### New Capabilities
- `process-shutdown`:TUI、daemon、headless 进程退出时的关停顺序,以及对后台工作的有界等待(D6、D9)。
- `prompt-config-turn-snapshot`:提示词相关配置在回合内的生效时机(D8)。

### Modified Capabilities
- `session-lifecycle`:新增一条需求,规定会话销毁与宿主关停时要释放会话资源(D7)。只新增,不改动已有需求。

## Non-goals

- **二期**:ConfigStore、三入口共用组合根、`SessionRegistryDeps` / `WebServerDeps` 改为引用注入、`lsp` 与 `web_search` 单例改租约、TUI 主会话并入 registry。
- **D18**:不彻底改造 `ToolContext`。
- **D10**:三入口之间的能力差异不统一,例如 TUI 专属的 Deny 规则、只在 TUI 注册的 memory 工具。
- **D12**:疑似 bug 放二期 P8。

## Impact

- **代码**:
  - `base/utils`:新原语;
  - `base/platform`:unique_handle、TerminationSignal;
  - `domain/session`:event_dispatcher、scoped_subscription、writer_lease、session_manager;
  - `host/session_host`:session_registry、thread_service;
  - `apps/daemon/worker.cpp`、`apps/headless/headless_runner.cpp`;
  - `apps/tui`:subagent_host、关停序列;
  - `adapters/tool`:mcp_manager、image_generate、web_search;
  - `adapters/lsp`、`domain/sandbox`、`domain/security`;
  - `apps/web` 两处最小点修;
  - `engine/agent`:O-10 与 O-11 在 split-agent-loop 的 A-14 之后进行。
- **可观察行为**:见 specs。退出时被中断的工具结果会从「MCP/LSP 已关闭」这类错误变为 `[Aborted]`;退出最多多等 2 秒;设置页修改的提示词配置从下一回合起生效;退出后不再残留写者租约文件。
- **测试**:新增原语单测,以及析构期用例(挂起的提问、运行中的回合、排队的控制任务)、关停序列单测、配置快照用例;Linux 下跑 ASan / TSan。
- **文档**:AGENTS.md「所有权与生命周期」一节;CLAUDE.md 中关停顺序、会话租约相关描述同步更新。
