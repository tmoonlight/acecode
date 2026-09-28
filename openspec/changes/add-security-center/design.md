<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design: add-security-center

## Context

沙盒的策略模型(`SandboxPolicy` 清单)、规则文件(`ExecRules`)、审批决策(`PermissionResult` 五态)都已在 `align-codex-sandboxing` 落地,本期不改判定语义,只补三样:一个能查询的审计存储、几条 REST、一个设置页。参照物是 WorkBuddy「安全中心」的信息架构,不照搬它做不到的承诺(Windows 读隔离 / 真断网)。

## Decisions

### D1. 审计是进程级单例 SQLite,记录点只在 AgentLoop 审批门

`security::AuditLog` 与 `sandbox::SandboxRuntime::instance()` 同款:进程级单例,三个入口(daemon worker / TUI main / headless)启动时 `configure(<data_dir>)`,未 configure 时 `record()` 是 no-op。文件 `<data_dir>/security/audit.sqlite3`,WAL + `synchronous=NORMAL`,单表 `audit_entries`,写入在调用线程同步完成(一次 insert 远小于一次进程 spawn)。数据目录迁移按后缀识别 `.sqlite3`,自动走 backup API,无需登记。

AgentLoop 通过 `set_audit_sink(std::function<void(const AuditEntry&)>)` 注入接收器,默认落到单例;单测注入 lambda 收集,不碰磁盘。记录点集中在 `execute_tool_batch` 的审批门,用一个 `record_audit` 局部 lambda 在每个「决定已作出」的分支调用一次:

| 分支 | category | decision | source |
|---|---|---|---|
| bash 规则 forbidden / 无人值守越权 | command | forbidden | rule / goal |
| bash 自动放行 | command | allow | rule / session / auto(按 `ExecDecision::reason`) |
| bash Prompt → goal 自动放行 | command | allow | goal |
| bash / 文件工具 Prompt → hook | command / file / tool | allow / deny | hook |
| Prompt → headless | 同上 | allow / deny | headless |
| Prompt → 用户 | 同上 | allow / allow_session / allow_scoped / allow_remember / deny | user |
| Prompt → 无确认通道 | 同上 | deny | none |
| 文件工具硬拒绝(内置保护规则 / yolo Deny / 写边界 / 规则目录) | file | forbidden | rule |
| 沙盒拒绝(post-exec `sandbox_violation`) | sandbox | blocked | sandbox |
| `allow_remember` 写回 / `allow_scoped` 授权 / 额外权限会话授权 | rule | allow | user |

只读工具(file_read / grep / …)自动放行**不记**,否则每回合几十条读把日志淹掉;写工具自动放行(auto 模式)记一条,这正是用户想看的「AI 改了什么、凭什么」。`target` 是命令原文或首个路径,多路径进 `detail.paths`。

行数上限 20000:每插入 256 条检查一次,超出删最旧。`clear()` 清表并 `VACUUM`。

### D2. 沙盒配置走 `config.json`,PUT 后实时下发

`GET/PUT /api/config/sandbox` 读写 `AppConfig::sandbox`(`enabled` / `network_access` / `deny_defaults` / `filesystem.read|write|deny`),`windows_backend` 与 `writable_roots` / `exclude_tmpdir` 保持只读透传(前者是 MXC 口子,后两者是旧字段,新清单已覆盖)。PUT 校验:条目非空、去重;`~`、`:workspace_roots`、`:tmpdir`、`:acecode_home` 记号原样保留,其余必须是绝对 / 根路径(deny 允许 glob)。落盘后经 `SessionRegistry::refresh_sandbox_config` 对每个活跃会话 `enqueue_control` → `set_sandbox_config`,与 `refresh_mcp_policy` 同款,和回合串行,不在模型请求中途翻转策略。TUI 进程下次启动生效。

响应附 `platform`:`probe_backend(choice)` 的 kind / available / reason / network_enforced / network_best_effort,以及 `read_isolation`(macOS / Linux 为 true,Windows 受限令牌为 false)—— 前端据此把「禁止名单在 Windows 上只拦写」写在界面上,不含糊。

### D3. 命令规则只重写两个「ACECode 维护」文件

`default.rules` / `default.sandboxed.rules` 本来就是 `allow_remember` 机器写的文件,把它们定义为界面可编辑的「托管文件」是最自然的:用户在界面上看到的正是「记住」写进去的东西,也能删。重写 = 按规则表重新生成整个文件(每条 `prefix_rule(pattern=[...], decision="...", justification="...")`),丢弃注释与 `match` / `not_match`;写前先 `parse_rules_text` 做一次往返校验,解析不过不落盘。其它 `*.rules`(用户手写)只读列出,带解析错误提示。

界面把「决策」做成四选一:放行(沙盒外)→ `default.rules` allow;放行(沙盒内)→ `default.sandboxed.rules` allow;询问 / 禁止 → `default.rules` prompt / forbidden。禁用前缀名单(`is_banned_prefix`)对界面同样生效:`rm`、`sudo`、解释器这类不能作为放行前缀,PUT 报 400 指出是哪条。

PUT 后 `SessionRegistry::refresh_exec_rules` 让活跃会话 `reload_exec_rules()`(同样 enqueue_control)。

### D4. 审计查询是分页 + 筛选,导出走同一条查询

`GET /api/security/audit?category=&decision=&since_ms=&before_id=&q=&limit=`:按 id 倒序,`before_id` 游标分页,`q` 对 target / reason / tool 做 LIKE。`summary` 给总数、按 decision / category 的计数、最近一条时间,以及最近 20 条被拦路径(category=sandbox 去重计数)—— 文件安全页用它做「一键加入可写 / 禁止」。导出复用查询(上限 20000 行),JSONL 每行一个对象,CSV 带表头,响应 `Content-Disposition: attachment`。清空需要前端确认框(Modal;按仓库的对话框惯例,删除类确认框打开即默认选中「清空」,Esc / 取消退出)。

界面上的放行规则比「记住」多一道限制:整条 pattern 精确命中禁用名单(与 `derive_remember_patterns` 同源)**或**首 token 是解释器 / shell / rm / sudo 这类能承载任意行为的命令,都拒绝。`git` 单独被禁是因为太宽,`git push --force` 是合法规则,所以不能按「任一前导片段命中即拒」处理。

不做 WS 推送:审计页可见时 15 秒轮询一次汇总,列表手动刷新。事件本来就来自审批门,用户在聊天里已经看见了。

### D5. 设置页结构

`SETTINGS_NAV_GROUPS.coding` 追加 `{ key: 'security', label: '安全中心', icon: 'lock' }`。页面组件 `SecurityCenterSettings.jsx`,内部分页(概览 / 文件安全 / 命令安全 / 审计中心)用一行分段按钮切换,状态留在组件里。数据层 `lib/securityCenter.js` 纯函数(草稿归一、条目校验、规则行 ↔ payload、筛选 → query、行展示映射)+ Node 单测。视觉套 `acecode-frontend-style`。

### D6. 不做的与坑

- 项目级 `.acecode/rules` 不进界面(设置页无 workspace 上下文)。
- Windows 下 `filesystem.read` 不生效(受限令牌管不了读),界面标「仅 macOS / Linux」。
- 审计 `target` 里可能含命令原文,导出文件就是明文;不做脱敏(它本来就是用户自己的命令)。
- 前端所有新中文文案先补 `i18n-en-overrides.mjs` 再 `pnpm i18n:catalog`,否则 `sourceCatalog.test.js` 挂、生成脚本还会去联网机翻。
