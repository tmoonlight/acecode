<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design: align-codex-sandboxing

## Context

`add-auto-mode-sandbox` 已落地:决策表、`.rules` 子集、三平台后端、会话前缀记忆、升级协议。本期对照 codex-rs 主干 `sandboxing` crate 与 `core/exec_policy.rs`,补齐权限清单模型、拒绝分类、审批扩展、规则写回与两个平台的短板。非目标:Linux helper 二进制 / seccomp、网络代理、Windows elevated 账户、MXC 实现、PTY 会话审批。

## Decisions

### D1. 无人值守下越权与额外权限一律 Forbidden,不是自动批准

`ExecDecisionInput::unattended` 由 AgentLoop 用 `goal_unattended_active()` 填。`decide_exec` 在 forbidden / yolo 之后立即判:`unattended && (escalation_requested || additional_requested)` → `Forbidden`,reason `escalation_unattended`。这与 Codex `approval_policy=never` 下 `PROMPT_CONFLICT_REASON` 同款。AgentLoop 对该 reason 返回专门的工具结果文案,引导模型去掉参数留在沙盒里重试;其余 Prompt 仍按原语义自动放行(沙盒不变)。

### D2. 权限清单落在 `SandboxPolicy` 上,后端只消费派生字段

不引入 Codex 的 `PermissionProfile` 三态包装:acecode 的 `SandboxMode` 已经是 `Disabled(FullAccess) / Managed` 的等价物。`SandboxPolicy` 新增 `readable_roots`(空 = 全盘可读)、`denied_paths`(绝对路径,读写都拒,含子树)、`denied_globs`(deny 条目里的 glob)。

来源 `config.sandbox.filesystem.{read,write,deny}` + 内置默认 deny 名单 + 会话授权(`SessionGrants`)+ 单次 `additional_permissions`。`write` 与旧 `writable_roots` 合并。记号:`~`(家目录)、`:workspace_roots`(写边界根 + linked git dirs)、`:tmpdir`(专用临时根);未知记号跳过并记日志。`resolve_access(path)`:最长前缀命中优先,同深度 deny > write > read;`can_write` / `can_read` 供状态展示与「只放行该目录」的合法性检查。

Windows 受限令牌无法限制读取(WRITE_RESTRICTED 只影响写检查),`denied_*` 在 Windows 上只保证写不通;`/sandbox` 状态与文档写明「读隔离仅 macOS / Linux」。这是把 MXC 留作口子的主要理由。

### D3. 模型侧参数对齐 Codex,旧参数保留为别名

bash 参数:`sandbox_permissions`(`use_default` / `with_additional_permissions` / `require_escalated`)、`justification`、`additional_permissions{file_system{read,write}, network{enabled}}`、`prefix_rule`。`with_escalated_permissions=true` 等价 `require_escalated`。校验在 `validate_escalation_arguments`:非 use_default 必须带 justification;with_additional_permissions 必须带非空 additional_permissions;路径必须是绝对路径或 `~` 开头;与 deny 名单冲突的路径直接报参数错误(不弹确认)。

决策表新增第 4a 条:`additional_requested` 且会话授权尚未覆盖 → `Prompt`(reason `additional_permissions_requested`),批准后沙盒 = 模式沙盒 + 额外条目;已被会话授权覆盖 → 按普通命令走(条目并入策略)。

### D4. 拒绝分类与「只放行该目录」

`classify_sandbox_violation(exit_code, output, backend)` 移植 Codex `violation.rs`:原因枚举 + 路径抽取(POSIX `path: Permission denied` 形态、Windows `Access to the path 'X' is denied` 与盘符路径)+ 512 字符片段。结果进 `metadata.sandbox_violation`,提示文案带路径并给出最小申请建议。

AgentLoop 记 `last_sandbox_violation_`(会话内,换 cwd / 模式清空)。下一次 bash 越权确认(`escalation_requested` 且路径已知且不在 deny 名单)时,payload 带 `permission.scoped_write_root`(被拒路径所在目录),前端 / TUI 多一个「只放行写入该目录」选项;选中即 `AllowScoped`:写进会话授权,命令留在模式沙盒里带该目录执行。

### D5. 审批决策扩到五个,协议向后兼容

`PermissionResult` / `PermissionDecisionChoice` 新增 `AllowScoped`(`allow_scoped`)与 `AllowRemember`(`allow_remember`)。旧客户端不发这两个字符串;服务端对不适用的请求(payload 没有对应字段)把 `allow_scoped` 视为 `allow`、`allow_remember` 视为 `allow_session`。hooks 的 `permission_resolved.decision` 透传新字符串。

### D6. 规则写回:两个文件,不发明新语法

「批准并记住」按批准时的沙盒边界分流:`decision.sandbox == FullAccess && escalation_requested` → `<data_dir>/rules/default.rules`(全局 allow = 沙盒外);否则 → `<data_dir>/rules/default.sandboxed.rules`,加载时按 `RuleScope::Sandboxed` 降级为 AllowSandboxed(免确认仍沙盒)。两个文件都是标准 `prefix_rule(pattern=[...], decision="allow")`,与 Codex 互通。追加前去重(同 pattern 已存在则跳过)、写后 `reload_exec_rules()`。

前缀来源:模型 `prefix_rule` 优先(必须非空、不在禁用名单、且每一段命令都以它开头),否则每段用 `always_allow_prefix_tokens_for_segment`;任一段得不到前缀或命中禁用名单 → 不提供「记住」选项。禁用名单移植 Codex `BANNED_PREFIX_SUGGESTIONS`(解释器、`rm`、`sudo`、`cmd /c`、`powershell -Command`、`git` 单独等)。

### D7. Windows:准断网环境 + Job Object,不承诺断网

`network_access=false` 时对子进程环境套用 Codex `apply_no_network_to_env` 同款:代理变量指向 `http://127.0.0.1:9`、`NO_PROXY=localhost,127.0.0.1,::1`、`PIP_NO_INDEX=1`、`NPM_CONFIG_OFFLINE=true`、`CARGO_NET_OFFLINE=true`、`GIT_SSH_COMMAND=cmd /c exit 1`、`GIT_ALLOW_PROTOCOLS=`、`ACECODE_SANDBOX_NETWORK_DISABLED=1`,并把 `<data_dir>/sandbox/denybin`(ssh.cmd / scp.cmd 桩)插到 PATH 最前、PATHEXT 把 .BAT/.CMD 提前。`BackendProbe.network_enforced` 仍为 false,新增 `network_best_effort=true`,状态行显示 `best-effort offline (env)`。`additional_permissions.network.enabled=true` 批准后本次不套用。

bash 子进程以 `CREATE_SUSPENDED` 启动后 `AssignProcessToJobObject` 再 `ResumeThread`;Job 不设 KILL_ON_JOB_CLOSE(正常结束时后台孙进程照旧存活,维持 agent-browser 场景),超时 / 中止改为 `TerminateJobObject`。分配失败(嵌套 Job 被拒)退回旧的 TerminateProcess。沙盒与非沙盒路径共用。

### D8. macOS:禁读、防改名、平台默认项

Seatbelt 组装对齐 Codex `create_seatbelt_command_args_with_profile`:全盘可读且无 deny → `(allow file-read*)` + preferences policy;全盘可读有 deny → 对每个 deny 路径追加 `(deny file-read* file-write* (literal / subpath ...))`;受限读 → 只放行 `readable_roots` + `seatbelt_read_only_platform_defaults.sbpl`。deny glob → `(deny file-read* (regex ...))` + `(deny file-write* (regex ...))` + 祖先目录 `deny file-write-unlink`。每个可写根加 `(deny file-write-unlink (require-all (literal ROOT) (vnode-type DIRECTORY)))`,只读子路径的祖先目录(到可写根为止)同样加 unlink 拒绝,放在策略最后。写根与排除路径若 realpath 与逻辑路径不同(`/tmp` 与 `/private/tmp`)两种拼写都下发。

### D9. MXC 只留口子

`BackendKind::WindowsMxc`、`config.sandbox.windows_backend`(`restricted-token` 默认 / `mxc`)、`sandbox_backend_mxc_win.cpp` 的 `probe_windows_mxc()` 返回不可用并说明「本构建未捆绑 MXC」。bash 的 Windows 分支按 backend 分派;策略对象已是 MXC `readwritePaths / readonlyPaths / deniedPaths / network` 的一一映射,接入时只需实现探测与启动两个函数。

### D10. Deny 规则硬拒绝收窄

`agent_loop.cpp` 的硬拒绝循环只对 `priority >= 1000` 的内置保护规则(`.acecode/rules/**`)或 yolo 模式生效;其它 Deny 命中(`.env` / `.git/**` 写入)回到 `should_auto_allow=false` 的弹确认路径。bash 的配置 Deny 仍映射为 exec forbidden。理由:Desktop 没有 `--dangerous`,原来的硬拒绝没有逃生口。

## Risks / Trade-offs

- deny 默认名单可能拦到合法工作流(例如读 `~/.aws` 做本地开发);`deny_defaults=false` 或在 `filesystem.deny` 里不写即可,且 Windows 上不影响读。
- 准断网只是环境变量层面的「劝退」,不是隔离;文档与状态行都不说「blocked」。
- Job Object 嵌套在 Desktop 托管的 daemon Job 里,Windows 8+ 支持;失败即回退。
- 规则文件追加不做文件锁(daemon 单进程写),多 daemon 并发写同一 `~/.acecode/rules` 的概率极低,重复行无害(去重按加载后的 pattern)。
