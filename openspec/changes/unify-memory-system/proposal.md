# Proposal: unify-memory-system

## Why

ACECode 的记忆只有「一个全局目录 + 每次请求注入索引 + `memory_read` / `memory_write` + TUI `/memory`」,而且只在 TUI 里完整接线:daemon(桌面版 / 网页版)和 headless 一直给记忆传空指针,桌面用户的模型根本没有记忆工具。反馈 LINDANDAN069 暴露了后果:用户让模型「记住这个处理方式」,模型只能把经验写进子目录 `CLAUDE.md`、`.acecode/MEMORY.md` 这类 ACECode 从不自动加载的文件,压缩之后就「忘了」,还重写已验证过的函数。

对照 Codex(`codex-rs/memories`)与 grok-build(`xai-grok-memory` v2)的源码,两家都把记忆做成了后台自动积累、分层整理、有预算注入的一套机制;ACECode 缺的不是某个工具,而是一套三个入口共用、能自动积累、用户看得见管得了的记忆系统。

## What Changes

- **一套实现,三个入口共用**:TUI、daemon、headless 通过同一个装配入口拿到记忆服务(存储、工具、注入、命令),不再各自接线;子会话沿用父会话的记忆服务。
- **两层作用域**:全局(个人偏好,`<data_dir>/memory/`,现有条目原样保留)+ 工作区(`<data_dir>/projects/<工作区 hash>/memory/`,按现有工作区 hash 识别,不看 git 远端)。worktree 会话的存储本来就归主工作区,记忆随之共享。
- **注入改为会话快照**:会话首次请求时生成一份有预算的记忆索引(全局 + 工作区),压缩后重建;会话中途写入不改变本会话已注入的内容,不打穿 prompt cache。索引条目带「N 天前」,提示词明确记忆是历史记录、用前核实、用户当前指令优先。
- **条目元数据**:创建 / 更新时间、来源(手写 / 摘要生成)、来源会话;写入前统一做敏感信息脱敏(密钥、token、密码、带凭据的 URL)。
- **工具扩展**:`memory_read` 支持作用域与关键字查找;`memory_write` 支持作用域(未指定时按类型推断),写入走脱敏与来源记录。
- **管理界面统一**:`/memory` 在 TUI 与网页斜杠命令里是同一套子命令(列出 / 查看 / 删除 / 立即整理 / 本会话开关);网页「设置 > 个性化」新增「记忆」区:使用记忆开关、**记忆摘要开关(默认关)与摘要模型(默认当前模型)**、按作用域浏览 / 编辑 / 删除条目、一键重置。
- **多进程安全**:新增记忆状态库(SQLite,WAL + 租约),TUI 与多个 daemon 同时运行时写入与后台作业互斥;写前按磁盘重扫(已在 `claude/fix-feedback-0930` 落地)。
- **自动积累(记忆摘要,默认关)**:会话闲置一段时间后,用摘要模型把会话提炼成「观察」放进作用域的收件箱;收件箱攒够一批或到时间后整合成正式条目(新建 / 更新 / 合并 / 删除,每步必须引用收件箱里的观察作为依据),原观察归档;`/memory flush` 手动触发。清除会话时撤回只来源于它的记忆;用户删除的条目留墓碑,整合不得再造。

## Capabilities

### New Capabilities

- `memory-summarization`:记忆摘要(自动提炼 + 整合)的开关、模型选择、触发时机、提炼与整合规则、遗忘与墓碑、作业并发与失败处理。

### Modified Capabilities

- `global-memory`:目录布局改为全局 + 工作区两层并新增收件箱 / 归档 / 状态库;条目元数据扩展;注入从「每次请求注入全局索引」改为「会话快照 + 预算 + 陈旧度标注」;`memory_read` / `memory_write` 增加作用域与脱敏;`/memory` 命令三端一致并新增子命令;线程安全扩展为多进程安全;新增三入口一致与网页管理界面要求。

## Non-goals

- 向量 / 嵌入检索与 FTS 全文检索(两家参考实现默认都不启用;记忆规模上来后另立项)。
- 按 git 远端识别工作区。
- 模型主动删除记忆(删除只来自用户操作与会话清除;模型只能新增 / 更新)。
- 引用统计与按使用率淘汰、外部内容污染标记、从 Claude Code / Codex 导入记忆(列为后续候选)。
- 修改项目指令文件(AGENTS.md / CLAUDE.md)的加载规则。

## Impact

- 代码:`src/domain/memory/`(作用域、元数据、脱敏、状态库、墓碑)、新增记忆服务装配与后台作业(`src/host/`)、`src/adapters/tool/memory_*_tool`、`src/engine/prompt/system_prompt.cpp` 与请求上下文注入、`src/apps/{tui,daemon,headless}` 三个入口、`SessionRegistry` 内置命令白名单与会话清除钩子、`src/base/config`(`memory.summary.*` 等新配置)。
- 接口:新增 `/api/config/memory`、`/api/memory*` REST;`docs/daemon-api.md` 同步。
- 前端:`SettingsPage` 个性化区、新组件与纯逻辑模块、`api.js`、i18n 目录。
- 数据:新增目录与 `memory/state.sqlite3`;现有 `~/.acecode/memory/` 条目兼容读取、首次写入时补元数据。
- 成本:记忆摘要开启后,每个闲置会话一次模型调用、每批整合一次模型调用;默认关闭。
