# Tasks: refactor20260927-split-tui-main

> **开工前必读**:
> - `refactor20260927-restructure-src-layers/design.md` §6「提交与协作约定」;
> - 本变更 design.md 的 §2–§7 与 §9「不变量清单」。
>
> **提交与认领**:
> - 提交前缀:`refactor20260927(tui-main/<任务编号>): …`。
> - 开工前在任务行末尾追加 `〔认领: <代理名> <日期>〕`,单独提交到 master。
>
> **执行顺序**:
> - 第 2–4 组整体串行,同一时刻只允许一个任务修改 `src/apps/cli/main.cpp` 与 `src/apps/tui/app/`。
> - 第 1 组在目录搬迁之前执行,使用旧路径 `src/main.cpp`。
>
> **每个任务的通用验证**:
> - 用例清单不变,外加新增用例;
> - Linux CI 与 Windows 本地构建 acecode 与 acecode_unit_tests;
> - 新增的、需要单测的 `.cpp` 在同一步登记进 `ACECODE_TUI_TESTABLE_SUBSETS`;
> - 新文件不超过 1000 行;
> - 跑 [manual-test-checklist.md](manual-test-checklist.md) 中与本任务对应的小节,结果写进提交说明。

## 1. 前置(可在 restructure 的 Phase 0 期间做,不依赖搬迁)

- [ ] 1.1 【P0-09】【主】去掉 main.cpp 中与 `tui_helpers.cpp` 孪生的 helper(MR-12)。
  - **先**把 `main.cpp:5878` 的写入目标改成 `acecode::tui::g_model_load_percent`,**再**删除 716-1150 与 987;
  - 约 30 处调用改为 `tui::` 限定;
  - `is_terminal_*` 包装改为直接调用 `tui::matches_terminal_*`,25 处一次性替换;`kTerminal*` 常量与 `is_alt_v/a` 迁到 `tui/terminal_key_event.hpp`;
  - **保留 200-203 的两条前置声明**,等 B-04 再删;删除 1621 行重复的 include 与 308 行的 `contains_box`;
  - 更新 CLAUDE.md「两份同名实现」一段,以及 `tui_helpers.cpp:497` 的注释。
  - 前置:restructure 1.8(P0-08)。
  - 验证:
    - 新增单测:写入 `tui::g_model_load_percent` 后,负载 chip 能渲染;中文注释写明回归现象是「负载 chip 永不显示」;
    - 手工逐个比对底栏的 chip,包括 token、缓存命中、模型负载;
    - 跑手工清单第 2、3 小节。
- [ ] 1.2 【P0-12】【主】【并】TUI 手工回归清单与启动时序记录。
  - 按 [manual-test-checklist.md](manual-test-checklist.md) 核对清单是否完整;
  - 记录现状下「原启动步骤 → 行号 → 新宿主」的完整表,存为本 change 目录的 `startup-order.md`(MR-3);
  - 录下四种启动场景(普通、`--resume`、Copilot 未登录、配置了 MCP)的 `state.conversation` 前 N 条快照,供 B-12 比对。
  - 验证:`startup-order.md` 覆盖 main.cpp 5259-6845 之间全部启动步骤,四份快照已存档。

## 2. 入口与启动(restructure 的 P3 之后开始)

- [ ] 2.1 【B-01】【子】基础设施。
  - 新增 `screen_port.hpp`(IScreenPort,含 `dimx()`)与 `fake_screen_port`;
  - 新增 `input/input_trace.hpp`:trace 函数只在 `#if` 内声明,调用点用宏包裹;`ACECODE_TUI_INPUT_TRACE` 对所有编译 TUI 源的目标生效,或改用 configure_file 生成的配置头;
  - `monotonic_milliseconds` 迁到 `redraw_pacer.hpp` 或 utils;
  - CMake 断言:凡是被 tests include 的 `apps/tui/**/*.hpp`,如果有对应 `.cpp`,该 `.cpp` 必须在 testable 中。
  - 前置:restructure 4.2(P3-02)。
  - 验证:trace 开和关两种配置都能构建;故意漏登记一个 `.cpp`,configure 报错。
- [ ] 2.2 【B-02】【子】CLI 入口外提。
  - `apps/cli/{process_environment,command_dispatch,pre_tui_commands}`、`adapters/upgrade/upgrade_cli_args`、`base/platform/utf8_command_line`,函数体逐字搬迁;
  - `reconcile_default_skills_on_startup` 移到 `domain/skills/default_skill_startup`(MR-2)。
  - 前置:2.1。
  - 验证:
    - 新增 `upgrade_cli_args`、`command_dispatch`、`utf8_argv` 单测;
    - `test.yml:118` 的 `--validate-models-registry` 步骤通过;`tests/scripts/verify_package_test.sh` 通过;
    - 手工验证 `--version`、`help`、`-p --help`、退出码 64 / 65、中文 `-p` 参数、Windows 双击启动。
- [ ] 2.3 【B-03】【主】启动引导外提。
  - 外提 `app/startup_environment`、`app/tui_runtime_init`;
  - `app/startup_worktree`:提供 `bootstrap_startup_worktree` 与 `finalize_session_worktree_on_exit(SessionManager&)` 两个 API,退出收尾只以 `SessionManager::active_worktree()` 为数据源;domain/worktree 只保留纯 git 操作;
  - 外提 `model/initial_state`、`domain/permissions/default_rules`(函数名带 tui)、`commands/command_bootstrap`;
  - 严格按 P0-12 的 `startup-order.md` 放置每一步。
  - 前置:2.2、1.2。
  - 验证:
    - 新增 `default_rules`、`startup_worktree` 单测;后者补一条「通过 EnterWorktree 进入、无变更、退出时删除」;
    - 新增 `initial_state` 单测;
    - 手工清单第 1、10 小节(横幅、`--worktree`、`--alt-screen`、conhost、hook 顺序)。

## 3. 视口、渲染与输入

- [ ] 3.1 【B-04】【子】纯逻辑叶子。
  - `domain/session/composer_attachments`、`domain/history/input_history_recorder`、`model/status_line`(完成后删掉 200-203 的前置声明)、`chat/message_render_revision`、`model/turn_lifecycle_rules`;
  - `render/frame_layout`:签名带上一帧 chat_box 的宽度(MR-15);
  - `populate_rewind_modes`。
  - 前置:2.3。
  - 验证:每个都有中文注释单测;`frame_layout` 覆盖上一帧宽度为 0 与非 0 两种情况。
- [ ] 3.2 【B-05】【主】ChatViewport / FrameGeometry 独占几何数据。
  - 删除 `ChatScrollRuntime` 与 `TuiRendererContext` 中对应的引用字段,以及 6 个视口 lambda。
  - 前置:3.1。
  - 验证:
    - 新增 `chat_viewport` 单测;
    - 手工验证:滚动、拖动滚动条、Ctrl+O、调整终端宽度、resume 后停在尾部、流式跟随(清单第 2、6 小节)。
- [ ] 3.3 【B-06】【主】只读视图。
  - header、activity、picker、prompt_status、link_hover;
  - 拆分 `tui_helpers.cpp`:status_chips、regular_sidebar_view、text_cells、thinking_phrases、input_wrap_view;`tui_helpers.hpp` 暂时保留为聚合转发头;
  - `prompt_status_view` 接收 `render_composer` 回调,只在常规分支调用(MR-6);
  - `regular_sidebar_view` 保持 `TuiState&` 签名;帧内顺序表登记侧栏 clamp 回写(MR-7)。
  - 前置:3.2。
  - 验证:
    - Screen 快照测试;tooltip 位置的表驱动测试;
    - ask / confirm 状态下 `input_hit_layout` 保持清空的断言;
    - 清单第 2、4 小节。
- [ ] 3.4 【B-07】【主】transcript、工具行、浮层与 frame_renderer。
  - prepare_frame_locked 加只读视图;
  - `hover_supported` 由 TuiScreenHost 探测后注入(MR-14);
  - `render_tui_frame` 与 `TuiRendererContext` 从 main.cpp 中消失。
  - 前置:3.3。
  - 验证:
    - overlay 与 tool_row 的快照测试,覆盖三种根布局;
    - 手工验证:调用行与结果行成对、diff、长 JSON 折叠、OSC8 链接、选区不漂移(清单第 3、6 小节)。
- [ ] 3.5 【B-08】【主】浮层输入。
  - **先**落地表驱动的吞键特征测试:所有键 × {无 picker / resume / model / mode / rewind / confirm},另外单列「Ctrl+E 在 picker 打开时仍会切换 tool_result」(MR-8);
  - handler 返回三态 `InputDisposition`(MR-1),只依赖 `input/ports.hpp`(MR-2);
  - 外提 5 个浮层 handler:ask_question_input、confirm_overlay_input、rewind_picker_input、completion_dropdown_input、list_picker_input;slash 与 @路径下拉进 testable。
  - 前置:3.4。
  - 验证:
    - 特征测试在改造前后都通过;
    - 新增单测:「ask 挂起时 Custom 事件返回 Declined,且不触达后续 handler」,用计数型 fake 断言;
    - 清单第 4 小节。
- [ ] 3.6 【B-09】【主】composer。
  - paste、suggestions、pending_attachment、edit_keys、submit、input_component、clipboard_keys;
  - unlock / lock 的区间逐行保持原样。
  - 前置:3.5。
  - 验证:FakeScreenPort 加注入剪贴板的单测;清单第 5 小节。
- [ ] 3.7 【B-10】【主】鼠标、按键与 TuiEventRouter。
  - 每个模块导出逐键的 handler,路由表逐行列出,带「原行号」列(MR-9);
  - 右键只由 mouse_router 在原位置调用;Ctrl+C 仍用 `post_event(Escape)` 回灌;
  - 事件所有权表加一行「全屏界面激活时 router 不运行」(MR-21);
  - 清理 `using namespace` 与 kTerminal 常量。
  - 前置:3.6。
  - 验证:router 结构测试断言到逐键函数这一粒度;清单第 6、7、8 小节。

## 4. 装配、生命周期与收尾

- [ ] 4.1 【B-11】【主】提交管线与 agent 桥。
  - TuiSubmitter、TuiCommandContextFactory(统一 6371 / 6410 / 7263 三处构造)、TuiAgentBridge、TuiOverlayGate、TuiTurnLifecycle;
  - 两阶段装配:先持有 accessor,之后 `attach(AgentLoop&)`,未 attach 时断言;通知窗口信息在调用时查询(MR-4);
  - `set_callbacks` 仍然分三次调用,保持原时序(MR-20);
  - `begin_user_turn_locked` 合并 7 处「新一轮等待」的重置之前,先附逐字段一致性证明。不一致的地方用参数保留差异;无法保留时,这部分就是【行为变更】,**停下来,交给用户拍板,再单独提交**(FR-12)。
  - 前置:3.7;split-agent-loop 的 2.1(A-01)。
  - 验证:
    - 工厂字段集合的断言测试;
    - 手工验证:一轮对话、重试、todo、goal、排队、通知、IM 远程、`/model`、revision 守卫(清单第 2、9 小节)。
- [ ] 4.2 【B-12】【主】启动任务与外部注册对象化。
  - 对象化 UpdateCheckTask、CopilotAuthTask、McpStatusBinding、ModelPoolMonitorSubscription、TuiNotificationBinding、InboundSubmitRegistration、AnimationTicker、FullScreenSurfaces、`host/session_host/auto_title_runner`;
  - 全部用 `optional` / `unique_ptr`,**在原步骤位置 emplace**;
  - AskUserQuestion 工具的注册(原 5431)仍在首回合之前(MR-3)。
  - 前置:4.1。
  - 验证:
    - 新增 `auto_title_runner`、`animation_tick` 单测;
    - 四种启动场景下,`state.conversation` 的前 N 条与 P0-12 快照逐条一致;
    - 清单第 1、9 小节。
- [ ] 4.3 【B-13】【主】TuiApp 成员化与关停序列。
  - 构造函数只做平凡初始化;`init()` 分阶段执行,异常时 scope guard 走同一个 TuiShutdownSequence(MR-5);
  - 按 design.md §2 补齐成员表(MR-18);
  - 主会话建立(5896-5921)与 AgentLoop 装配(5826-5857)作为命名步骤;SessionManager 的声明移到 AgentLoop 之前;
  - TerminalRestoreGuard 留在原位置注册;ConsoleCtrlHandlerRegistration 做成幂等(MR-17);
  - `g_session_manager` / `g_active_screen` 移到 process_guards;
  - 关停顺序按 design.md §5 逐条保持;SubagentHost 提前停止属于 O-05,不在本任务。
  - 前置:4.2。
  - 验证:
    - `tui_shutdown_sequence` 单测;
    - 对 `init()` 的每个阶段做异常注入,都不 terminate、不卡死;
    - `wc -l apps/cli/main.cpp` 不超过 80;
    - `check_line_coverage.py` 覆盖率 100%;
    - 手工清单第 10 小节,覆盖全部退出路径。

## 5. 验收

- [ ] 5.1 【P6B 完成】按 design.md §10 逐条核对:
  - 行数:main.cpp 不超过 80,新文件都不超过 1000;
  - 行号覆盖率 100%;
  - 所有权指标;
  - 用例清单;
  - manual-test-checklist.md 全部小节在三类终端上各跑一轮;
  - CLAUDE.md 中 main.cpp 行号锚点与 ARCHITECTURE.md 中 TUI 入口的描述已更新。
  - 验证:核对结果作为勾选依据,写进提交说明。
