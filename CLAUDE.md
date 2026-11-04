# CLAUDE.md

Implementation memory for coding agents working in this repository. 用户向的安装与运行模式看 [README.md](README.md),稳定结构看 [ARCHITECTURE.md](ARCHITECTURE.md),贡献者规则、构建命令与 src 分层搬迁交付记录看 [AGENTS.md](AGENTS.md)。

**本文件只留每次都必须看到的规则和索引。** 2026-11-04 之前积累的全文(每个子系统的设计决定、踩过的坑、回归测试清单,约 18 万字)原样归档在 [docs/archive/claude-md-2026-11-04.md](docs/archive/claude-md-2026-11-04.md),不再改动。下面各条末尾的「归档:xxx」是该段在归档里的小节标题或加粗首句;**改动某个模块之前,先到归档里搜对应小节再动手**,别只凭这里的一句话摘要。新的详细记录写进对应的 docs 主题文档,这里只加一行摘要加链接;本文件曾超过 project_instructions 的单文件上限 256 KiB,ACECode 自己加载时会被截断,别让它再长回去。

## UI 文案强约束

**Avoid unnecessary secondary labels and descriptions. Let the layout do the explaining.**

遵守 [AGENTS.md 的 UI 文案强约束](AGENTS.md#ui-文案强约束):通过分组、间距、对齐和明确命名表达界面关系;不默认添加次级标签、描述或 tooltip,保留必要的决策信息、错误恢复和无障碍名称。

## Project-Level Agent Overrides

The Superpowers plugin is disabled for this repository. Do not invoke or follow `superpowers:*` skills, including `superpowers:using-superpowers`, unless the user explicitly re-enables Superpowers for a specific turn.

## 源码分层与构建

- 源码分 base / domain / adapters / engine / host / apps 六组,依赖只能向下。放置规则看 [docs/architecture/src-layout.md](docs/architecture/src-layout.md),依赖闸门是 [src/layers.tsv](src/layers.tsv)。项目头用模块根形式(`"utils/paths.hpp"`),共享测试头用 `test_support` 完整前缀,禁止父目录 include,旧路径不留转发头。业务状态归所属模块(模型探测缓存在 provider、搜索地区缓存在 tool/web_search、桌面工作区记录在 desktop、TUI 命令用量在 tui),`utils/state_file` 只做通用 JSON 事务。
- 2026-09-29 搬迁已完成(PR #87 合入 master),九个旧 ref/worktree 保留原状、不标弃用;映射版本 refactor20260927/P3。各阶段决定看 AGENTS.md「src 分层搬迁交付记录」与 `openspec/changes/refactor20260927-*`。
- `acecode_testable` 是无源文件的 INTERFACE 聚合;`acecode` 链 `acecode_cli`(TUI / headless / daemon);整个 TUI 属于 `acecode_tui`,没有 testable 子集;`cmake/acecode_layer_libraries.cmake` 定义库归属,configure 期守卫与 CI 的 File API 检查校验覆盖和依赖边界;`acecode_unit_tests` 需 `BUILD_TESTING=ON`;`acecode-desktop` 只在 `-DACECODE_BUILD_DESKTOP=ON` 时创建。
- 改嵌入前端先在 [web/](web) `pnpm build`,再重新 configure CMake(嵌入发生在 configure 期,`web.static_dir` 非空时改走文件系统)。Windows 需 libcurl ≥ 8.14,UTF-8 编译选项。
- CI(test.yml)跑五类静态闸门:`check_layers`、`check_file_size`、`check_ownership --strict --final`(新代码不写裸 `std::thread`,用 `JoiningThread`;存起来的回调捕获 `lifetime_.ref(*this)`,`LifetimeToken` 是最后一个成员、析构先 `stop()`;不写 `[&]` / `[this]`;未跟踪的新文件要用临时索引加进去才会被 lint)、`check_doc_paths`(所有 .md 里出现的 `src/` / `tests/` 路径必须真实存在)、`validate_map`。
- 设置 > 环境(`src/base/environment/`,`routes_environment.cpp`):`bootstrap.cpp` 在 TUI / daemon / headless 配置加载后运行,headless 不落盘自动检测;`toolchains.cpp` 保留原始 PATH、每次保存重建前缀。
- **数据目录迁移**(`paths.cpp` 读 `data-dir.redirect.json`):先复制到私有 staging、SQLite 走 backup API、再次校验后才写指针;拒绝忙会话 / 开着的 PTY / 其它活 daemon;成功后共享写闸门关到重启;**绝不删除**失败的校验目标或活动根。四条铁律:OS 错误文本先单独 `ensure_utf8` 再拼接,`json_response` 的 `error_handler_t::replace` 兜底不能去;`\\?\` 扩展路径只用于文件 IO,不进 JSON / 指针 / 日志;Agent Browser profile 只能尽力复制,已快照主库的 `-wal/-shm/-journal` 不复制;「其他实例」只在证明 PID 复用时放行,每个拒绝分支记日志。归档:数据目录迁移的几条铁律。

## 运行面(入口)

- **TUI**:[src/apps/cli/main.cpp](src/apps/cli/main.cpp) → [TuiApp](src/apps/tui/app/tui_app.hpp),FTXUI 循环,worker 回调投回 UI 事件队列。TUI 主会话不在 SessionRegistry 里。
- **Daemon**:[src/apps/daemon/cli.cpp](src/apps/daemon/cli.cpp) → `worker.cpp::run_worker`(已近 1000 行上限,装配往外拆),写 `<data_dir>/run/` 运行文件,服务 [src/apps/web/](src/apps/web) 路由与 WebSocket。
- **Headless**(`acecode -p "<prompt>"`):`command_dispatch.cpp::dispatch_non_tui_command` → [headless_runner.cpp](src/apps/headless/headless_runner.cpp),无 run/ 文件、无心跳、无 Crow,会话正常落盘可 resume。`headless::active()` 两个消费点:权限门(默认自动拒绝并给模型解释,`--yolo` 全放行)与 AskUserQuestion 自动应答。`-c` 接最近会话,`--resume <id>` 在 print 模式 id 必填,`--session-id` 自定 id,`--output-format json` 成败都出单个 result 对象;未传模型走默认模型、权限固定 default、`--max-turns` 默认无限;`--disable-tools` / `--enable-skills` / `--enable-mcp` 与 `--list-tools` / `--list-skills` / `--list-mcp`,未知名称 exit 64。归档:Current Runtime Surfaces。
- **Desktop**:[src/apps/desktop/](src/apps/desktop),每个 workspace 一个 daemon 子进程(独立端口 / token / Job Object),webview 经同进程 bridge 调用,daemon 内部零改动。见 [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md)。
- 进程级服务(工具重写加载、审计库、记忆运行时、LSP、代理探测、数据目录告警补记)在 worker.cpp / TUI main.cpp / headless_runner.cpp 各自接线;新增进程级服务要逐个入口核对,别只接 daemon。工具注册在三处启动时各做一次,`/model` 中途切换不重建 ToolExecutor。

## Agent Loop 与工具

- [AgentLoop](src/engine/agent/agent_loop.hpp) 是门面;[TurnRunner](src/engine/agent/turn/turn_runner.cpp) 管多轮状态机,[TurnFinalizer](src/engine/agent/turn/turn_finalizer.cpp) 管有序收尾;固定依赖经 [AgentLoopServices](src/engine/agent/agent_loop_services.hpp) 注入,事件型 prompter 在显式 start() 前装好。纯文本回复结束回合;`AskUserQuestion` 不是终止符。`config.agent_loop.max_iterations` 0 = 无限。
- **`task_complete`** 是显式终止符,也是 Web 的「下一步」框:参数 `summary`(别名 `recap`)+ 可选 `ask` / `actions`(1 到 2 个按钮,第一个是推荐项)/ `blocked`,规范化在 `task_complete_tool.cpp::parse_task_complete_call`,结果写 `metadata.wrap_up`;工具 output 不再回显 summary,Web(`lib/nextStepBox.js`)取 `wrap_up.recap`,绝不能退回显示 output;TUI / IM 把问题接在总结后面。归档:task_complete = Claude 桌面版 wrap_up。
- 核心工具双端注册:bash / file_read / file_write / file_edit / grep / glob / task_complete / AskUserQuestion / skill / memory / 可选 web_search / MCP;`ToolResult` 可带 summary 与 hunks 让 TUI / Web resume 渲染紧凑行。`bash_tool` 流式清洗输出、轮询 abort、截断超大输出、支持 POSIX `stdin_inputs`;文件工具改文件前调 `track_file_write_before`。
- **Prompt cache 前缀不变量**:注入到最后一条真实 user 消息之前的内容必须内容驱动、逐字节稳定(cwd 与只到天的日期留在静态 system prompt;会话上下文按 cache_key pin;git 快照按会话缓存;工具表 `std::map` 定序;Anthropic 合成 tool_call_id 用内容哈希)。守护:`agent_loop_termination_test.cpp::RequestPrefixIsByteStableAcrossIterationsInATurn`。usage 口径:`prompt_tokens` 含全部输入,`cache_read_tokens` 是子集;有 usage 没缓存计数显示 0% 而不是隐藏。归档:Prompt cache prefix invariant。
- **权限与沙盒**:`PermissionMode::Auto` 规范名 `auto`(`accept-edits` 只是输入别名);`exec_decision` 决定是否确认与执行边界,AgentLoop 是唯一审批入口,`bash_tool` 只执行注入的 `exec_sandbox`、不得自行取消沙盒重试;规则 forbidden 优先于 yolo / dangerous;active goal 下越权与额外权限申请一律 Forbidden(`escalation_unattended`),不能回到「Prompt 再被 goal 放行」。清单判定「最长前缀优先,同深度 deny > write > read」,**比较两侧都先 `canonical_utf8`**(junction 别名曾绕过 deny),单测手拼策略也用 `canonical_policy_path`。审批决策五个:allow / allow_session / allow_scoped / allow_remember / deny。Windows 用免管理员 WRITE_RESTRICTED 令牌、不隔离网络、受限令牌管不了读;子进程挂 Job Object 但**不设 KILL_ON_JOB_CLOSE 也不设 SILENT_BREAKAWAY_OK**;macOS Seatbelt,Linux bubblewrap;探测结论粘性,`/sandbox on` 是唯一不重启的恢复入口。详见 [docs/sandbox.md](docs/sandbox.md)。归档:Auto 模式与 exec 沙盒。
- **安全中心 / 审计**:审计 SQLite 唯一记录点是 AgentLoop 审批门(`audit_gate` / `record_audit`),每个「决定已作出」的分支记一条,只读工具的自动放行不记;`/api/security/exec-rules` 只整体重写 `default.rules` / `default.sandboxed.rules` 两个托管文件。归档:安全中心。
- **工具名两套**:原生名(注册 id、JSONL、权限、hooks)与模型侧名([tool_protocol_names.cpp](src/domain/llm/tool_protocol_names.cpp),由「工具重写」`<data_dir>/tool-rewrites.json` 动态发布,默认空 = 不重写)。给模型看的文案里工具名一律 `model_tool_name_for_native(...)` 动态取;**绝不对工具输出正文做整词重写**;发给 provider 的历史只有一个入口 `provider_history.cpp::model_facing_provider_messages`;入站解析 fail-open;翻译失败回退原生定义而不是空表;hooks matcher 也接受当前模型侧名;测试用 RAII `ScopedModelToolNameMappings`。归档:模型侧工具名与「工具重写」。
- **输出损坏重发一次**:正文(代码块外)泄漏 `<arg_key>` / `<arg_value>` 模板标记,或工具调用名字为空 / 不合法且夹着模板标记([tool_call_corruption.hpp](src/domain/llm/tool_call_corruption.hpp)),整条丢弃、清流式草稿、发 `response_corrupted_retry` 后原样重发,每步最多 1 次;**参数检查只认模板残片的结构,不要改成子串一出现就拦**(编辑 ACECode 自己的源码会被反复丢弃);`</think>` 刻意不算。旧会话里的坏调用由 `recover_provider_history` 投影兜底。提示词与工具输出里不写 `skill_view(name="…")` 这类伪函数调用,GLM 会照抄成工具名。归档:输出损坏重发一次。
- **视觉能力四面同源**:`LlmProvider::supports_vision()` 发散到序列化层、system prompt `# Environment` 的 `Active model can read images directly` 行、`vision_analyze` 的描述与执行门(按 `(provider, model id)` 剔除当前模型,**不按 name**)、skill frontmatter 首句。`ToolContext` 的 active_* 为空一律 fail-open。**不要**改成「没 vision 就不注册 `vision_analyze`」。改 `assets/seed/` 要同步 `seed.version` / `MANIFEST.json` 与 `default_skill_seeder_test.cpp`。归档:视觉能力必须在四个面同源。
- **GPT 系 apply_patch**:判定只看模型 id(`model_family.cpp::model_prefers_apply_patch`),不看 provider 名;三个编辑工具始终注册、只裁模型侧定义表(`filter_tool_definitions_for_model`),system prompt 按模型族分支,非 GPT 模型输出逐字节不变(`system_prompt_test.cpp::NonGptModelStateIsByteIdenticalToLegacyPrompt`);`apply_patch` 先全量校验再落盘,权限门按路径集合走,Delete / Move 源也要 `track_file_write_before`;表面只扫 header 行。归档:GPT 系模型适配。
- **具体进度提示(工具前言 v4)**:文案全部由 daemon 生成(`engine/tool_preamble/`:推理加粗标题 > 工具模板 > 场景文案),loading 只说正在做什么、不带参数、不依赖模型配合;开关 `agent_loop.tool_preamble.enabled`(设置 > 常规 > 工作模式);只在 `emit_agent_progress` 一个口子替换。前三版(先说一句话 / 必填参数 / `<text_preamble>` 标签)都被否,别做回去;历史标签只剥不用。归档:具体进度提示。
- **提问**:`agent_loop.question_policy` ask / deny / timeout(`question_timeout_seconds` 默认 60);YOLO 不改提问策略;active goal 覆盖为 30 秒超时采纳推荐项。**提问期间的插话**走 `AgentLoop::interject_question`(同回合 soft steer,tool_result 标 interjected,`question_closed.reason=interjected`),**不要**改成 `interrupt_turn`。归档:提问期间的插话。
- **侧边对话只放只读工具**(`side_chat.cpp` 小工具循环):白名单 `file_read` / `grep` / `glob` / `lsp` 与会话策略取交集,MCP 一律不放,**不要**改成按 `is_read_only` 筛;凡要弹确认的一律拒绝;在 `MtimeTracker::DetachedReadScope` 里执行;Web 侧边多一个 `send_to_main_session`,用户确认之前绝不送达,送达走 `send_side_chat_message`(有可插话回合就 steer,否则普通 send)。归档:侧边对话只放只读工具。
- **子代理 / 蜂群**:`spawn_subagent` / `wait_subagent` 双端注册,子会话是普通 SessionRegistry 会话(父 cwd + 权限模式),不能再派子代理;子会话从会话列表隐藏、始终跟随主会话(无单独归档 / 删除,主会话永久删除才级联);蜂群 `off | star | mesh` 两套工具互斥,邮件进 AgentLoop 邮箱,信封落盘为 user 角色 + `metadata.inter_agent`,子 agent 完成不唤醒空闲父 agent,Web 芯片只在与服务端模式不同时才随消息提交 `swarm_mode`。全文 [docs/subagents.md](docs/subagents.md)。
- **写边界**:`AgentLoop::write_root()`(会话 worktree > LOOP 策略 > 继承)非空即有边界,Yolo 不免除,只有 dangerous 放行;子会话共享父会话 worktree 并继承写边界;bash 走 `shell_write_guard` 可证明写目标守卫(**守卫不是沙箱**,事后用 `workspace_touched` 快照兜底);附加文件夹不能打穿写边界。归档:写边界与子代理继承、多文件夹项目。
- **Worktree**:落在主仓根 `.acecode/worktrees/<slug>`,分支 `worktree-<flatten(slug)>`;`EnterWorktree` / `ExitWorktree` 只在用户明确提到 worktree 时可调,会话存储位置不动;合回主干必须再调 `ExitWorktree`;`ExitWorktree remove` fail-closed;`--worktree [name]` 时 worktree 就是项目根。纯逻辑 [worktree_core](src/domain/worktree/worktree_core.hpp),git 层 [worktree_manager](src/domain/worktree/worktree_manager.hpp)。归档:Worktree 隔离。
- **Goal**:每会话至多一个,存项目级 `state.sqlite3`,只有 `active` 自动 continuation(Plan mode 不触发);unattended 下写工具自动放行、bash 仍取决策表沙盒;provider 终止错误把 goal 转 usage_limited(429)/ blocked,用户 abort 转 paused。归档:Thread Goals。
- **定时任务两道闸**:`create_scheduled_task` 按 `activation_skill` 延迟开放并在技能索引单列「Skill-gated built-in tools」;`loop.active` 会话里经 `loop_execution_refusal` 并入 `hidden_builtin_tools`;AI 创建时拒绝仍带「建定时任务」字样的 prompt。**不要**把隐藏改成只靠提示词。归档:智能定时任务。
- **MCP 调用响应 abort**:`McpManager::invoke` 放工作线程、100ms 轮询 abort,置位立即返回。`image_generate` 配置属于 Settings > Tools,测试动作绝不在加载 / 保存时跑。
- **重试与超限**:429 文案明确说额度 / 余额用完按硬配额不重试(`retry_policy.cpp::is_hard_quota_message`);上下文超限识别含 LiteLLM / vLLM 文案;压缩连续超限时逐步加大删除量;只有图片的 user 消息压缩后保留占位文字。归档:其它反馈修复备忘。
- **PA 内网适配**([src/adapters/pa/](src/adapters/pa/README.md)):只按报文特征判定,不硬编码地址 / 模型名,通用路径只留兜底调用点。五条:中文超限文案兜底(故意不限状态码)→ 上下文预算学习器收敛到观测拒收规模的 85% → 随机拒收走 `run_pa_overflow_rescue` 绝不终止回合 → **限流文案(请求频率 / 过于频繁 / 限流)绝不当超限**,排在超限判定之前 → 带内联图片被拒先撤图(`withhold_inline_images`)且不进学习器。**改之前读 README 里的三条坑**(观测要么可信要么整条丢弃;model 为空不记不查;观测表是进程级单例)。归档:PA 内网模型服务适配。
- **图片**:「压缩图片」只在发送层生效(`model_image_payload::compressed_model_image` → LRU + `.model-v1` sidecar),快照原图不改;请求体预算把最旧的图换成文件引用并记低水位,前缀不变;`computer_use` 截图只重编码不改尺寸。图片附件一律走快照 + 缩略图,**栅格图不要做成 `@路径` 引用**。归档:「压缩图片」、图片附件一律走快照。

## 会话与持久化

- [src/domain/session/](src/domain/session) 每会话一个 `<id>.jsonl` + `<id>.meta.json`;运行期显示字段不序列化;resume 直接采用 JSONL 重建伪行、预览、摘要、diff,不复制成 PID 后缀文件(旧 `<id>-<pid>.*` 是不支持的实验数据,让用户删);writer lease 守共享 transcript,owner PID 死或心跳过期可回收。`/rewind` 用每用户回合检查点。
- Daemon 多会话靠 `SessionRegistry`,每个 entry 自带 SessionManager / PermissionManager / AgentLoop / 异步权限与提问 prompter;`EventDispatcher` 给事件单调 seq 与有界回放环。
- **标题两份,内存优先**:`list_active()` 报内存 title 并盖过磁盘 meta,只写磁盘的改名会被盖回;`SessionManager::update_meta` 写前 `adopt_foreign_user_title_locked()`。**显示标题只有一个字段** `sessionDisplayTitle`(title > summary),前端不许从消息正文现推;`summary` 由 `summarize_user_message_text` 取显示文本折单行截 80 字节,实时同步只有 `session_updated` 一条路。归档:会话标题有两份、会话显示标题只有一个字段。
- **列表成本在文件个数**:只要最新几行时必须走 `list_session_metadata_page(project_dir, limit, accept)`(先枚举文件名 + mtime,只打开 `limit + max(8, limit)` 个);`accept` 拒掉的不占名额;活跃会话逐个点读自己的 meta;**`dir` 必须由会话自己的 cwd 推导**(一个 daemon 服务多个 workspace;后台任务面板曾因此停在 00s);`total` 只是上界,前端看 `has_more`;`is_canonical_meta_filename` 手写扫描不用 regex。归档:会话列表的成本在文件个数。
- **侧栏卡顿第二来源是请求条数**:`pinned-sessions` / `opencode-import` 只对可见 workspace 重取(`sidebarAuxiliaryFetch.js`);诊断看 `performance.getEntriesByType('resource')` 的排队时间,不看服务端日志。`WorkspaceRegistry::scan()` 按每个 hash 目录自身 mtime 负缓存,2 秒内动过的不信缓存,写路径主动失效。归档:侧边栏卡顿的第二个来源、WorkspaceRegistry::scan() 的负缓存。
- **首页任务建议**(`/api/home/intent-prompts`)按需读取、不整库扫目录,目录修改时间按路径读,找会话预算 30 秒。归档:首页任务建议。
- **历史按页访问**:首次 `messages?limit=200`,`before` 前插保留 id 与滚动位置,游标含字节位置 / 行指纹 / 文件身份,替换失效返回 409 后重载尾页;`load_active_messages` 锁内只打开文件、锁外解析;`list_active` 锁内只复制 entry 引用。归档:会话历史按页访问、读取与恢复的锁边界。
- **记忆**:三端经 `create_memory_runtime` 拿同一个 `MemoryService`,作用域按会话项目目录解析(不用进程 cwd);写入先重扫磁盘再在 `state.sqlite3` 事务里完成,写入口 `redact_secrets`;注入是按会话冻结的快照(不打穿 prompt cache);`/memory` 只有一份文本实现;摘要调度器只在 daemon / TUI。「记住」一律走 `memory_write`。归档:记忆(openspec unify-memory-system)。
- **Skills 与项目指令**:`SKILL.md` frontmatter 启动读、正文懒加载;技能索引每请求注入 session-context(预算 1% 上下文窗口),没有它模型永远不会主动调技能;`/<skill>` 在 Web 与 TUI 都换成 `build_skill_invocation_hint` 的轻量提示,不注入正文。项目指令加载 outer-first、在 linked worktree 根止步;仓库根只留规范文档,别加重复的根指令文件。
- `file_read` 的「文件未变」去重按会话 id 分区,压缩与线程修复后清空观测;JSONL 的非搜索追加走 `append_non_searchable_locked`,否则搜索索引整份重建。

## Daemon 与 Web

- Daemon 启动写 pid / port / guid / token / heartbeat 到 `<data_dir>/run/`;loopback 免 token,非 loopback 要 `X-ACECode-Token`,非 loopback 拒 dangerous。协议改动同步 [docs/daemon-api.md](docs/daemon-api.md)。远程 Web 代理把上游源地址绑到 `127.0.0.2`,macOS 退到本机非回环 IPv4。
- `app_config_mu` 是 shared_mutex:只读快照共享、写路径独占,恢复路由不把全量 IO 包在配置锁内;`SessionRegistry::resume` 的 `resume_inflight_` 单飞。Attention 落盘节流(flusher 线程合并写,只有状态跃迁同步落盘),别改回逐事件写。daemon 线程清单见归档:Maintenance Notes。
- **模型配置**:手改 config.json 即时生效(`refresh_model_config_if_changed`,入口 `sync_model_config` 与 `GET /api/models`);saved_models 结构变化才推进进程级 revision;`SessionModelBinding` 是唯一的会话 Provider 发布缝,AgentLoop 回合开始捕获快照;重建门是进程加盐的构造计划指纹,`unresolvable` 保留当前 Provider。`sampling` 按 provider / 协议校验,会静默丢弃的值算校验错误。归档:Model Profiles And Context Windows。
- **cwd 一律以 UTF-8 `std::string` 传递**,别用 `fs::path` 当参数类型接它(MSVC 隐式转换按 GBK 解码,中文目录算错 hash 或抛异常);内部需要时用 `path_from_utf8`。Desktop 托管的 daemon stderr 指向 NUL,只写 stderr 的诊断等于没有;路由逃逸异常收成 JSON 500 并记日志。归档:cwd 一律以 UTF-8。
- **HTTP / WS 增量**:`/<skill>` 命令在 `POST messages` 里经 `try_expand_skill_command` 展开;`/api/commands` builtins 硬编码 init + compact;`/api/files*` 走 cwd 白名单 + weakly_canonical 防越权;`/api/fs/*` 已鉴权即可只读浏览全盘且**不 canonical**(经 junction 会注册出两个 workspace hash),`ec.message()` 入 JSON 前 `ensure_utf8`;fork 复制前缀到新会话、不继承 checkpoints;Web 路径选择器在无 Desktop bridge 时一律 web 弹窗。完整表格见归档:HTTP / WS 协议增量。
- **消息通道**(QQ / 微信 / 飞书 / 钉钉 / Telegram / Discord / LINE):三层 `adapters/im/<平台>` → `host/channels/core` → `apps/daemon/im_channels.cpp`,方向只能向下;平台清单只有一张表 `core/platforms.cpp`,认识传输层类型的只有 `core/platform_adapters.cpp`;只给本机(`is_trusted_local_client_address`,127.0.0.2 也 403);凭据只回尾号、不写日志;设置页只给简要状态,细节由 `core/state_log.cpp` 写 daemon 日志,前端测试守着不渲染细节;外部平台插件只在启动扫描 `<data_dir>/connectors/`,stdio JSON lines,reader 带超时读。各平台的坑看 [docs/channels.md](docs/channels.md) 与归档:消息通道。
- **Web 前端**:React 18 + Vite 5 + Tailwind v4,`pnpm build` 后重新 configure;产物多 chunk 按需加载(`EAGER_VENDOR_PACKAGES` 别加按需库),`/assets/<file>` 不经过 Crow catchall,静态资源强 ETag + `assets/` immutable。数据整形逻辑放 [web/src/lib/](web/src/lib) 纯函数 + Node 测试,不塞进组件。新中文文案先补 `i18n-en-overrides.mjs` 再 `pnpm i18n:catalog`。
- **实时 transcript 是单写者 store**(`singleWriterStore.js`,`commit(producer)` 唯一写入口,hook 内无 useState;排队输入同一个 store);回放去重只认 `replayed` 帧,`STREAM_NEUTRAL_EVENT_TYPES` 不 flush token;工具伪行按 call id 配对;尾部窗口 120 行、跟随时封顶 180,`pinned` 别去掉;卡住兜底三层(WS 心跳 / 事件游标带实例号 / `transcriptLiveWatchdog`)。详见 [docs/web-chat/transcript-state.md](docs/web-chat/transcript-state.md) 与归档:实时 transcript 状态是单写者、事件回放去重、工具伪行按 call id 配对、Transcript 尾部窗口、实时状态卡住的兜底。
- **流式渲染**:`renderMarkdownBlocks` 分块 + `useStreamingRenderThrottle` 100ms 降频,未闭合代码块不高亮;`.ace-chat-transcript-scroll` 关 overflow-anchor。归档:流式渲染防跳动。
- **交互约定**:对话框键盘走 `components/Modal.jsx` + `lib/dialogKeyboard.js`(Esc / Tab 循环 / Enter 默认操作 / auto-repeat Enter 拦下),不要手写 `fixed inset-0` 遮罩;排队卡片在 transcript 外(`QueueCardList`),中断后队列暂停不自动出队;粘贴 ≥ 20 行 / 2000 字符折成卡片、≥ 128 KiB 落文件;`@` 会话引用有查询词必须走服务端 `q=`;侧栏拖进输入框 = @ 引用;Slate 选区在 compositionstart 前 `adoptDomSelection`。归档:对话框键盘约定、排队卡片栈、粘贴的文本块、`@` 会话引用、输入框光标。
- **搜索与批量**:`Ctrl+K` 快捷键只有一个定义点 `lib/searchPaletteShortcut.js`(xterm 要放行);前端纯聚合所有 workspace,后端零路由变更;侧栏多选批量操作只发一次 `POST /api/session-batch`。
- **git 感知**:`/api/git/info` 必须走 `gitInfoCache.js`(一次 spawn 5~7 个 git),`GitSessionPill` 用 `pillCouldRender` 在请求前短路;`src/domain/gitinfo/` 的 ref 名进 prompt / REST / argv 前过 `is_safe_ref_name`;SidePanel「变更」tab 在 git 仓库走 git 级视图。归档:git 感知三件套。
- **HUD 主题**(荒坂,工坊专用、默认不启用,**不写进版本更新记录**):`themeHud.css` 每条选择器以 `:root[data-theme-chrome="hud"]` 开头(滚动条部件用 `html[...]`),`themeHud.test.js` 守着;音效 / 异形窗口 / 背景染色只影响声明它们的主题;动效不跟随系统「减少动态效果」(用户常在 RDP 里用)。归档:HUD 主题外观与多风格。

## Desktop Shell

- 每 workspace 一个 daemon:`workspace_registry`(`src/base/workspace/`)/ `daemon_pool` / `pick_active` / `web_host` / `tray_icon_win` / `notifications_win`;切 workspace 走整页 navigate(跨端口 CORS);webapp 兼容模式(WebView2 不可用或 `--webapp`)用托盘 + 消息循环托住进程,别用 `WaitForSingleObject(msedge)` 当生命周期;单实例互斥 `Local\ACECode-Desktop-Singleton-v1`;关窗默认隐藏到托盘(`desktop.close_to_tray`)。bridge 函数清单(`aceDesktop_*`)与通知抑制规则看 [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md) 与归档:Desktop shell + multi-workspace。
- 写 workspace.json 一律先读盘再改(`latest_workspace_meta`),Desktop 与 daemon 各持一份缓存。托盘 / 通知 / toast 的 controller 窗口**不能用 HWND_MESSAGE**。
- **窗口细节**:启动浮窗由独立绘制线程播动画(`splash_animation.cpp` 纯函数);窗口位置跨启动记忆(`window_placement_win.cpp` + `place_saved_desktop_window`,普通边框口径);Win10 顶边 1px 线由宿主窗口自画(别用 `DwmExtendFrameIntoClientArea`,别画在网页里);Ctrl+滚轮缩放只走 `aceDesktop_zoomFactor`,WebView2 `IsZoomControlEnabled` 保持 FALSE;主题异形窗口全部挂在 `g_window_shape.chamfer` 上。归档:启动浮窗动画、主窗口位置跨启动记忆、Win10 顶边、Ctrl+滚轮页面缩放、异形窗口。
- **挂机 OOM 三层**:侧栏空闲刷新「内容没变就不 setState」(`structuralState.js`;React 18 相同值更新仍留队列);Sidebar / ChatView / App 里**不要再写 `useCallback(fn, [会变的状态或 props])`**,用 `useStableCallback`(三条边界见归档),`stableCallbacksArchitecture.test.js` 守着;webview 桥接 `_promises` 泄漏由 `kBridgeReplyCleanupScript` 接管;渲染进程退出自动 Reload(10 分钟最多 3 次)。归档:Desktop 挂机内存。
- **Agent Browser**:永久开发宽松浏览器(证书 / SmartScreen / 旧 TLS 一律静默放行,**没有开关没有提示**,边界由 `agentBrowserArchitecture.test.js` 扫整个 `src/`);导航状态按最终结果决定(`agent_browser_navigation_state.cpp`,重试全页最多一次);页面归属落在 host 的 `agent_browser_page_directory`,联结键只有 `session_id`,带 owner 的建页不改显示页;前端页签唯一来源 `agentBrowserPages.js`;Cookie 三档 shared / per_session / until_quit(until_quit 要隐藏保活 controller,切档不用 `Profile8::Delete`,跨进程状态在 `state.json` 的 `agent_browser` 段);新标签页推荐由 daemon 存 `agent_browser_top_sites.json`。全文 [docs/agent-browser.md](docs/agent-browser.md) 与归档:Agent Browser 四节。
- **iOS 模拟器**(仅 macOS):helper `acecode-ios-sim` 运行期 dlsym 私有框架、跑在 Seatbelt 里;Xcode 自动发现、不用 `xcrun`;帧缓冲必须同步加锁;HID 按 CoreSimulator 版本分 Indigo / dtuhid;Desktop 原生零拷贝视图只加载系统路径的 CoreSimulator。见 [docs/ios-simulator.md](docs/ios-simulator.md) 与归档:iOS 模拟器。别在用户 Mac 上跑 lldb。
- **控制台(ConsoleDock)**:`src/base/pty/` 四后端 ConPty / Winpty / Pipe / PosixPty;ConPTY 读循环要 PeekNamedPipe 轮询,spawn 时 `STARTF_USESTDHANDLES` + 三个 NULL 句柄;`/ws/pty` loopback 硬限制;xterm `attachCustomKeyEventHandler` 放行 ``Ctrl+` `` 与 Ctrl+K。归档:控制台停靠区。
- **Windows 弹框三层**:`notification_backend_policy`(auto / system / custom 纯判定表)→ `notifications_win.cpp` 注册表探测 + 运行期粘性降级 → `custom_toast_win.cpp` 自绘(独立线程消息循环,per-pixel alpha 要 `apply_rounded_alpha`)。归档:Windows 弹框。

## TUI

- 工具行 ` ● ToolName(args)` 三态指示灯由 `tool_row_format.cpp::compute_tool_call_dots` FIFO 配对;调用 / 结果成对相邻(展示派发在各执行点,Phase 3 只落盘),resume 回放同构;Ctrl+O 全局 verbose;无摘要结果按可视行折 3 行(`tool_result_fold.cpp`)。归档:TUI 工具行渲染。
- 鼠标默认开(滚轮滚 transcript,右键 / Ctrl+V 剪贴板经 OSC 52 与 `platform/clipboard`);`decide_render_mode` 纯函数决定 alt-screen,`tui.alt_screen_mode` auto 恒 AltScreen,classic conhost 用保守布局。斜杠命令别名只挂在原名上(`SlashCommand::aliases`),不单独注册,`has_command` 对别名也返回 true。`ui.border` / `ui.accent` 保持柔白,彩色只给语义角色。归档:Mouse Input、Legacy terminal fallback、斜杠命令别名。

## 网络、搜索、LSP、模型

- **代理**:`proxy_resolver` 统一注入 cpr;`network.proxy_mode` auto / off / manual;启动 TCP probe 不通则进程级回退直连(`/proxy refresh` 清),session override 永远胜过 fallback;不暴露 TLS 校验开关。
- **Web Search**:默认 `parallel`(RSS + DuckDuckGo + Bing CN 并发合并);`enabled=false` 不注册工具;查询限 200 码点;`/websearch` 只改会话级。
- **LSP**(`src/adapters/lsp/`,仿 opencode,零新三方库):只探测已安装 server、绝不下载、server 不进安装包;编辑后只注入 ERROR 级诊断;**两条铁律**:所有入口按调用传入的 `session_cwd` 判 workspace 边界,比较 / root / slot key / URI 全部先 `weakly_canonical`(用户目录是 junction 时两形态不匹配)。Windows 上 .cmd/.bat 经 `cmd.exe /d /c`。归档:LSP。
- **模型解析**:`default_model_name` → `<cwd_hash>/model_override.json` → 恢复会话的 provider/model 元组 → 兜底;没有 saved model 时启动失败而不是合成。上下文窗口经 `resolve_model_context_window_nonblocking`,规则见 [docs/model-context-resolution.md](docs/model-context-resolution.md)。`saved_models` 重名修复、悬空默认回退、`config-backups/last-good` 快照与 `invalid` 归档见归档:Config Schema、Model profile resolution、`/model` command。

## Config Notes

写入时省略默认值。主要段:`saved_models` / `default_model_name`、`models_dev`、`skills`、`memory`、`project_instructions`、`agent_loop`、`daemon`、`web`、`network`、`web_search`、`image_generation`、`image_compression`、`lsp`、`worktree`、`tui`、`desktop`、`mcp_servers`(无 `transport` 默认 stdio;`sse` 旧双端点;`http` Streamable HTTP 默认 `/mcp`)。工具重写与 Agent Browser 跨进程状态刻意不进 config.json。完整示例与字段含义见归档:Config Schema。

## Useful Source Anchors

- [src/apps/tui/commands/builtin_commands.cpp](src/apps/tui/commands/builtin_commands.cpp): slash command registration and command help.
- [src/adapters/tool/tool_executor.cpp](src/adapters/tool/tool_executor.cpp): tool registry behavior, tool result message formatting, and compact call previews.
- [src/engine/agent/event_payload/tool_event_payload.cpp](src/engine/agent/event_payload/tool_event_payload.cpp): web serialization of tool progress and summaries.
- [src/engine/agent/event_payload/message_payload.cpp](src/engine/agent/event_payload/message_payload.cpp): REST/WS message payload identity and metadata.
- [src/domain/session/session_serializer.cpp](src/domain/session/session_serializer.cpp): persisted message field allowlist.
- [src/apps/tui/render_mode.hpp](src/apps/tui/render_mode.hpp): pure terminal render-mode decision logic.
- [src/base/utils/paths.cpp](src/base/utils/paths.cpp): user vs service data directory resolution.

## CI / Release

- `package.yml` 出 Linux x64/arm64、Windows x64/arm64、macOS x64/arm64,`v*` tag 自动发版;`release-tag.yml` 手动打注解 tag 并显式 `gh workflow run package.yml`,发布门槛仍是该提交的 test.yml 通过。npm 发布暂停(`if: ${{ false }}`),实现保留。归档:CI / Release。
