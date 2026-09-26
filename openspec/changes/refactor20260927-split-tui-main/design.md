# Design: refactor20260927-split-tui-main

> **行号基准**:master `7942011b` 的 `src/main.cpp`。restructure P2-08 会先把它 R100 移到 `src/cli/main.cpp`,P3 之后位于 `src/apps/cli/main.cpp`。行号漂移时按函数名定位。
>
> **路径约定**:不带前缀的路径相对 `src/apps/tui/`。
>
> 总纲与协作约定见 `refactor20260927-restructure-src-layers/design.md` §6,提交前缀 `refactor20260927(tui-main/<任务>)`。

## Context

main.cpp 的结构(调研实测):

| 行号 | 内容 |
|---|---|
| 1-204 | 191 个 include、文件级 `using namespace ftxui; using namespace acecode;`、前置声明 |
| 205-2752 | 匿名命名空间 helper:ask 浮层输入适配(216-694);与 tui_helpers.cpp 孪生的渲染 helper(716-1150);进程与终端 helper;全局 `g_session_manager` / `g_active_screen`(1311-1317)与信号处理;IME 死代码(1395-1617);状态行与剪贴板文案;聊天滚动 runtime(1781-2034);粘贴;confirm / rewind / slash / @路径各浮层的事件处理 |
| 2753-3803 | CLI 分派(2767-2976)、预 TUI 命令、启动引导(3028-3711)、主循环与关停(3314-3456) |
| **3804-5237** | **`render_tui_frame`,约 1430 行** |
| 5237-5257 | `int main` |
| **5259-8827** | **`run_interactive_app`,约 3570 行**:服务装配、AgentCallbacks 回调、自动标题线程、提交管线、子代理接线、启动 resume、动画线程、巨大的 CatchEvent(6845-8636)、全屏界面、主循环收尾 |

- **引用捕获**:`ChatScrollRuntime` 9 个引用成员,`TuiRendererContext` 24 个引用成员,`CatchEvent` 45 项引用捕获。
- **测试**:TUI 大部分只能手工验证。CMake 用正则把 `src/tui/`、`src/markdown/` 剔出 acecode_testable,需要单测的文件必须登记进显式清单。
- **半抽取副本**:仓库里还有一套未接线、已漂移的副本(tui_init / cli_dispatch / agent_callbacks_builder / tui_context / terminal_utils 等),由 restructure 的 P0-08 删除。

## Goals / Non-Goals

**Goals:**
- `apps/cli/main.cpp` 不超过 80 行;本变更新建或触及的 `apps/tui/{app,render,input,overlays,chat,composer,term,model}` 下,每个文件不超过 1000 行,目标 200–600 行。
- `run_interactive_app` 里的所有引用捕获都变成有明确所有者的成员;全局改成 RAII 注册;线程自持。
- 事件路由、帧内可变步骤、启动顺序、关停顺序四张表写进本文,作为实施与评审的对照。
- 原 main.cpp 的每一行都有去处:用 `check_line_coverage.py` 检查,覆盖率 100%。

**Non-Goals:**
- 不改任何用户可见行为。现存的不统一也逐键保留,包括吞键矩阵、Home/End 遮蔽、`set_callbacks` 分三次调用、TUI cwd 的 ANSI 行为。
- 关停时 SubagentHost 先于 MCP/LSP 停止,属于 O-05(D6),不在本变更。

## Decisions

### 1. 对外契约(拆分前后逐字节一致)

1. **acecode 可执行文件的 CLI 表面**:
   - 分派顺序:`--remote-web-proxy` → version → help → upgrade|update → `--apply-update` → daemon → service → `--service-main` → channels → `-p` → Windows 双击自启 → TUI。
   - 退出码:用法错误 64;非 Windows 下的 service 65;`-p` 为 0/1/64/130。
   - 文案:`acecode v` + 版本号;`models.dev registry OK: ...`。`.github/workflows/test.yml:118` 与 `tests/scripts/verify_package_test.sh` 依赖这两段文案。
   - `-p` 在 Windows 上经 `CommandLineToArgvW` 重建 UTF-8 argv;`-p --help` 优先于用法报错。
2. **进程级副作用与顺序**:
   - `configure_process_environment` 是 main 的第一句;
   - `startup_before_model_load` 早于 load_config → `startup_models_loaded` 在 provider 安装之后 → `session_start` → `title_changed`;
   - 日志初始化之后立即补记 deferred 告警;proxy 早于任何 cpr 调用;`tool_rewrites::load_and_apply` 早于第一次 register_tool;`web_search::init` 早于 builtin 注册;
   - 所有工具在首回合之前注册完,包括启动 resume 触发的 goal 回合。
3. **退出行为**:
   - 关停序列 20 余步的顺序不变(见 §5 表);
   - 打印 resume 提示;
   - `--worktree` 退出清理:无变更静默删除,有变更或数不清时保留;
   - Windows 的 Ctrl+C / BREAK / CLOSE 语义不变;POSIX SIGINT/SIGTERM 在 Loop 之外 finalize,然后 `_exit(1)`。
4. **屏幕与键鼠路由**:屏幕上的一切,以及键鼠路由语义,包括现存不统一的部分,全部保持。
5. **仓库内依赖的接口保持不变**:
   - `TuiState` 的字段与锁约定,只修正 124、200-207、249-252 三处过期注释;
   - `CommandContext` 结构;
   - `rewind_callback` 与 picker callback「持锁调用、内部只用 `*_locked`」的合约;
   - `SubagentHost` / `ask_via_tui_overlay` / AgentLoop 公共 API / `SessionModelBinding` 以及各进程单例的 API;
   - `tui_helpers.hpp` 中已被 tests 使用的签名:拆分时先把它保留为聚合转发头,等测试 include 迁完再去掉。
   - 新代码不引入 `using namespace`,不新建命名空间,仍然放在 `acecode::tui` 里。

### 2. TuiApp 的所有权

- **构造与初始化分离**(MR-5):
  - 构造函数只做平凡初始化;
  - 分阶段装配放进 `run()` 里的 `init()`,每完成一个阶段就登记一次;
  - `init()` 抛异常时,scope guard 走同一个 `TuiShutdownSequence`,跳过还没到达的步骤。

  这样设计的原因:构造函数抛异常时,对象自己的析构函数不会运行。而 AgentLoop 一构造就起 worker 线程,如果靠析构兜底,异常路径上 worker 会卡在 join。
- **成员分组与声明顺序**:声明顺序就是析构的逆序;正常路径由显式的 TuiShutdownSequence 负责关停。
  - **A 启动环境与服务**:`TuiLaunchOptions` → `StartupEnvironment` → `unique_ptr<TuiServices>`。TuiServices 内部的成员顺序是:HookManager → AppConfig → SessionModelBinding → ToolExecutor → SkillRegistry → MemoryRegistry → McpManager → WorkspaceRegistry → WorkspaceToolDeps → SkillUsageStore。AppConfig 必须早于 ToolExecutor,因为 skills 工具持有 `&config`。
  - **B UI 状态与屏幕**:`TuiState` → `TuiScreenHost`(ScreenInteractive 与 ActiveScreenRegistration 紧挨着;redraw_pacer 与 last_keyboard_input_at_ms 也归它)→ `ChatViewport` → `FrameGeometry`。
  - **C 会话与 agent**:TokenTracker → PermissionManager → **SessionManager**(从 AgentLoop 之后前移到之前,让持有其指针的 AgentLoop 先析构)→ TurnObservation → agent_aborting → TuiSubmitter → TuiOverlayGate → TuiTurnLifecycle → AutoTitleRunner → TuiAgentBridge → `unique_ptr<AgentLoop>` → `unique_ptr<SubagentHost>` → CommandRegistry → TuiCommandContextFactory。
  - **D 进程与后台注册**:ModelPoolMonitorSubscription、SessionFinalizeRegistration、ConsoleCtrlHandlerRegistration、TuiNotificationBinding、InboundSubmitRegistration、McpStatusBinding、UpdateCheckTask、CopilotAuthTask、AnimationTicker。**一律用 `optional` 或 `unique_ptr`,在原启动步骤的位置 emplace**(MR-3)。声明顺序只决定析构逆序,不能靠声明位置决定启动时机。
  - **E 组件树**:TuiInputContext → TuiEventRouter → input_component → TuiFrameRenderer → FullScreenSurfaces → root。
  - **补齐的状态**(MR-18):`auth_done`、`mcp_first_turn_wait_done`、`version_str` / `cwd_display`、`root_surface_index`。TuiInputContext / TuiCommandContextFactory 对 `IFullScreenSurfaces` 的引用,注明「只在事件期间使用,构造期禁止调用」。
- **两阶段装配**(MR-4):TuiSubmitter / TuiTurnLifecycle / TuiOverlayGate 先持有 accessor,等 AgentLoop 构建完成后再调用 `attach(AgentLoop&)`;未 attach 时的调用用断言兜住。通知窗口信息在调用时向 TuiNotificationBinding 查询,不再按值捕获。这是回调捕获规则里明确允许的例外。
- **回调注入保持原时序**(MR-20):AgentLoop 构造时传入第一版回调,另外两次 `set_callbacks` 在原步骤位置执行。改成一次注入属于行为修复,放二期。
- **全局量**:
  - `g_session_manager` → process_guards 中唯一的 `atomic<SessionManager*>`,只由 SessionFinalizeRegistration 写入;atexit 用 `call_once` 注册一次。
  - `g_active_screen` → 同一文件里唯一的 atomic,只由 ActiveScreenRegistration 写入。控制台 Ctrl 线程是唯一必须读全局的消费者;模型池回调改为持有 `weak_ptr<UiPostTarget>`。
- **进程级注册的原位保留**(MR-17):
  - `TerminalRestoreGuard`(atexit `reset_cursor`)在 `ensure_interactive_terminal` 的原位置用 `call_once` 注册;
  - `ConsoleCtrlHandlerRegistration` 维护 `registered_` 标志,显式 release 之后析构不再调用 FALSE。
- **线程**:动画、自动标题、更新检查、Copilot 登录、模型池监控一律自持,析构时 join,依赖 ownership 的 P2-01 `JoiningThread`。Copilot 认证线程改持 `shared_ptr<CopilotProvider>` 属于 O-07,本变更只迁移位置。
- **电源锁**:魔法串 `"tui-main"` 收成常量 `kTuiMainPowerSessionId`。

### 3. 输入路由(`app/tui_event_router` + `input/` + `overlays/`)

- **三态返回**(MR-1):`enum class InputDisposition { Continue, Consumed, Declined }`。Router 遇到 Consumed 或 Declined 就终止,分别返回 true / false。现有代码中「停止链并把 false 交还给 FTXUI」的分支必须映射成 Declined,例如:
  - 输入框指针按下返回 `press.event_consumed`;
  - ask 守卫对 Custom / cursor_position 返回 false;
  - 非聊天区的鼠标返回 false。
- **handler 只依赖接口**(MR-2):`input/ports.hpp` 定义 `ITurnSubmitter`、`ICommandContextFactory`、`IFullScreenSurfaces`,`screen_port.hpp` 定义 `IScreenPort`,其中包含 `dimx()`(MR-13)。TuiInputContext 只持有接口,具体实现留在 `app/`。原因:acecode_testable 是 OBJECT 库,testable 对象只要引用到只在 exe 中定义的符号,链接就会失败。
- **逐键路由表**(MR-9):每个模块导出逐键的 handler 函数,路由表逐行列出,并带「原行号」一列。原顺序如下:

  prelude → bracketed paste → Ctrl+V → Alt+V → Ctrl+C → 待发附件焦点 → 输入框指针按下 → ask 守卫 → 远程确认泵 → confirm → rewind → @路径 → slash → Enter → picker 翻页 → 聊天 PgUp/PgDn → Alt+↑/↓ → Home/End → Esc → Tab/Shift+Tab → 鼠标 → 编辑键 → false

  需要特别注意的交错:
  - Ctrl+A、VT220 Home、Ctrl+O、Ctrl+E、End 在原链上交错排列;Ctrl+E 未消费时 fall through;
  - 右键复制/粘贴只能由 mouse_router 在原位置调用,也就是鼠标块内、「Pressed 即隐藏气泡」之后;
  - picker 的 Enter 在 Enter 分支开头;
  - picker 的 Esc 在「隐藏气泡 + 复位拖选」之后;
  - Tab 在非 mode picker 时 fall through;
  - Ctrl+C 忙时用 `PostEvent(Escape)` 回灌同一路由。
- **吞键矩阵**(MR-8):先落地表驱动的特征测试,再抽 handler。测试覆盖所有键 × {无 picker / resume / model / mode / rewind / confirm},并单列一条「Ctrl+E 在 picker 打开时仍会切换聚焦的 tool_result」。
- **事件所有权的边缘路径**(MR-21):全屏界面(设置中心、管理中心)激活时,由它们独占事件,router 不运行。现状是这期间远程确认泵暂停,子代理的权限请求要等关闭设置页后才弹出。这一行为保持不变,并写进手工回归清单。

### 4. 渲染(`render/` + `app/tui_screen_host`)

- `render_tui_frame` 与 `TuiRendererContext` 从 main.cpp 中消失,由 `render/frame_renderer` 编排:先执行 prepare 阶段,也就是帧内可变步骤;再调用只读视图。
- **帧内可变步骤表**(MR-6、MR-7):整帧持有 `state.mu`,顺序固定为:
  1. `sync_from_layout` → `clamp_focus` → 选区锚点补偿(focus 与 offset 都没变且 y 有效时才 ShiftSelection;哨兵值 -999999)→ 清空本帧的 vector;
  2. 视图构建期间允许的写入:
     - transcript 的 `message_render_cache.store` 与 `chat_link_regions` 收集(原 4061-4066);
     - ask 布局回写(原 4886-4897)与 `ask_question_frame.reset_for_render`(原 4842);
     - **composer 的 `Render()` 只在常规分支调用**:`prompt_status_view` 接收 `render_composer` 回调;ask / confirm 状态下,`input_hit_layout` 必须保持清空;
  3. 根布局阶段:**侧栏 clamp 回写** `state.sidebar_scroll_top_row`。`regular_sidebar_view` 保留 `TuiState&` 签名并加注释说明;
  4. 每条消息同时 `reflect_unclipped`(layout box)和 `reflect`(裁剪后的 box)。
- `hover_supported` 由 TuiScreenHost 探测一次后注入 renderer(MR-14);`osc8` 仍在 renderer 内部探测。
- `frame_layout` 的纯函数签名带上一帧 chat_box 的宽度(MR-15):`compute_frame_layout(term_w, conhost, prev_chat_box_width)`。
- 重绘节拍:`begin_frame` 在渲染之前;`complete_frame` 经 Post 在本帧 Draw/Flush 之后执行。

### 5. 启动与关停顺序表

**P0-12** 记录「原启动步骤 → 行号 → 新宿主」的完整表,**B-03 / B-12 按表执行**,表格存档在本 change 目录的 `startup-order.md`。关键约束:
- UpdateCheck(5425)、`start_mcp_servers_async`(5431)、AskUserQuestion 工具注册(5431)、Copilot 认证(5486-5557)都早于 AgentLoop 构造(5826);model_pool(5859-5893)在 AgentLoop 之后、SessionManager 主会话建立(5896-5921)之前;atexit / ctrl handler(6188-6196)在启动 resume(6198-6341)之前;RC 入站(6612)与动画线程(6638)在最后。
- **主会话建立**(5896-5921)与 **AgentLoop 装配**(5826-5857)是两个命名步骤,不能笼统并进 TuiApp。其中裸指针的来源顺序关系到 prompt 前缀的字节稳定。
- **关停顺序逐条保持**:model_pool stop → 自动标题线程 join → shutdown_notifications → 清 active screen → 撤销 console ctrl handler → 动画停止 → RC stop → agent_aborting=true + abort + 持锁唤醒 confirm_cv(Deny)与 ask_cv,清 remote_confirm_queue,overlay_cv.notify_all → `agent_loop.shutdown` → 释放 `"tui-main"` 电源锁 → MCP shutdown → `lsp::shutdown` → compact 线程 → anim / auth / update join → worktree 收尾 → finalize → cleanup_old_sessions → 清 session 指针 → 打印 resume 提示。
- **worktree 收尾的数据源**(MR-10、FR-8):
  - `app/startup_worktree` 提供两个 API:启动引导 `bootstrap_startup_worktree`,以及退出收尾 `finalize_session_worktree_on_exit(SessionManager&)`;
  - 退出收尾只以 `SessionManager::active_worktree()` 为数据源,覆盖三种来源:`--worktree` 启动、会话中途 `EnterWorktree`、`--resume` 恢复进入的;
  - domain/worktree 只提供纯 git 操作,例如 `count_worktree_changes`、`remove_worktree`。

### 6. CMake 与可测性

- **每一步都登记**(MR-11):新增的、需要单测的 `.cpp` 必须在同一步登记进 `ACECODE_TUI_TESTABLE_SUBSETS`。B-01 新增断言:凡是被 tests include 的 `apps/tui/**/*.hpp`,如果有对应的 `.cpp`,该 `.cpp` 必须在 testable 中。
- **只在 exe 里的符号**(MR-2):testable 对象引用到的符号,其定义不能只存在于 exe 中。`reconcile_default_skills_on_startup` 移到 `domain/skills/default_skill_startup`。handler 只依赖 `input/ports.hpp` 里的接口。
- **ftxui 边界**:`apps/tui/model/` 下的文件不得依赖 ftxui(R6),全部进 testable。
- **`ACECODE_TUI_INPUT_TRACE`**(MR-16、FR-15):trace 函数只在 `#if` 内声明,调用点用宏包裹;同时让这个定义对所有编译 TUI 源的目标生效,或改用 configure_file 生成的配置头,避免 ODR 违规。

### 7. 删除与去重

- **与 tui_helpers.cpp 孪生的 23 个渲染 helper**(716-1150,P0-09):
  1. 先把 5878 行的写入目标改成 `acecode::tui::g_model_load_percent`,否则负载 chip 会永远不显示,而编译和测试都不会报错;
  2. 删除 main.cpp:987 的匿名 `g_model_load_percent`;
  3. 约 30 处调用改为 `tui::` 限定;
  4. 更新 CLAUDE.md「两份同名实现」一段,以及 `tui_helpers.cpp:497` 的注释。
- **按键谓词的薄包装**:`is_terminal_key` / `is_terminal_codepoint`(514-522、696-709)改为直接调用 `tui::matches_terminal_*`,25 处调用一次性替换;`kTerminal*` 常量与 `is_alt_v/a` 迁到 `tui/terminal_key_event.hpp`。
- **前置声明**:**保留 200-203 的两条前置声明**(MR-12),等 B-04 把 status_line 外提后再删;删除 1621 行重复的 `#include "tui_state.hpp"`,以及 308 行从未使用的 `contains_box`。
- **薄适配层**:6815-6835 的 5 个 lambda 薄适配层、5461-5486 的 6 个视口 lambda,随 ChatViewport 与 handler 成员化一起删除。
- IME 死代码与半抽取副本由 restructure 的 P0-08 删除。

### 8. 目标文件表

行号指原 main.cpp;不带前缀的路径相对 `src/apps/tui/`。

| 目标文件 | 来源 | 行数 |
|---|---|---|
| apps/cli/main.cpp | 5237-5257 | ≤80 |
| apps/cli/process_environment | 1155-1175、2756-2765 | 90 |
| apps/cli/command_dispatch | 2767-2976 | 280 |
| apps/cli/pre_tui_commands | 2978-3026 | 90 |
| adapters/upgrade/upgrade_cli_args | 2840-2887 | 80 |
| base/platform/utf8_command_line | 2925-2958 | 60 |
| domain/skills/default_skill_startup | 1176-1208 | 80 |
| app/tui_app | TuiApp;`run()` 内分阶段的 `init()` | 560 |
| app/tui_services | 5259-5338、3169-3179、3553-3665 | 320 |
| app/tui_runtime_init | 3055-3122、3152-3167 | 200 |
| app/startup_environment | 3028-3053、3529-3551 | 120 |
| app/startup_worktree | 3458-3528(bootstrap)、3411-3443(`finalize_session_worktree_on_exit`) | 160 |
| model/initial_state | 3181-3224、3249-3270、3667-3684 | 120 |
| domain/permissions/default_rules(函数名带 tui) | 3272-3294 | 70 |
| commands/command_bootstrap | 3296-3312 | 50 |
| app/process_guards | 1310-1393、6188-6196 | 230 |
| term/terminal_control | 1210-1265 | 130 |
| input/input_trace.hpp | 1267-1308,加 16 处 trace 块 | 160 |
| screen_port.hpp(含 dimx) | 新增 | 70 |
| app/tui_screen_host | 5383-5424、3686-3711、8637-8685 中 redraw_pacer 的帧计时 | 260 |
| chat/chat_viewport、chat/message_render_revision | 1781-2034、5340-5381、5450-5485;1802-1850 | 380 / 90 |
| render/frame_geometry.hpp、frame_renderer、frame_layout | 3713-3930、5142-5235、8637-8685 中 TuiRendererContext 的装配 | 80 / 340 / 120 |
| render/header_view、transcript_view、message_row_views | 3932-3976;3978-4123、4424-4515;4170-4217 | 90 / 420 / 260 |
| render/tool_row_view、activity_indicator_view、picker_views | 4124-4169、4218-4423;4517-4614;4616-4830 | 280 / 130 / 230 |
| render/overlay_views、prompt_status_view、link_hover_tooltip | 4832-4975;4977-5140;3756-3801 | 200 / 220 / 90 |
| render/status_chips、regular_sidebar_view、text_cells;model/thinking_phrases;composer/input_wrap_view | 从 tui_helpers.cpp(1386 行)拆出 | 260 / 450 / 140 / 90 / 360 |
| input/tui_input_context.hpp + input/ports.hpp | 45 项引用捕获 | 130 + 60 |
| app/tui_event_router | 6845-8636 的骨架;6815-6835 的 lambda 适配器删除 | 230 |
| overlays/ask_question_input、confirm_overlay_input、rewind_picker_input | 216-694;2287-2380、7061-7087;2382-2589 | 480 / 180 / 230 |
| overlays/completion_dropdown_input、list_picker_input | 2591-2751;7108-7169 等 6 处 | 190 / 300 |
| model/status_line;composer/composer_paste、input_suggestions | 1624-1680、1759-1779;2036-2203 | 120 / 260 / 60 |
| composer/pending_attachment_input、composer_edit_keys、composer_submit、input_component | 2205-2285、7000-7035;8196-8634;7104-7338;6790-6814 | 130 / 320 / 280 / 80 |
| input/clipboard_keys、interrupt_keys、chat_view_keys | 6910-6951、7651-7686;6952-6998、7521-7635;7400-7520 | 170 / 220 / 150 |
| input/mouse_router、scrollbar_drag_input、chat_link_input | 7636-7740、8094-8195;7742-8057;8058-8093 | 200 / 300 / 130 |
| domain/session/composer_attachments、domain/history/input_history_recorder | 1708-1757;7203-7218 | 90 / 70 |
| app/tui_submitter、tui_command_context | 5435-5448、6037-6106;6371 / 6410 / 7263 三处 CommandContext 构造 | 230 / 110 |
| app/tui_agent_bridge、tui_overlay_gate、tui_turn_lifecycle | 5559-5821、6429-6474;5649-5679、5829-5839;6476-6606 | 460 / 140 / 260 |
| model/turn_lifecycle_rules、animation_tick | 判定纯函数;6636-6788 中持锁的部分 | 130 / 190 |
| host/session_host/auto_title_runner | 5922-6035 | 250 |
| app/tui_startup_tasks | 3123-3146(UpdateCheckTask)、3225-3246(McpStatusBinding)、5425-5557、5859-5893 | 300 |
| app/tui_subagent_wiring、tui_startup_resume | 6108-6186;6198-6341 | 150 / 230 |
| app/tui_notification_binding、tui_remote_control_binding、animation_ticker | 6343-6427;6608-6634;6636-6788 | 120 / 90 / 170 |
| app/full_screen_surfaces、tui_lifecycle | 6836-6843、8686-8814;3314-3456、8815-8827 | 220 / 230 |

子目录名避开了模块名(R9):用 `term/`,不用 platform;`model/` 不是模块名。

## 9. 不变量清单(逐条守住)

1. `configure_process_environment` 是 main 的第一句;CLI 分派顺序、退出码、文案、UTF-8 argv 重建、双击自启判定(argc==1 且 GetConsoleProcessList==1)都不变。
2. **启动顺序**:
   - worktree 早于 logger;deferred 告警紧跟日志初始化;
   - before_model_load hook 早于 load_config;environment::bootstrap 紧随 load_config;
   - proxy 早于任何 cpr 调用;provider 只经 `SessionModelBinding::install_explicit` 发布;
   - tool_rewrites 早于第一次 register_tool;web_search 早于 builtin 注册;
   - 所有工具在首回合之前注册完,工具表变化会打穿 prompt cache 前缀;
   - D 组对象在原步骤位置启动。
3. TUI 专属的 7 条 priority=100 Deny 规则只作用于 TUI 主会话;`.acecode/rules/**` 的 priority=1000 内置保护不受影响。
4. memory 目录创建失败时,只把运行时副本设为 `enabled=false`,不回写 config.json;`--question-policy` 只写 `*_cli` 运行时字段。
5. 关停顺序逐条保持(§5)。`--worktree` 退出时先 chdir 回 original_cwd 再 remove;有变更或数不清时一律保留(fail-closed)。
6. **Windows 控制台**:
   - `prepare_windows_ctrl_c_handling_after_ftxui_install` 是 Loop 的第一个 Post 任务;
   - CTRL_C 转成 PostEvent(CtrlC);BREAK / CLOSE 必须调用 `screen->Exit()`,不能返回 FALSE;
   - active screen 在 ScreenInteractive 析构之前清空;
   - POSIX 的 signal_handler 只在 Loop 之外生效。
7. bracketed paste 开关与输入缓冲 flush 包住 Loop 两侧;`write_terminal_control_sequence` 写完后恢复 console mode。
8. 帧内顺序与可变步骤见 §4。`message_render_revision` 包含 `transcript_expanded`,不包含 content;测量 pass 与布局共用同一实现;L1 缓存只缓存不含链接的消息,content 哈希只进缓存键。
9. follow-tail 用 `focusPositionRelative(0,1)`;短对话底部锚定;clamp / scroll 之后,`follow_tail ⇔ top_row ≥ max_scroll_top`。
10. 侧栏:宽度大于 120 列且不是 conhost 才显示,宽 43;隐藏时三个 box 写倒置哨兵 {1,0,1,0},并复位侧栏的滚动与拖拽状态。
11. **工具行**:
    - 三态灯由调用与结果的配对元数据得出;Ctrl+O 才显示参数;结果前缀 `  └ `;摘要行没有 icon;
    - fold 按当前宽度折叠成 3 个可视行,展开后以 2000 个硬行为上限;
    - diff 最多 3 块、每块 20 行;diff 与 summary 的 metric 拼接规则不可合并;
    - task_complete 与 AskUserQuestion 的结果永远显示全文。
12. 活动指示:tool_running 优先于思考动画;心跳只在 thinking_start_time 不是纪元原点时显示;conhost 不渲染思考行。
13. 事件路由顺序与三态返回见 §3;吞键矩阵与 Home/End 的遮蔽关系逐键保持。
14. **Ctrl+C 与 Esc**:
    - 非 Ctrl+C 的按键取消退出武装;忙时用 PostEvent(Escape) 回灌;
    - Esc 依次:先隐藏气泡、复位拖选,再 picker 取消 → Shell 退出 → 清附件 → 中断。
15. **各浮层**:
    - slash:Enter 补全后返回 false;Tab 被吞;Esc 置 dismissed_for_input。
    - confirm:数字键即下标;No 永远在最后;a / Shift+Tab 只在有 AlwaysAllow 时生效;Esc 等于 Deny;子会话的请求不 notify confirm_cv;释放时 notify_all overlay_cv。
    - rewind:`rewind_callback` 持 state.mu 调用;`/fork` 直接走 ConversationOnly。
16. **粘贴两段加锁**:读剪贴板、存附件、ensure_active_session_id 时不持 state.mu,重新加锁后再判断 can_accept;`next_paste_id` 不复位;任何输入变化都调用 `cancel_ctrl_c_exit_locked`。
17. **提交**:
    - Enter 的守卫在清空 composer 之前执行;
    - 斜杠命令分派与 MCP 协调期间释放 state.mu;
    - submit 本身在持锁状态下被调用,自身不得同步加 state.mu;
    - RC 入站与 Enter 分支的锁内逻辑一致;
    - 「新一轮等待」的五个字段成组重置(合并 7 处重置属于 B-11,必须附一致性证明)。
18. **回调**:
    - on_message 剥掉 `<text_preamble>`,并原位替换尾部的 assistant;
    - tool_call 行在 push 时就计算 display_override;
    - on_delta 经 pacer 调度,不逐 token PostEvent;进度 update 150ms 节流,end 无条件 PostEvent。
19. **Done 伪行与通知**:
    - Done 伪行只在 was_waiting 且 !busy、非用户中断、耗时 ≥1s 时追加,不进 LLM context 或 JSONL;
    - 完成通知只在「已完成、未中断、有最终文本、开关打开、窗口失焦」时,经 post_task 在锁外发出。
20. on_tool_confirm 在 overlay_cv 上排队,100ms 轮询 abort,abort 时返回 Deny;子会话的 permission_request 只进 remote_confirm_queue,由事件线程泵出。
21. resume 权限恢复:Plan 模式先设 pre_plan 再设 Plan;worktree 目录不存在就 clear_active_worktree;`publish_current_goal_state` 先于 `maybe_continue_goal`。
22. 全屏界面只在没有 ask / confirm 且前台空闲时才能打开;关闭后回到 Chat 并 TakeFocus 输入组件。
23. TUI 的 `get_cwd` 保持 ANSI 行为;新代码里的 cwd 一律用 UTF-8 的 `std::string`。

## Risks / Trade-offs

- **[事件优先级链被打乱,边缘输入路径漏掉]** → 三态返回;逐键路由表带原行号;吞键矩阵特征测试;router 结构测试细到逐键函数。
- **[D 组启动时机改变,导致可见行为变化]**(系统行顺序、MCP 与首个 goal 回合的工具表)→ 在原步骤位置 emplace;B-12 的启动快照测试覆盖四种启动场景。
- **[构造期异常卡死]** → 平凡构造 + `init()` + scope guard,每个阶段都做异常注入测试。
- **[OBJECT 库链接失败]** → 接口隔离 + 每步登记 + CMake 断言。
- **[TUI 大部分只能手工验证]** → 每个 B 任务跑 [manual-test-checklist.md](manual-test-checklist.md) 里对应的小节;能抽成纯逻辑的都放进 `model/` 补单测。

## Migration Plan

- 每个 B 任务一组提交,可以单独 revert。B-13 完成之前,TuiApp 与旧的 `run_interactive_app` 不会同时存在;每一步都是把代码从 main.cpp 移到新文件,不存在双轨。

## 10. 验收(P6B 完成)

- `apps/cli/main.cpp` 不超过 80 行;§8 表中的新文件都不超过 1000 行;`tui_helpers.cpp` 拆分后没有超过 1000 行的文件。
- `check_line_coverage.py`:原 main.cpp 的每一行都有去处,覆盖率 100%。
- 所有权指标:`apps/tui/app` 下存进长寿对象的 `[&]` / `[this]` 为 0;`std::thread` 裸成员与局部变量为 0;两套全局只剩 process_guards 中的唯一实现。
- 用例清单与 G0 相同,外加新增单测;Linux CI 与 Windows 本地都构建 acecode 与 acecode_unit_tests。
- manual-test-checklist.md 全部小节在 Windows Terminal、conhost、macOS 或 Linux 上各跑一轮。

## 11. 评审修正对照(对抗评审 MR-n 的落点)

| 评审 | 严重度 | 问题 | 落点 |
|---|---|---|---|
| MR-1 | blocker | bool 返回值无法表达「停止并交还 false」 | §3,B-08、B-10 |
| MR-2 | blocker | OBJECT 库引用只在 exe 中定义的符号 | §6,B-02、B-03、B-08 |
| MR-3 | blocker | D 组启动顺序改变可见行为 | §2、§5,P0-12、B-03、B-12 |
| MR-4 | major | C 组与 AgentLoop 的构造循环 | §2 两阶段装配,B-11 |
| MR-5 | major | 构造期抛异常时析构兜底无效 | §2,B-13 |
| MR-6 | major | composer 的 Render 有副作用 | §4,B-06 |
| MR-7 | major | 侧栏 clamp 回写 | §4,B-06 |
| MR-8 | major | 吞键矩阵与代码不符 | §3,B-08 |
| MR-9 | major | 按主题打包的模块在路由上交错 | §3,B-10 |
| MR-10 | major | worktree 收尾的数据源 | §5,B-03 |
| MR-11 | major | 每一步都要登记 CMake | §6,所有 B 任务 |
| MR-12 | major | 前置声明不能提前删 | §7,P0-09、B-04 |
| MR-13 | minor | IScreenPort 缺 dimx | §3,B-01 |
| MR-14 | minor | hover 重复探测 | §4,B-07 |
| MR-15 | minor | frame_layout 需要上一帧宽度 | §4,B-04 |
| MR-16 | minor | input_trace 的 ODR | §6,B-01 |
| MR-17 | minor | atexit 与 ctrl handler 的时机和幂等 | §2,B-13 |
| MR-18 | minor | 成员清单遗漏 | §2,B-13 |
| MR-19 | minor | 文档引用 | restructure P0-08 |
| MR-20 | minor | set_callbacks 描述矛盾 | §2,B-11 |
| MR-21 | minor | 全屏界面期间的事件旁路 | §3,B-10,手工清单第 8 项 |
| FR-8 | major | startup_worktree 违反 rank | §5,B-03 |
| FR-12 | major | 「新一轮等待」重置合并的行为风险 | B-11 |
| FR-15 | minor | trace 编译定义 | §6,B-01 |
