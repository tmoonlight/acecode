# Design: unify-memory-system

## Context

动机见 proposal.md。与方案相关的现状:

- **记忆模块**:`src/domain/memory/`(`MemoryRegistry` 目录缓存、`MEMORY.md` 索引渲染、frontmatter 解析、路径校验),单一目录 `<data_dir>/memory/`。工具 `memory_read` / `memory_write` 在 `src/adapters/tool/`。TUI 命令 `src/apps/tui/commands/memory_command.cpp`(list / view / edit / forget / reload)。
- **注入**:`build_user_memory_context_prompt` 把整份 `MEMORY.md` 拼进会话上下文块(与项目指令、自定义指令同一块),每次请求重建,由 `PromptContextCache` 按内容 key 钉住。记忆一变,本会话后续请求的上下文块就变。
- **接线**:TUI 在 `tui_runtime_init.cpp` 注册记忆工具并把注册表传给子会话宿主;daemon(`worker.cpp`)与 headless(`headless_runner.cpp`)在 master 上传空指针。分支 `claude/fix-feedback-0930` 已给 daemon 接线,并让写入前按磁盘重扫。
- **可复用设施**:共享设置修改层 `src/base/config/settings_mutations`(网页路由与 TUI 设置中心都走它);自动标题的后台模型解析与创建(`session_auto_title.cpp` 的 `explicit_profile` / `create_provider_from_entry`);SQLite RAII 封装 `platform/unique_sqlite.hpp`;会话永久删除 `SessionStorage::purge_session_files`;daemon 内置命令白名单 `SessionRegistry::execute_builtin_command`;`/lsp` 那种「一份实现、两端调用」的文本命令分发模式。
- **分层约束**:`src/host/app_runtime/` 是重构计划为「三入口共用组合根」预留的模块,本期不得创建(重构 D10:本期保持三入口差异)。
- **参考实现**(源码调研,均默认关闭):Codex 会话闲置 6 小时后用便宜模型逐会话提炼、再由单写者整合,注入 ≤2500 token 的摘要且只在会话开始 / 压缩后注入,作业靠 SQLite 租约;grok-build v2 每回合后提炼「观察」进收件箱,满 20 条或 24 小时整合为主题文件,整合操作必须引用观察作为证据,删除留墓碑,索引每会话注入一次并持久化。

## Goals / Non-Goals

**Goals:**
- 记忆的装配、注入规则、命令语义只有一份实现,三个入口调用同一个入口函数。
- 注入内容在会话内稳定,后台写入与其他进程写入都不会改变进行中会话的请求前缀。
- 多进程共享数据目录时写入不丢、作业不重复。
- 记忆摘要默认关闭;开启后成本有界(闲置触发、每轮上限、输入预算),每一处自动改动都可追溯(来源、依据、归档)。

**Non-Goals:**
- 不创建 `host/app_runtime`;记忆装配放在 `session_host`,接口按日后整体迁入组合根的方式设计。
- 不引入新的第三方依赖(不做 FTS / 向量检索)。
- 不用会调用工具的子代理做整合。
- 会话记录文件格式不变,只在会话 meta 增加一个可省略字段。

## Decisions

### D1 装配:`session_host` 里的 `MemoryRuntime`
新增 `src/host/session_host/memory_runtime.{hpp,cpp}`:持有记忆存储(全局一份、工作区按需打开)、运行时记忆配置快照、工具注册、上下文快照构建、`/memory` 命令分发,以及(daemon 与 TUI 中)记忆摘要调度器。三个组合根各调用一次 `create_memory_runtime(config, data_dir, surface)`,把它交给 `ToolExecutor`、`SessionRegistryDeps` 与 `AgentLoopServices`;子会话沿用父会话的运行时。

- 备选:现在就建 `host/app_runtime` —— 违反重构的阶段安排,拒绝。
- 备选:把记忆工具塞进 `register_session_builtin_tools` —— 那里只放无状态工具,记忆工具需要按会话解析作用域,拒绝。

### D2 作用域:以会话自己的项目目录为准
全局作用域 `<data_dir>/memory/`;工作区作用域 `get_project_dir(会话 cwd)/memory/`,与会话存储同一个标识,worktree 会话的存储本就归主工作区。解析作用域一律用**会话**的项目目录(工具上下文里的会话与工作区信息),不用进程 cwd —— daemon 一个进程服务多个工作区,拿进程 cwd 会读错目录(CLAUDE.md 记录过同族问题)。`MemoryRegistry` 泛化为按目录实例化的存储,全局与各工作区各一个实例。

- 备选:按 git 远端识别 —— 用户明确否决(对一般用户不友好)。

### D3 注入:按会话冻结的记忆快照
把记忆块从会话上下文块里拆出来单独缓存:会话第一次请求时渲染一次,存进 `PromptContextCache`(连同当时的压缩代数);之后同一会话直接复用,压缩代数变化(摘要压缩或线程修复)或会话恢复时重建。渲染规则:按作用域分段,每段逐行取 `MEMORY.md` 条目并附条目 `updated_at` 距今天数,超过 `max_index_bytes`(默认改为 8 KiB,每个作用域)时截断并加省略说明;末尾附固定的使用说明(记忆是历史、用前核实、当前指令优先)。

- 备选:保留「按内容 key 每次请求重建」—— 记忆摘要与其他进程的写入会在回合中途改变请求内容,打穿 prompt cache,拒绝。
- 备选:像 grok 那样把索引写进会话记录持久化 —— ACECode 的会话上下文约定是「只进请求、不落盘」,不改变这一约定。

### D4 条目元数据与兼容
frontmatter 增加 `created_at`、`updated_at`、`source`、`source_sessions`;解析宽容,旧文件照常加载,系统写入时补齐(`created_at` 取文件原修改时间)。旧版本只认全局目录、忽略未知字段,因此新数据对旧版本向后兼容(工作区记忆对旧版本不可见)。

### D5 状态库:`<data_dir>/memory/state.sqlite3`
所有进程共用一个库(WAL、busy_timeout),表:提炼进度(每会话一行:工作区、提炼位置、尝试次数、租约持有者与到期时间、最近错误)、整合进度(每作用域一行,同样带租约)、墓碑(作用域、名字、规范化标题、删除时间)、状态摘要(最近提炼 / 整合时间与错误)。租约持有者令牌为「pid + 随机数」,长作业定期续约;过期即可被其他进程接手。提交做成幂等(按观察 id 与计划 hash 去重),崩溃后重放不会重复写入。

- 备选:锁文件(grok 旧版)—— 状态展示、墓碑、调度都需要查询,仍要一个库,拒绝。
- 备选:复用各工作区的 `state.sqlite3`(goal 存储)—— 全局作用域与跨工作区调度需要单一的库;goal 存储以「文件存在」作为惰性打开条件,不宜混用,拒绝。

### D6 多进程写入:状态库事务做作用域写锁
每次写条目 / 重建索引 / 整合应用,在状态库上开 `BEGIN IMMEDIATE` 事务作为该作用域的跨进程写锁,锁内先按磁盘重扫再写。重扫(已在修复分支落地)解决「缓存旧」,写锁解决「两个进程同时重扫再同时写」的窗口。

### D7 脱敏:手写扫描器,单一收口
`src/domain/memory/secret_redaction.{hpp,cpp}`,在存储层写入口统一调用,覆盖工具写入、观察、整合产物、网页编辑四条路径。识别:已知前缀的密钥(`sk-`、`ghp_`、`gho_`、`xai-`、`AKIA` 等)、`Bearer` 令牌、PEM 私钥块、URL 中的用户名密码、`password|passwd|pwd|token|secret|api_key|apikey|密码` 后跟 `=` / `:` / `:` 的值。用逐字符扫描实现。

- 备选:`std::regex` —— 大输入下 MSVC 实现慢且有栈溢出风险,项目里同类判定都改成了手写扫描,拒绝。

### D8 `/memory`:一份文本实现,两端调用
`dispatch_memory_command(args, ctx) -> text` 放在 `session_host`,TUI 内置命令与 daemon 内置命令白名单都调用它,网页斜杠下拉列出它;只有 `edit` 分端处理(TUI 开编辑器、网页提示去设置页)。`flush` 立即返回「已开始整理」,完成后以系统通知(只进界面记录)报告处理的观察数与改动的条目数,避免 REST 调用被模型请求阻塞。

### D9 会话级开关
会话 meta 新增 `memory_mode`(`off` 时写入,默认省略即开启)。`AgentLoop` 在回合开始读取:关闭时不注入记忆块,并在模型侧工具表里过滤掉记忆工具(与按模型族裁剪编辑工具同一处),调度器跳过该会话。

### D10 设置:配置文件为准,调度器按修改时间重载
配置新增 `memory.summary.{enabled=false, model_name="", idle_minutes=30, max_session_age_days=7}`;`max_index_bytes` 默认改为 8 KiB。修改走 `settings_mutations` 新增的 `set_memory_settings`,网页 `/api/config/memory` 与 TUI 设置中心共用。每个 daemon 各持一份内存配置,调度器每轮检查 `config.json` 修改时间,变了就重读 `memory` 段,所以别的进程改的设置一分钟内生效。

- 备选:把设置存进状态库 —— `config.json` 是用户可见、可手改的设置来源,其他设置都在那里,拒绝。

### D11 调度器
`MemorySummaryScheduler` 只在 daemon 与 TUI 进程中启动(headless 不启动),每 2 分钟一轮:枚举本进程服务的工作区中最近 `max_session_age_days` 内活动过的会话(用分页的 meta 读取,不全量扫描),按规格筛出待提炼会话;会话是否「正在进行」看注册表的忙碌状态与会话写入租约;每进程同时最多一个提炼请求、每轮最多 2 个会话;提炼成功后检查对应作用域是否达到整合条件。

### D12 提炼
输入取规范会话记录中提炼位置(消息序号)之后的部分,只保留用户可见的用户消息、助手正文与工具结果;按「用户原话 > 助手最终结论 > 助手中间说明 > 工具输出」逐级裁剪到预算内(摘要模型窗口的 60%,工具输出单条 2 KB)。模型请求不带工具、非流式,输出按规格的 JSON 严格解析(多余字段即拒)。模型解析顺序:`memory.summary.model_name` → 会话最后使用的模型 → 默认模型(沿用自动标题的解析与创建函数)。PA 网关报上下文超限时下次重试把输入预算减半。

### D13 整合:JSON 操作计划
把作用域现有条目(`source: manual` 的标为只读)、本批观察、墓碑一起交给模型,模型返回操作计划(`create` / `update` / `merge` / `delete`,每项带依据观察 id)。先整体校验(规格列出的全部规则),通过后在作用域写锁内应用:写条目、重建索引、把本批观察移入 `archive/<日期>/`、记录计划 hash;任一步失败整批回滚到应用前状态并记录错误。归档每轮清理 30 天前的内容。

- 备选:Codex 式「受限子代理 + 文件工具 + diff 基线」—— 需要沙盒化文件操作、产物校验更难、token 更多;JSON 计划可确定性校验、可单测,拒绝前者。

### D14 遗忘
在会话永久删除的调用路径(REST `DELETE /api/sessions/:id?purge=1`、TUI `/tasks clear` 等最终都走 `purge_session_files`)后调用 `MemoryRuntime::forget_session`:删收件箱与归档中该会话的观察,从 `source: summary` 条目的来源里移除该会话,无来源者删除并记墓碑。

### D15 网页与 TUI 界面
网页:个性化页新增「记忆」区,组件 `MemorySettings.jsx`,纯逻辑 `lib/memorySettings.js`(Node 单测),`api.js` 新增 `/api/config/memory` 与 `/api/memory*`;样式遵循 acecode-frontend-style。TUI:设置中心个性化页新增开关与模型选择,条目管理用 `/memory`。

## Risks / Trade-offs

- [摘要成本] → 默认关闭;闲置触发;每轮上限;输入预算;可选本地或轻量模型。
- [记忆投毒:工具输出或网页内容诱导写入] → 提炼提示要求忽略对话中的指令;观察带来源可追溯;整合必须引用依据;手写条目只读;界面可审阅、删除;外部内容污染标记列为后续。
- [观察质量参差、越积越多] → 允许 `noop`;标题与陈述有长度上限;整合后本批全部归档;归档 30 天清理。
- [预算截断导致条目看不见] → 省略说明 + `memory_read` 可查;条目按更新时间倒序列出。
- [PA 网关对摘要请求随机拒收] → 失败隔离、退避重试,超限时预算减半。
- [多进程时钟与崩溃] → 租约带到期时间与持有者令牌,提交幂等。
- [配置重载延迟] → 规格承诺一分钟内;调度器每轮检查修改时间。
- [默认预算从 32 KiB 降到 8 KiB] → 发布说明写明;超出部分仍可经 `memory_read` 获取。
- [主规格格式] `openspec/specs/global-memory/spec.md` 仍是 `## ADDED Requirements` 形态,解析出的需求为 0,归档会拒绝本变更的 MODIFIED → 归档前先把主规格规范为 `## Requirements`。

## Migration Plan

1. **一期(不增加模型调用)**:`MemoryRuntime` 与三入口接线、两层作用域、会话快照注入、元数据、脱敏、状态库与写锁、`/memory` 两端统一、会话开关、REST 与网页 / TUI 设置(记忆摘要开关此时可见但调度器未上线时保持禁用)。
2. **二期(记忆摘要)**:调度器、提炼、整合、遗忘与墓碑、状态展示;开关解禁。
3. **回滚**:`memory.enabled=false` 关闭全部记忆;记忆摘要默认关闭;数据格式对旧版本向后兼容,回退版本后全局记忆照常可用。

## Open Questions

- 闲置阈值 30 分钟、整合阈值(20 条 / 24 小时)与每轮上限 2 个会话是否合适,可在上线后按实际调用量调整,不影响规格与任务拆分。
