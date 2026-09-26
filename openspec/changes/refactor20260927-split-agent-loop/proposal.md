# Proposal: refactor20260927-split-agent-loop

> 属于 **refactor20260927 系列**。总纲、协作约定、全局依赖图与决策登记见 `refactor20260927-restructure-src-layers/design.md`。本变更在该系列 P3(目录搬迁)之后执行,文件落在 `src/engine/agent/` 下。

## Why

`agent_loop.cpp` 有 6952 行,`agent_loop.hpp` 有 1010 行,是整个仓库最大的耦合点:

- 约 100 个公开方法和 25 类职责挤在一个类里;
- `execute_tool_calls` 单函数约 1490 行;
- `run_agent_with_input` 约 905 行;
- `execute_tool_calls` 里 `[&]` 捕获几十个局部变量,并行只读线程也在读写;
- 回合级状态散落在成员与栈上,靠手工复位;
- `set_session_manager` / `set_*_config` 等延迟裸指针注入,其中 `set_*_config` 指向共享可变 AppConfig 的子对象,已经构成数据竞争。

改任何一处都要在 7000 行里找上下文,CLAUDE.md 记录的不变量(prompt cache 前缀、审批门唯一入口、单写者、锁序)也全靠人记。拆开之后,每个职责有自己的文件和类型,不变量由类型与 lint 守住。

## What Changes

- 把 `AgentLoop` 拆成门面加约 60 个协作文件,每个不超过 1000 行,目标 200–600 行。分布在以下子目录:
  - `request/`(请求组装,prompt cache 敏感区)、`model_step/`(provider 流式调用与用量记账)、`recovery/`(溢出恢复与 PA 接触点);
  - `tool_exec/`(工具批次调度与生命周期)、`approval/`(唯一审批入口)、`hook_bridge/`;
  - `turn/`(回合编排与收尾)、`worker/`(任务队列)、`control/`、`transcript/`(messages_ 单写者);
  - `goal/`、`compaction/`、`side_question/`、`progress/`、`boundary/`、`guards/`、`event_payload/`、`detail/`。
- 门面只做编排:`agent_loop.cpp` 不超过 900 行,`agent_loop.hpp` 不超过 450 行。hpp 只前置声明协作对象,TUI 与 registry 不再被 20 来个重头拖累。
- 回合级状态收进由 worker 持有的 `TurnContext`,取代逐字段手工复位。
- 手工配对的生命周期改成 RAII:`ActiveProviderScope`、`BusyCycleScope`、`SessionLease`、`SynchronizedDoomGuard`、`ToolStreamProgress`,worker 线程改为 `JoiningThread`。
- 新增构造注入 `AgentLoop(AgentLoopServices, AgentLoopOptions)` + `start()`,取代 `set_session_manager` / `set_hook_manager` 等延迟注入;全部调用方与测试迁移后,删除旧构造与旧 setter。
- `messages_mut()` 删除,唯一调用方改为 `history_on_worker(fn)`。`cwd()` 改为按值返回,避免悬空引用。
- **不改变任何用户可见行为。** 原计划 A-15(回合级配置快照,D8)与 A-16(关停后清空队列,D7)都属于行为变更,已移入 `refactor20260927-adopt-ownership-conventions`,编号为 O-10、O-11。

## Capabilities

### New Capabilities
- 无。纯结构重构;`.openspec.yaml` 已设 `skip_specs: true`。

### Modified Capabilities
- 无。

## Non-goals

- 不修调研中发现的疑似 bug,例如进度 key 用 `"\0"` 拼接、`stop_hook_active_` 跨回合残留。纯搬迁阶段原样保留,归二期 P8。
- 不彻底改造 `ToolContext`(D18):不动它的形状,只加文档标注、AbortSignal 兼容指针与工厂收口。
- 不改变 side question 的线程模型(D11):仍然一个请求一个线程,只把线程表换成可回收的。
- 不做三入口组合根与 ConfigStore,放二期。

## Impact

- **代码**:`src/engine/agent/**`(新增约 60 个文件)、`src/adapters/pa/pa_rescue_driver.*`、`src/adapters/computer_use/session_lease.hpp`;调用方有 `host/session_host/session_registry.cpp`(make_entry)、`thread_service.cpp`(history_on_worker)、`apps/tui/app/*`(随 split-tui-main 的 B-13)。
- **测试**:`tests/agent/` 新增纯函数单测与 `agent_loop_fixture.hpp`;约 17 个用例从旧 setter 迁到 fixture。
- **文档**:CLAUDE.md 中指向 `agent_loop.cpp` 行号的描述改为指向新文件;`src/layers.tsv` 回填 R11 单出口白名单的实际文件。
- **协作**:整体串行,同一时刻只允许一个任务改 agent 目录下正在拆的文件;依赖 restructure 的 P3 完成;A-14 依赖 split-tui-main 的 B-13。
