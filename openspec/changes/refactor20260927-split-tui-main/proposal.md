# Proposal: refactor20260927-split-tui-main

> 属于 **refactor20260927 系列**。总纲、协作约定、全局依赖图与决策登记见 `refactor20260927-restructure-src-layers/design.md`。本变更在系列 P3(目录搬迁)之后执行。拆分后的文件落在 `src/apps/cli/` 与 `src/apps/tui/` 下,少量纯逻辑下沉到 domain / host。

## Why

`src/main.cpp` 有 8827 行,是 TUI 的全部:入口分派、启动引导、1430 行的整帧渲染函数 `render_tui_frame`,以及约 3570 行的 `run_interactive_app`。后者用几十个按引用捕获局部变量的 lambda 串起了回调、线程和事件处理,状态分散在几组引用聚合里:

- `ChatScrollRuntime`:9 个引用成员;
- `TuiRendererContext`:24 个引用成员;
- `CatchEvent`:45 项引用捕获。

此外,这个文件里还有两套进程级全局、5 个手工 join 的线程、一段已经死掉的 IME 代码,以及与 `tui_helpers.cpp` 逐字重复的 23 个渲染 helper。其中两个 `g_model_load_percent` 写读的不是同一个变量,已经埋下静默失效的隐患。改任何 TUI 行为都要在 9000 行里找上下文,事件路由、帧内可变步骤、启动与关停顺序这些隐含约束,完全靠人记住。

## What Changes

- `main.cpp` 最终只剩 `int main` 与模式分派,不超过 80 行,放在 `apps/cli/`。CLI 分派、预 TUI 命令、进程环境,分别拆到 `apps/cli/` 下各自的文件。
- TUI 收成一个 `TuiApp` 对象。`run_interactive_app` 里按引用捕获的局部变量,都变成所有权明确的成员,分组如下:
  - 服务与启动:`TuiServices`;
  - 屏幕与绘制:`TuiScreenHost`、`ChatViewport` / `FrameGeometry`;
  - 与 AgentLoop 的桥:`TuiAgentBridge`、`TuiOverlayGate`、`TuiSubmitter`、`TuiTurnLifecycle`;
  - 输入与界面:`TuiEventRouter`、`FullScreenSurfaces`;
  - 后台与生命周期:`AnimationTicker`、各启动任务、`TuiShutdownSequence`。
- `render_tui_frame` 拆成 `render/` 下的若干只读视图,外加一个 prepare 阶段。帧内的可变步骤登记成表,顺序固定。
- 事件处理按「浮层 / 输入源 / 编辑键」拆到 `input/` 与 `overlays/`。`TuiEventRouter` 用一张逐键路由表保持原来的优先级链,每个 handler 返回三态结果:继续、已消费、交还给 FTXUI。
- 两套全局(`g_session_manager`、`g_active_screen`)改成 RAII 注册守卫;TUI 的线程(动画、自动标题、更新检查、Copilot 登录、模型池监控)改为自持,析构时 join。
- 删除与 `tui_helpers.cpp` 孪生的 helper,统一成一个 `g_model_load_percent`。`tui_helpers.cpp`(1386 行)按职责拆开。
- 纯逻辑(不依赖 ftxui)放进 `apps/tui/model/` 并补单测。
- **不改变任何用户可见行为。** 关停时让 SubagentHost 先于 MCP/LSP 停止,这一项属于行为变更(D6),由 adopt-ownership-conventions 的 O-05 单独负责。

## Capabilities

### New Capabilities
- 无。纯结构重构;`.openspec.yaml` 已设 `skip_specs: true`。

### Modified Capabilities
- 无。

## Non-goals

- 不修现存的不统一行为:picker 吞键矩阵、Home/End 的遮蔽关系、`set_callbacks` 分三次注入等,一律逐键原样保留。统一它们属于行为修复,放二期 P8。
- 不拆 `settings_center.cpp`(2842 行)、`management_center.cpp`(2146 行)、`builtin_commands.cpp`(2060 行),放二期 P6C。
- TUI 主会话不并入 SessionRegistry(D16);不做三入口共用组合根,放二期。
- TUI 的 `get_cwd` 仍是 ANSI 行为。改成 UTF-8 会改变 CJK 路径的 hash,放二期 P8,届时要提供旧 hash 回退。

## Impact

- **代码**:`src/apps/cli/**`、`src/apps/tui/{app,render,input,overlays,chat,composer,term,model,commands}/**`,新增约 60 个文件;下沉的纯逻辑有 `domain/skills/default_skill_startup`、`domain/permissions/default_rules`、`domain/session/composer_attachments`、`domain/history/input_history_recorder`、`host/session_host/auto_title_runner`、`base/platform/utf8_command_line`、`adapters/upgrade/upgrade_cli_args`。
- **CMake**:每一步新增的、需要单测的 `.cpp` 都要登记进 `ACECODE_TUI_TESTABLE_SUBSETS`;新增一条断言:凡是被 tests include 的 TUI 头,对应的 `.cpp` 必须在 testable 里。
- **测试**:新增 `apps/tui/model/**` 纯逻辑单测、表驱动的吞键特征测试、router 结构测试、启动快照测试。TUI 渲染与交互仍以手工回归为主,见 [manual-test-checklist.md](manual-test-checklist.md)。
- **文档**:CLAUDE.md 里「`renderable_tool_summary_line` 有两份同名实现」这一段改为单份;main.cpp 的行号锚点、ARCHITECTURE.md 中 TUI 入口的描述同步更新。
- **协作**:整体串行,同一时刻只允许一个任务修改 `apps/tui/app` 与 `apps/cli/main.cpp`;依赖 restructure 的 P3;B-11 需要 split-agent-loop 的 A-01;split-agent-loop 的 A-14 依赖本变更的 B-13。
