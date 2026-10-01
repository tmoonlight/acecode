# Tasks: unify-memory-system

一期 = 第 1–7 组(统一实现、作用域、注入、命令、设置与界面,不增加模型调用);二期 = 第 8–9 组(记忆摘要)。测试源码按项目约定写中文场景注释。

## 1. 规格基线

- [x] 1.1 把主规格 `openspec/specs/global-memory/spec.md` 的 `## ADDED Requirements` 规范为 `## Requirements`,验证:`openspec show global-memory --type spec --json --no-scenarios` 解析出 8 条需求,本变更 `openspec validate` 不再提示归档会拒绝 MODIFIED

## 2. 存储层(`src/domain/memory/`)

- [x] 2.1 把 `MemoryRegistry` 泛化为按目录实例化的存储(全局 / 工作区各一个实例),原有接口行为不变,验证:`tests/memory/memory_registry_test.cpp` 现有用例全过,新增「两个作用域互不影响」用例
- [x] 2.2 frontmatter 读写 `created_at` / `updated_at` / `source` / `source_sessions`,旧条目在系统写入时补齐(`created_at` 取文件修改时间),未知字段保留,验证:frontmatter 单测覆盖旧条目补齐与未知字段往返
- [x] 2.3 新增 `secret_redaction`(逐字符扫描,不用 `std::regex`),覆盖规格列出的密钥前缀、`Bearer`、PEM 私钥块、URL 凭据、`password|passwd|pwd|token|secret|api_key|apikey|密码` 赋值(含全角冒号),验证:单测逐类命中且普通文本(含 `token` 一词的自然语言)不被误改
- [x] 2.4 新增状态库 `<data_dir>/memory/state.sqlite3`(WAL、busy_timeout):提炼进度、整合进度、墓碑、状态摘要四张表;租约(持有者令牌、到期、续约、过期接管)与幂等提交,验证:单测覆盖租约互斥、过期接管、墓碑 90 天过期
- [x] 2.5 作用域写锁:写条目 / 重建索引 / 应用整合时在状态库上开 `BEGIN IMMEDIATE`,锁内按磁盘重扫,验证:两个存储实例 + 两个库连接交替写入,`MEMORY.md` 保留全部条目(在修复分支的 `UpsertKeepsEntriesWrittenByAnotherProcess` 基础上扩展)

## 3. 记忆运行时与三入口接线

- [x] 3.1 新增 `src/host/session_host/memory_runtime.{hpp,cpp}`:`create_memory_runtime`、按会话项目目录解析作用域(不用进程 cwd)、工具注册、快照构建入口、`forget_session`,验证:单测覆盖 daemon 多工作区时按会话解析到正确目录
- [x] 3.2 TUI(`tui_runtime_init.cpp`)、daemon(`worker.cpp`)、headless(`headless_runner.cpp`)改为调用 `create_memory_runtime`,移除各自的记忆接线(含修复分支里 daemon 的临时接线),验证:`acecode -p --list-tools` 列出 `memory_read` / `memory_write`;web smoke 新建会话的工具表含两者;TUI 现有记忆用例通过
- [x] 3.3 子会话沿用父会话的记忆运行时与工作区作用域,验证:子会话 `memory_read({})` 返回父会话工作区条目的用例
- [x] 3.4 headless 的记忆工具可被 `--disable-tools` 移除且不启动记忆摘要调度器,验证:headless 选项单测

## 4. 记忆工具

- [x] 4.1 `memory_read` 增加 `scope` / `query`,按名字先工作区后全局,读前重扫,验证:`memory_read_tool_test` 覆盖作用域优先级、关键字命中片段、读到另一个存储实例刚写的条目
- [x] 4.2 `memory_write` 增加 `scope`(缺省按类型推断,无工作区时写全局),写前脱敏并在结果中提示,记录来源与时间,走作用域写锁,验证:`memory_write_tool_test` 覆盖推断规则、显式作用域、脱敏提示、来源字段
- [x] 4.3 权限:两类作用域目录内的写入按规格自动放行,解析到作用域外(含符号链接)一律拒绝,验证:`permissions_test` 新增工作区作用域与符号链接逃逸用例

## 5. 记忆上下文快照

- [x] 5.1 把记忆块从会话上下文块拆出,由 `PromptContextCache` 按会话冻结,压缩代数变化或会话恢复时重建,验证:agent loop 用例「会话中 `memory_write` 后后续请求逐字节不变、压缩后出现新条目」,且 `RequestPrefixIsByteStableAcrossIterationsInATurn` 仍通过
- [x] 5.2 渲染:按作用域分段、条目附距今天数、每作用域 `max_index_bytes` 预算与省略说明、固定使用说明;两个作用域都为空时不注入,验证:`system_prompt_test` 新增预算截断、天数标注、为空不注入、工作区隔离用例
- [x] 5.3 `memory.max_index_bytes` 默认改为 8 KiB,验证:配置默认值单测;发布说明记录该变化

## 6. `/memory` 命令与会话开关

- [x] 6.1 在 `session_host` 实现 `dispatch_memory_command`(list / view / forget / flush / off / on / reload,edit 分端),forget 记录墓碑,验证:单测逐个子命令核对输出文本
- [x] 6.2 TUI `/memory` 改为调用 `dispatch_memory_command`(edit 仍用 `$EDITOR`),验证:TUI 命令单测
- [x] 6.3 daemon 内置命令白名单加入 `memory`,网页斜杠下拉列出,验证:`commands_handler_test` 与 web smoke 中 `/memory list` 输出与 TUI 相同
- [x] 6.4 会话 meta 新增 `memory_mode`,`AgentLoop` 关闭时不注入记忆块、在模型侧工具表过滤记忆工具,验证:agent loop 用例覆盖 `/memory off` 后请求不含记忆块与工具、恢复会话后保持、`/memory on` 后恢复

## 7. 设置、REST 与界面

- [x] 7.1 配置新增 `memory.summary.{enabled,model_name,idle_minutes,max_session_age_days}`(稀疏写回),`settings_mutations` 新增 `set_memory_settings`,验证:配置解析 / 写回单测与 settings_mutations 单测
- [x] 7.2 REST:`GET/PUT /api/config/memory`,`GET /api/memory`(按工作区列出两个作用域 + 状态)、`GET/PUT/DELETE /api/memory/<scope>/<name>`、`POST /api/memory/reset`;纯函数 handler + routes,编辑走脱敏与写锁,删除记墓碑,验证:handler 单测 + web smoke;`docs/daemon-api.md` 同步
- [x] 7.3 网页「设置 > 个性化」新增「记忆」区:使用记忆开关、记忆摘要开关与模型(「当前模型」+ 已保存模型,一期后端调度未上线前禁用)、全局 / 当前工作区条目浏览、查看编辑删除、二次确认重置、状态显示;纯逻辑 `lib/memorySettings.js` 登记 `runTests.js`,验证:`pnpm test`、`pnpm build` 通过;新中文文案先补 `i18n-en-overrides.mjs` 再 `pnpm i18n:catalog`
- [x] 7.4 TUI 设置中心个性化页新增使用记忆开关、记忆摘要开关与模型选择,写入同一配置,验证:在 TUI 修改后 `GET /api/config/memory` 返回一致的值

## 8. 记忆摘要:调度与提炼(二期)

- [x] 8.1 `MemorySummaryScheduler`:只在 daemon 与 TUI 启动,2 分钟一轮,每轮检查 `config.json` 修改时间并重读 `memory` 段,每进程同时 1 个提炼、每轮最多 2 个会话,验证:注入时钟与配置读取的单测覆盖重载与上限
- [x] 8.2 待提炼会话筛选:工作区会话、非子会话、非 headless、未关闭记忆、有新的可见消息、闲置 ≥ `idle_minutes`、7 天内活动、未被其他进程持有,验证:单测逐条排除规则
- [x] 8.3 提炼输入:提炼位置之后的可见消息,按优先级裁剪到摘要模型窗口 60%,工具输出单条 2 KB,验证:单测覆盖裁剪顺序与预算
- [x] 8.4 模型解析(`memory.summary.model_name` → 会话最后使用的模型 → 默认模型)与无工具请求,复用自动标题的解析与创建函数,验证:stub provider 单测断言请求发给了正确模型且不带工具
- [x] 8.5 输出严格校验(多余字段、非法取值、超长即作废),同一范围最多 3 次,PA 上下文超限时预算减半,验证:单测覆盖 noop、多余字段、超长、重试上限
- [x] 8.6 观察脱敏后写入对应作用域 `inbox/`(带会话 id、消息范围、时间),写入成功才推进提炼位置,崩溃重放不重复,验证:单测模拟写入后崩溃再重放
- [x] 8.7 关闭记忆摘要或关闭使用记忆时停止调度并丢弃进行中结果,验证:单测

## 9. 记忆摘要:整合、遗忘与状态(二期)

- [x] 9.1 整合触发(满 20 条 / 最早超 24 小时 / `flush`)与作用域租约,每批最多 100 条,验证:单测覆盖触发条件与并发只执行一次
- [x] 9.2 计划校验:每个操作引用本批观察、名字合法、`source: manual` 只读、墓碑名字与标题(忽略大小写)不得创建、描述 ≤150 字符、正文 ≤4 KiB、脱敏,验证:单测逐条规则各一个作废用例
- [x] 9.3 应用与归档:写锁内写条目、重建索引、本批观察全部移入 `archive/<日期>/`、记录计划 hash;任一步失败回滚;归档 30 天清理;连续 3 次失败保留观察并记录错误,验证:单测覆盖成功、回滚、幂等重放、清理
- [x] 9.4 `forget_session` 接入会话永久删除路径(REST `?purge=1`、TUI `/tasks clear`):删观察、从来源移除、无来源条目删除并记墓碑、手写条目保留,验证:web smoke 永久删除后条目撤回
- [x] 9.5 `/memory flush` 异步执行,立即回复已开始,完成后发系统通知报告观察数与改动条目数;未开启时只返回说明,验证:agent / web 测试
- [x] 9.6 状态摘要(两个作用域收件箱计数、最近提炼与整合时间、最近错误)接入 `GET /api/memory` 与个性化页,开关解禁,验证:handler 单测 + 前端测试

## 10. 文档与整体验证

- [x] 10.1 更新 CLAUDE.md(记忆一节)、`docs/daemon-api.md`、用户帮助中记忆相关内容,验证:文档描述与接口、默认值一致
- [x] 10.2 Windows 本机验证:构建 `acecode`、`acecode-desktop`、`acecode_unit_tests`,`python scripts/refactor/run_fast_tests.py --profile fast` 全绿,`pnpm test` / `pnpm build` 通过
- [x] 10.3 端到端手测并记录:桌面版开启记忆摘要 → 会话闲置后收件箱出现观察 → `/memory flush` 整合出条目 → 新会话注入可见 → 永久删除来源会话后条目撤回;TUI 与网页 `/memory list` 输出一致
