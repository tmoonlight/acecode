# 验证记录

本次在 worktree `claude/unify-memory`(起点 master `5e9258d1`,另含同分支的两个反馈修复提交)上实现,Windows 11 本机 Ninja Release 构建。端到端使用临时 HOME 与用户现有模型配置的副本,没有读写用户真实的 `~/.acecode` 数据。

## 自动化(任务 10.2)

- 构建:`acecode`、`acecode-desktop`、`acecode_unit_tests` 均成功;前端 `pnpm build` 之后重新 configure,`acecode.exe` 嵌入的是新前端(daemon 实际返回的页面含记忆设置文案)。
- `python scripts/refactor/run_fast_tests.py --profile fast`:inventory 5425,执行 4811,失败 0(`not_run` 613 为快速档按设计排除的慢用例)。
- 追加跑了 `--profile full`:执行 5424,失败 0。
- 记忆相关筛选(`*Memory*:*memory*:SecretRedaction*:...`)275 项通过,包括 `MemorySummaryTest.ToleratesReasoningAroundJson`(模型把推理写进正文时仍能取出 JSON)。
- `web`:`pnpm test` 2920 项通过、0 失败;`pnpm build` 通过,正则兼容性检查通过。新中文文案经 `i18n-en-overrides.mjs` + `pnpm i18n:catalog` 生成。
- 静态闸门:`check_layers.py --layout final --strict`、`check_doc_paths.py --strict`、`normalize_includes.py --check`(src / tests)、`check_file_size.py --strict`、`check_ownership.py --strict --final`、`validate_map.py --strict`、`check_layer_libraries.py`(File API)、`scripts/refactor/tests` 99 项均通过。
- `acecode --validate-models-registry` 在指定 `ACECODE_MODELS_DEV_DIR` 后通过(构建目录不在源码树内,默认向上查找不到 `assets/models_dev`,与本次改动无关)。
- `openspec validate unify-memory-system --strict --no-interactive` 与 `git diff --cached --check` 通过。

## 端到端:/memory flush 路径

临时 HOME + daemon REST(桌面版与网页共用的后台),真实模型 `darkness`(会把推理写进正文)。

1. `PUT /api/config/memory {"summary":{"enabled":true}}` 生效,`summary_available: true`。
2. 新会话陈述「本项目正式发布只在周五进行」,只要求回复「收到」,不要求模型记忆。
3. `/memory flush` 立即返回已开始;完成后对话里出现系统消息 `Memory flush finished: 1 new observation(s), 1 observation(s) consolidated, 1 memory entry changed.`,工作区出现条目 `release_friday_only`(`type: project`,`source: summary`,`source_sessions` 为该会话),状态中两个收件箱回到 0、最近提炼 / 整合时间已记录。
4. 另一个新会话只凭上下文回答「发布安排在星期几」:模型引用了注入的 `[project] release_friday_only — 本项目正式发布仅安排在周五进行…`,回答「星期五」。
5. 归档并 `DELETE /api/sessions/<id>?purge=1` 后,工作区条目被撤回(引用该会话的摘要条目为空)。

首轮手测发现推理型模型输出的 JSON 前后带推理文字,提炼被判为无效;修复为容错提取 JSON 对象并在无效时记录输出样本,flush 报告改为写入对话记录,修复后重跑即上面的结果。

## 端到端:会话闲置自动提炼路径(任务 10.3)

同样的临时 HOME 与模型,`PUT /api/config/memory {"summary":{"enabled":true,"idle_minutes":5}}`(允许的最小闲置时间)。

1. 新会话说明「习惯用 pnpm 而不是 npm 管理前端依赖」,只要求回复「好的」。
2. 不做任何操作。回合结束约 375 秒后(闲置满 5 分钟后的第一轮调度),状态变为全局收件箱 1、记录了最近提炼时间,`memory/inbox/<会话>-0-5.json` 中是一条 `feedback` 观察「前端依赖管理统一使用 pnpm」;未满 20 条,尚未整合。
3. `/memory flush`:`Memory flush finished: 0 new observation(s), 1 observation(s) consolidated, 1 memory entry changed.`,全局出现 `frontend_pnpm`(`source: summary`,来源为该会话),收件箱回到 0,记录了最近整合时间。
4. 新会话只凭上下文回答「平时用哪个包管理器」:回答「pnpm」。
5. 归档并永久删除来源会话后,`frontend_pnpm` 被撤回,两个作用域都没有剩余条目。

## 网页界面

daemon 使用嵌入资源(去掉配置副本里指向主检出旧前端的 `web.static_dir`),浏览器打开:

- 设置 > 个性化 > 记忆:使用记忆、记忆摘要开关、摘要模型(「当前模型」+ 已保存模型)、全局 / 当前工作区条目页签、重置;无工作区时提示「当前没有打开工作区」。
- 打开记忆摘要后出现状态区(待整合观察:全局 / 当前工作区计数,最近提炼、最近整合);`GET /api/config/memory` 确认已写回。
- 在工作区会话里让模型用 `memory_write` 记一条项目约定,设置页「当前工作区 1」列出该条目;编辑正文追加 `password=...` 保存后提示 1 处疑似密钥已替换,`GET /api/memory/workspace/<name>` 返回 `password=[REDACTED]`;删除有二次确认,删除后条目消失并写入墓碑。
- 会话里输入 `/memory list`,对话出现「记忆信息」系统通知,内容为全局无条目、工作区 1 条 `[project] commit-message-language — … (today)`。

## TUI 与网页 /memory 一致性

TUI 的 `/memory` 与网页内置命令都调用 `dispatch_memory_command`;`TuiMemoryCommand.ListUsesSharedDispatcherText` 注册真实的 TUI 命令执行 `/memory list`,与网页那份文本逐字比较。本次没有在交互式终端里手动运行 TUI。

## 验证边界

端到端走的是桌面版所用的 daemon 与嵌入网页,在浏览器里操作;没有启动 `acecode-desktop` 窗口本身,也没有在 macOS / Linux 上验证(按仓库约定留待多平台补验)。

## 2026-10-02 发布补验

v0.9.31 的最终 master CI 已通过，但 macOS arm64 / x64 打包均在 `memory_scheduler.cpp` 编译失败：libc++ 的文件时钟计数类型使 `std::to_string` 重载不明确。缓存改为 `std::optional<std::filesystem::file_time_type>`，直接比较原生修改时间，不降低精度、不依赖时钟纪元。修复通过 v0.9.32 接续发布；Windows CLI / Desktop / 单测增量构建通过，CLI 输出 `acecode v0.9.32`；记忆调度、摘要、运行时与守护测试共 37 项通过，0 失败。分层、所有权、OpenSpec 严格验证及 `git diff --check` 通过。最终 macOS 构建及签名、公证以 [v0.9.32 发布流水线](https://github.com/tmoonlight/acecode/actions?query=branch%3Av0.9.32) 的结果为准。
