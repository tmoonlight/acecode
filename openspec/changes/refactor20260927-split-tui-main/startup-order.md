# TUI 启动顺序基线（P0-12）

本表以 `3ddb7d43` 的 `src/main.cpp` 为原始行号基准；P0-12 认领时 `7621b4f6` 的该文件与之相同。后续删除死代码、去重和搬迁不能使这些原始锚点改指其他语句。目标路径没有前缀时相对 `src/apps/tui/`；`domain/`、`host/`、`apps/cli/` 从 `src/` 起算。表中的新宿主是既有 design.md 的迁移归属，不表示已经完成迁移。

执行顺序按本表从上到下；异步任务标注的是注册、启动位置，不承诺线程完成顺序。局部变量、回调捕获、锁边界和再次覆盖 callback 的位置都属于需要保留的时序。

## 进入交互启动之前

`5237–5257` 的入口依次执行进程环境准备、非 TUI 命令分发、交互 CLI 解析和前置 TUI 命令，最后进入 `run_interactive_app`。这些步骤归属 `apps/cli/`。进入 TUI 前的路径不应被移进 TuiApp 构造函数。

## 5259–6845 的完整步骤表

| 原行号（含首尾） | 原步骤及顺序约束 | 新宿主 |
|---|---|---|
| 5259–5266 | 进入 `run_interactive_app`，复制 CLI 的危险模式、resume、alt-screen 等选项 | `app/tui_app`、`app/tui_services` |
| 5267–5274 | 建立 working_dir、启动 worktree 状态及横幅；准备交互终端、标题、cwd、日志、工作区索引；失败立即返回 | `app/startup_environment`、`app/startup_worktree` |
| 5275–5280 | 构造 HookManager，加载旧 hook 配置，分发 `startup.before_model_load` | `app/tui_services` |
| 5281–5283 | `load_tui_config_and_runtime`：load_config → environment bootstrap → 默认技能 reconcile → hook registry refresh → proxy → models registry | `app/tui_runtime_init`、`app/tui_services`；默认技能逻辑归 `domain/skills/default_skill_startup` |
| 5284–5294 | 校验 CLI 问答策略并应用本次运行覆盖；非法值按原退出码退出 | `app/tui_services` |
| 5295–5302 | cwd override、SessionModelBinding、解析并安装 provider，随后形成 provider accessor；`startup.models_loaded` 在初始化 helper 内分发 | `app/tui_services` |
| 5303–5309 | 安装工具名改写规则与安全审计配置 | `app/tui_services` |
| 5310–5316 | 构造 ToolExecutor、SkillRegistry、MemoryRegistry、McpManager，初始化工具与注册表 | `app/tui_services` |
| 5317–5324 | 计算工作区 projects 目录、扫描 registry、注册工作区工具 | `app/tui_services` |
| 5325–5329 | 创建并注入 SkillUsageStore | `app/tui_services` |
| 5330–5338 | 构造 TuiState；初始化侧栏、模型状态、输入历史、MCP/危险模式启动消息；挂 skill usage、slash history，追加 worktree 横幅 | `model/initial_state`、`app/tui_services` |
| 5339–5381 | 固定版本/cwd 显示信息、动画 tick、chat/sidebar/ask/path 等几何与命中缓存 | `chat/chat_viewport`、`render/frame_geometry.hpp` |
| 5382–5388 | 检测终端能力与渲染模式；必要时只追加一次 legacy terminal 提示 | `app/tui_screen_host`、`model/initial_state` |
| 5389–5405 | 在原位置创建 ScreenInteractive，设置 Kitty、鼠标及同步输出，创建 redraw pacer 和键盘时间状态 | `app/tui_screen_host` |
| 5406–5424 | 注册计划重绘回调，设置 active screen、Ctrl+C 状态和选区变更回调 | `app/tui_screen_host`、`app/process_guards` |
| 5425–5430 | 启动 UpdateCheck 后台任务；此时 AgentLoop 尚未构造 | `app/tui_startup_tasks::UpdateCheckTask` |
| 5431–5433 | **先**注册 AskUserQuestion 工具，**后**启动 MCP 状态绑定及异步启动；不能移到首回合之后 | `app/tui_services`、`app/tui_startup_tasks::McpStatusBinding` |
| 5434–5448 | 定义首回合 MCP 等待协调回调：只执行一次，保持 1500 ms 预算及 warning/重绘时机 | `app/tui_submitter` |
| 5449–5485 | 创建 ChatScrollRuntime 与六个视口 lambda | `chat/chat_viewport` |
| 5486–5491 | 声明 Copilot 认证完成标志、线程与是否启动标志 | `app/tui_startup_tasks::CopilotAuthTask` |
| 5492–5511 | 判断未登录 Copilot；持锁重置等待计时字段、追加认证提示，再 PostEvent；随后设置线程已启动标志 | `app/tui_startup_tasks::CopilotAuthTask` |
| 5512–5557 | 启动认证线程：静默认证 → device code 提示 → device flow → 成功/失败提示；更新 auth_done；无认证线程时直接置完成 | `app/tui_startup_tasks::CopilotAuthTask` |
| 5558–5563 | 构造 TokenTracker 并配置初始状态 chip | `app/tui_services`、`app/tui_agent_bridge` |
| 5564–5572 | 建立 callback 共享状态、首 token/重试等标志，构造 AgentCallbacks | `app/tui_agent_bridge` |
| 5573–5610 | 安装 on_message：展示前缀处理、assistant 合并、tool 预览及 conversation 追加；保留锁和重绘位置 | `app/tui_agent_bridge` |
| 5611–5635 | 安装 transcript_message：compact notice 生命周期及缓存失效 | `app/tui_agent_bridge`、`chat/message_render_revision` |
| 5636–5648 | 安装**第一版** on_busy_changed；后面仍会覆盖 | `app/tui_agent_bridge` |
| 5649–5679 | 安装工具确认回调：入队、等待、退出时安全拒绝与 confirm_cv | `app/tui_overlay_gate` |
| 5680–5708 | 安装 delta/stream 回调及重绘节流；保留首 token 与显示状态更新顺序 | `app/tui_agent_bridge` |
| 5709–5740 | 安装 tool_result：结构化摘要、hunks、反向匹配对应工具行 | `app/tui_agent_bridge` |
| 5741–5752 | 安装 usage 回调并更新 token 状态 | `app/tui_agent_bridge` |
| 5753–5757 | 安装 goal_status 回调 | `app/tui_agent_bridge` |
| 5758–5768 | 安装 todo 回调 | `app/tui_agent_bridge` |
| 5769–5774 | 安装 thinking title 回调 | `app/tui_agent_bridge` |
| 5775–5789 | 安装 transcript replace 与 compact summary 回调 | `app/tui_agent_bridge` |
| 5790–5802 | 安装 retry reset 回调 | `app/tui_agent_bridge` |
| 5803–5810 | 安装 model retry 回调 | `app/tui_agent_bridge` |
| 5811–5816 | 安装 retry resume 回调 | `app/tui_agent_bridge` |
| 5817–5821 | 安装**第一版** on_turn_finished，之后会加入自动标题行为 | `app/tui_agent_bridge` |
| 5822–5824 | 构造 PermissionManager，应用默认/危险模式及 TUI 默认规则 | `domain/permissions/default_rules`、`app/tui_services` |
| 5825–5857 | **命名步骤：装配 AgentLoop**。构造 loop，设置 capability policy、Ask 通道、上下文配置及 hooks/skills/memory/instructions/git 指针，**第一次** set_callbacks | `app/tui_app`、`app/tui_agent_bridge`、`app/tui_overlay_gate` |
| 5858–5893 | 按 wizard 来源启动 model_pool monitor；更新负载和有效 context，再 Post 到屏幕；必须在 loop 之后、主会话建立之前 | `app/tui_startup_tasks::ModelPoolMonitorSubscription` |
| 5894–5921 | **命名步骤：建立主会话**。构造 SessionManager、start_session、设置 provider/model/权限/先前模式/worktree 元数据，给 loop 设置 SessionManager 指针 | `app/tui_app`、`app/tui_services` |
| 5922–6009 | 建立自动标题线程集合、join 函数及完整 runner；包括应用结果、state/终端标题/hook/重绘更新、异常及重试处理 | `host/session_host/auto_title_runner` |
| 6010–6035 | 定义自动标题的提交触发及 turn-finished 触发，替换 on_turn_finished | `host/session_host/auto_title_runner`、`app/tui_agent_bridge` |
| 6036–6036 | **第二次** set_callbacks，使自动标题回调生效 | `app/tui_agent_bridge` |
| 6037–6052 | 定义 TUI 模型解析回调 | `app/tui_submitter` |
| 6053–6073 | 定义模型 transition 应用回调 | `app/tui_submitter` |
| 6074–6099 | 定义结构化提交：模型绑定刷新/提示 → 自动标题 → loop submit | `app/tui_submitter` |
| 6100–6107 | 定义文本提交包装，不提前执行提交 | `app/tui_submitter` |
| 6108–6128 | 装配 SubagentHost/SessionRegistry 依赖：provider、工具、cwd、config、MCP、技能、memory、instructions、hooks、权限及 power guard | `app/tui_subagent_wiring` |
| 6129–6144 | 接线 parent session id 与后台任务发布回调 | `app/tui_subagent_wiring` |
| 6145–6166 | 接线子代理权限请求：入队、Post 和等待 | `app/tui_subagent_wiring`、`app/tui_overlay_gate` |
| 6167–6187 | 构造 SubagentHost，注册 spawn/wait/thread 等工具 | `app/tui_subagent_wiring` |
| 6188–6197 | 设置 g_session_manager，注册 atexit 及 Windows console handler / POSIX SIGINT、SIGTERM；早于 resume | `app/process_guards` |
| 6198–6208 | 建立 resume 状态，处理 latest 与显式 id 的选择 | `app/tui_startup_resume` |
| 6209–6227 | 加载显式会话 meta，查找 canonical session 文件和恢复目标 | `app/tui_startup_resume` |
| 6228–6249 | 解析并切换恢复会话的模型/provider，保留异常和临时模型提示 | `app/tui_startup_resume` |
| 6250–6259 | 调用 resume_session；按原条件追加失败、不兼容或不存在提示 | `app/tui_startup_resume` |
| 6260–6290 | 把恢复消息投影到 conversation，恢复 todos；恢复 active worktree/cwd，路径不存在则清状态并追加提示 | `app/tui_startup_resume` |
| 6291–6310 | 恢复权限模式、plan 前模式、session allows 和 meta | `app/tui_startup_resume` |
| 6311–6319 | 恢复 token/chip、追加 resume 提示、记录成功、发布 goal 并按原规则继续 | `app/tui_startup_resume` |
| 6320–6324 | 恢复终端标题 | `app/tui_startup_resume` |
| 6325–6333 | 未找到可恢复会话/旧 PID 格式的提示路径 | `app/tui_startup_resume` |
| 6334–6342 | 按顺序分发 session_start，以及恢复成功后的 title_changed hook | `app/tui_startup_resume`、`app/tui_services` |
| 6343–6346 | 构造 CommandRegistry，注册 slash commands | `commands/command_bootstrap` |
| 6347–6408 | Windows 通知初始化与点击绑定；点击时再查询窗口信息，Post 到 screen 后构造 CommandContext、恢复会话/更新视口 | `app/tui_notification_binding`、`app/tui_command_context` |
| 6409–6428 | CLI resume picker 请求经 CommandContext 执行 `/resume` | `app/tui_startup_resume`、`app/tui_command_context` |
| 6429–6445 | 接线工具 progress start | `app/tui_agent_bridge` |
| 6446–6465 | 接线 progress update，保留 150 ms 节流 | `app/tui_agent_bridge` |
| 6466–6475 | 接线 progress end | `app/tui_agent_bridge` |
| 6476–6505 | 开始安装**最终版** on_busy_changed：busy/thinking、turn 时间、完成状态更新 | `app/tui_turn_lifecycle` |
| 6506–6563 | 准备完成通知、turn_done 与远程 assistant 转发；保持中断、时长和已转发游标条件，通知在后段解锁后才实际 Post | `app/tui_turn_lifecycle` |
| 6564–6605 | 消费排队输入、追加 user 行、视口及状态重置；保持 unlock → MCP 等待 → relock → submit，最后在 6598 解锁后发送通知并重绘 | `app/tui_turn_lifecycle`、`app/tui_submitter` |
| 6606–6607 | **第三次** set_callbacks，使 progress 和最终生命周期回调生效 | `app/tui_agent_bridge` |
| 6608–6635 | 绑定 RC 入站：忙时排队，空闲时结构化提交；保留 MCP 等待时的锁边界 | `app/tui_remote_control_binding` |
| 6636–6695 | 设置 running，启动动画线程；保留 legacy idle/busy 周期、键盘/帧 pacing、循环 sleep 和 legacy tick；这些步骤不持 state.mu | `app/animation_ticker` |
| 6696–6788 | 6696–6779 持锁 tick：Ask session、状态文本/Ctrl+C/hover 期限、拖动自动滚动；6780 起锁外按原条件 PostEvent/计划重绘 | `model/animation_tick`、`app/animation_ticker` |
| 6789–6814 | 创建可聚焦 composer Renderer，装配输入换行和 caret/selection 命中布局 | `composer/input_component` |
| 6815–6835 | 定义 paste、取消及附件等五个 lambda 适配器；随 handler 成员化移除薄包装 | `app/tui_event_router`、`composer/` |
| 6836–6844 | 声明全屏设置/管理/可用性函数槽；实际装配仍在原 8686–8814 | `app/full_screen_surfaces` |
| 6845–6845 | 创建 CatchEvent 的起点；后续 handler 的执行顺序由 B-10 的逐键表保持 | `app/tui_event_router` |

## 调用 helper 内的顺序

| 原行号 | 顺序及保留点 | 新宿主 |
|---|---|---|
| 3028–3053、3529–3551 | atexit cursor restore → stdin/stdout TTY 校验 → 标题 → cwd → 可选 worktree → 日志 → workspace metadata | `app/startup_environment`、`app/startup_worktree` |
| 3055–3093 | 日志与此前暂存的 data-dir 警告 → 输入 trace 路径 → proxy init/probe/提示；proxy 必须早于 CPR 请求 | `app/tui_runtime_init` |
| 3095–3122 | models registry 初始化与按配置异步刷新；web search 初始化、缓存 region 解析、必要时后台探测 | `app/tui_runtime_init` |
| 3152–3167 | memory 目录建立，失败只停用本次运行 memory；按配置 scan | `app/tui_runtime_init` |
| 3169–3179、3639–3665 | web search → LSP init → builtin tools → skills 和工具 → memory 和工具 → MCP connect_all → 项目 scope reconcile | `app/tui_services` |
| 3181–3224、3667–3684 | MCP 侧栏 → 模型状态 → ask 配置 → 输入历史 → 危险模式消息 → MCP 后台启动消息 | `model/initial_state` |
| 3249–3270 | legacy 提示只由探测/auto/未显示过三类条件共同决定，并写入一次性 flag | `model/initial_state` |
| 3272–3294 | parse 默认权限模式 → CLI 危险模式覆盖 → 六条 TUI 默认拒绝规则 | `domain/permissions/default_rules` |
| 3553–3586 | 旧 hook 配置先用于 before_model_load；加载 AppConfig 后再刷新新 hook registry，然后初始化 proxy/models | `app/tui_services`、`app/tui_runtime_init` |
| 3590–3636 | resolve effective model → install_explicit → provider snapshot → profile context_window → startup.models_loaded hook | `app/tui_services` |

## 快照采集口径与状态

四个场景使用隔离的测试用户数据目录和人工构造的 resume 会话，不读写开发者的真实会话、provider 凭据或配置。快照必须来自运行中的 `state.conversation`，不能从上表或源码字符串拼出预期值。

采集位置固定为完成上表的启动装配之后、创建 CatchEvent 之前。记录前 16 条以及实际总条数；不存在的条目不补齐。对每条消息保留所有 `TuiState::Message` 字段，区分空字符串、false 和缺失的 optional。普通启动允许得到空数组；不能为了让快照非空而添加生产行为。

异步网络是场景输入的一部分：未登录 Copilot 保持设备授权请求尚未返回，MCP 场景使用已配置但初始化尚未完成的本机 stdio 服务。这样可以比较启动期第一批消息，不依赖外网延迟或真实账户。MCP 后续成功/失败、设备授权成功/失败仍须按手工清单另验。

采集脚本、四份实际快照及可复现条件存于本目录的 [startup-snapshots/README.md](startup-snapshots/README.md)。Windows 原始代码两轮独立冷启动已完成，四个场景分别记录 0、3、1、1 条消息，两轮快照逐字节相同。该结果不代表 Linux CI、完整手工清单或后续 B-12 的新旧比对已经通过；P0-12 的通用 CI 验证仍待完成。

## 手工清单完整性复核

已逐节对照 [manual-test-checklist.md](manual-test-checklist.md) 与 design.md 的不变量：第 1 节覆盖启动、四场景及 hook；第 2–3 节覆盖消息、状态 chip、retry/todo/goal 和工具行；第 4 节覆盖浮层吞键；第 5–7 节覆盖 composer、鼠标和中断；第 8 节覆盖全屏界面；第 9 节覆盖子代理/RC/通知/标题；第 10 节覆盖关停、worktree、断网与中文 IME。未发现必须删除或缩减的验证项。

核对清单的覆盖范围不等于已经逐项执行。三类终端的实操、四平台构建/测试及 B-12 的新旧快照比对各自保留独立结果；本表不替代这些验收。
