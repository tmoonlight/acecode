<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design: add-auto-mode-sandbox

## Context

acecode 现有的执行安全全部落在 `AgentLoop` 的权限门上(`src/agent_loop.cpp` 写工具分支 → `PermissionManager::should_auto_allow` → prompt),特点是:

- 判定粒度是**工具名**。bash 永远 `is_read_only=false`,`git status` 与 `rm -rf` 在权限门眼里没有区别;「总是允许」记的也是工具名,点一次等于本会话所有 bash 放行。
- 文件工具有 `PathValidator` 的 cwd 边界与危险路径判定,bash 没有任何边界(`agent_loop.cpp` 显式跳过 bash 的路径校验),模型还能随便传 `cwd`。
- `agent_loop_shell_guard.hpp` 的「可证明写目标」守卫是字符串启发式,只在 LOOP Yolo 与 write boundary 下生效,注释里自己写明「不是进程沙盒」。
- 子进程启动:Windows `cmd.exe /c` + `CreateProcessW`,POSIX `/bin/sh -c`(或环境配置的终端)。仓库里没有受限令牌、Job Object、bwrap、sandbox-exec 的任何代码。

Codex 的对应实现(2026-09 main 分支,以下均为拉取原文核对过的事实):

- 预设 Auto = `--sandbox workspace-write --ask-for-approval on-request`;Read Only = `read-only` + `on-request`;Full Access = `danger-full-access` + `never`。
- `exec_policy.rs::render_decision_for_unmatched_command_for_platform`:命中危险命令 → `Never` 时 Forbidden、否则 Prompt;未命中时 `Never` → Allow,`UnlessTrusted` → Prompt,`OnRequest` → 文件系统沙盒 Unrestricted/External 时 Allow,Restricted 时「模型请求越过沙盒」则 Prompt 否则 Allow。**Windows 上沙盒后端 Disabled 但 profile 带文件系统限制时,未命中命令一律 Prompt**。
- 所有命令段都命中 execpolicy `allow` 规则时 `bypass_sandbox=true`(在沙盒外跑)。
- `.rules` 语法:Starlark `prefix_rule(pattern, decision, justification, match, not_match)`;多规则命中取最严格;脚本按 `&&` / `||` / `;` / `|` 拆段,含重定向 / 替换 / 变量 / 通配符 / 控制流则整条不拆。
- `denial.rs::is_likely_sandbox_denied`:exit 0 不算;exit 2/126/127 不算;seccomp 下 128+SIGSYS 算;否则在 stdout/stderr 里大小写不敏感找 `operation not permitted` / `permission denied` / `read-only file system` / `seccomp` / `sandbox` / `landlock` / `failed to write file`。
- Windows 沙盒(unelevated):`CreateRestrictedToken` 带 `WRITE_RESTRICTED`,限制 SID 列表 = {Everyone, 登录会话 SID, 合成 SID};在工作区与 `%TEMP%` 上打合成 SID 的写 ACE,`.git` / `.codex` / `.agents` 打拒绝写 ACE;不断网(防火墙规则需要管理员);Everyone 可写目录是已知绕过面。elevated 档另建 `CodexSandboxOffline/Online` 两个账户 + 防火墙规则,经过 UAC 的 helper 启动 —— 本期不做。
- macOS:`/usr/bin/sandbox-exec -p <policy> -DWRITABLE_ROOT_0=... -- <cmd>`,policy = `seatbelt_base_policy.sbpl`(deny default + 一串 sysctl/mach-lookup 放行)+ `(allow file-read*)` + 每个可写根 `(allow file-write* (subpath (param "WRITABLE_ROOT_i")))` 配 `(require-not (subpath (param "..._EXCLUDED_j")))` + 有网络时追加 `seatbelt_network_policy.sbpl`。
- Linux:`bwrap --ro-bind / / --unshare-net ...`,启动前用 `bwrap --unshare-user --unshare-net --ro-bind / / /bin/true` 探测用户命名空间。

## Goals / Non-Goals

**Goals**

- `auto` 模式下:读工具、工作区内文件编辑、已知安全命令、沙盒内的未知命令全部不弹确认;危险命令、模型显式申请越权、沙盒不可用时的未知命令弹确认;越界写与受后端限制的网络访问在沙盒内直接失败并引导模型升级申请。
- 三平台沙盒后端共用同一份策略模型(`SandboxPolicy` / `WritableRoot`),不可用时的降级路径与 Codex 的 Windows-disabled 分支一致。
- 命令分类器与规则文件是纯函数,单测在三平台跑;规则文件与 Codex `.rules` 互通(子集)。
- 老配置 / 老会话 meta / 老 CLI 参数里的 `accept-edits` 无损迁移到 `auto`。
- 不打穿 prompt cache:system prompt 里的沙盒行只随模式 / 配置 / 探测结果变化,且探测结果在回合开始前就固定。

**Non-Goals**

- 见 proposal 的 Non-Goals。另:本期不做「approve and remember to rules file」的持久化(会话级前缀记忆即可);不做 Job Object 收尾(令牌继承已让孙进程同样受限,而 kill-on-close 会杀掉命令有意留下的后台进程,与现有语义冲突)。

## Decisions

### D1. 模式改名而不是加一档

`AcceptEdits` 直接改成 `Auto`,不保留一个独立的 `accept-edits` 档。理由:两档的差别只在"bash 是否自动",而 `auto` 的 bash 自动是有条件的(安全命令 / 沙盒内),`accept-edits` 用户想要的本来就是这个;并存只会让 Ctrl+P 循环多一档没人选的东西。别名解析集中在 `PermissionManager::parse_mode_name`(新增静态函数),所有原来各自手写 `if (mode == "accept-edits")` 的地方(config / session_storage / session_manager / session_registry / headless / worker / tui_init / builtin_commands / permission_mode_handler)统一调用它,别名只在这一处维护。

### D2. 分类器输出三态 + 拆段,而不是布尔"安全"

```cpp
enum class CommandKind { KnownSafe, Dangerous, Unknown };
struct CommandSegment { std::vector<std::string> tokens; };
struct CommandClassification {
    CommandKind kind;
    std::vector<CommandSegment> segments;   // 尽力拆段，不透明段仅供更严格规则匹配
    bool split_safely;                      // 是否按 Codex 规则安全拆段
    std::string reason;                     // 给 prompt / 日志的一句话
};
CommandClassification classify_command(const std::string& command,
                                       CommandPlatform platform = host_platform());
```

- **拆段规则**照抄 Codex 文档:只有脚本里没有 `>` `<` `$` `` ` `` `%...%` `*` `?` `[` `{` `&`(单个)/ 换行 / 控制流关键字(`if for while until case do done then fi function select`)时,才按 `&&` `||` `;` `|` 拆;否则 split_safely=false，不接受前缀 allow。PowerShell 的 `{` 判为不透明区域,与 Codex「无法完整检查的 AST 区域必须审批」同义。
- **解包**:`bash|sh|zsh|dash -c/-lc <script>`、`cmd(.exe) [/d] [/s] /c <script>`、`powershell|pwsh(.exe) [-NoProfile ...] -Command|-c <script>`、`sudo [opts] <cmd>`、`env [VAR=v...] <cmd>`、`nice`/`time` 前缀。深度上限 8。`-EncodedCommand|-enc|-e` 直接 Dangerous。
- **Dangerous 判定在拆段之前对整条原文跑一遍**(也对每段跑):危险模式不需要拆段成功才能命中,含重定向的 `rm -rf x > log` 也要抓到。
- **KnownSafe 要求每一段都在安全名单里**,且任一段的 token 引用敏感路径(`PathValidator` 的 dangerous files / directories 名单)即降级 Unknown；Default/Plan 会确认，Auto 仍按未知命令的沙盒边界执行。
- 名单是三平台合一的:`ls` 在 PowerShell 里是别名、在 bash 里是 coreutils,cmd 未必提供该命令;`del` / `rd` 是 cmd 内建;`Remove-Item` 是 PowerShell。按 token 名判,不按当前 shell 判 —— 模型在 Windows 上照样会写 POSIX 命令交给 cmd,cmd 报错也只是失败一次。

### D3. 规则文件:Codex `.rules` 的 `prefix_rule` 子集,手写解析器

不引入 Starlark 解释器。解析器只接受:注释 `#`、`prefix_rule(` 关键字参数 `)`、字符串(双/单引号、`\\` `\"` `\n` 转义)、字符串列表、列表里的列表(并集)、`decision` / `justification` / `match` / `not_match`。`host_executable(...)` 整条跳过。其它任何语法 → 该文件整体跳过 + LOG_WARN(不阻塞启动,不部分生效)。`match` / `not_match` 在加载时验证,失败同样整文件跳过 —— 与 Codex 的加载期校验语义一致。

加载位置:`<data_dir>/rules/*.rules`(全局,`RuleScope::Global`)、`<cwd>/.acecode/rules/*.rules`(项目,`RuleScope::Project`)。匹配:命令段的前 N 个 token 逐个等于 pattern(第 i 项是列表时任一相等);首 token 另做 basename 回退(`/usr/bin/git` / `C:\...\git.exe` → `git`,大小写不敏感)。多规则命中取最严格;多段命令逐段评估,整体取最严格;只有**每段都命中 allow** 才算 allow。

**项目作用域的 allow 降级为 `AllowSandboxed`**:免确认但仍在沙盒内跑。Codex 靠"项目层是否可信"门控项目规则;acecode 没有 trusted-project 概念,把"能否绕过沙盒"作为全局 / 项目的分水岭是最小的等价物。配套:`file_write` / `file_edit` 新增对 `.acecode/rules/**` 的内置 Deny 规则,沙盒对 `.acecode/rules` 打拒绝写 ACE —— 文件工具直接拒绝改规则，Shell 拒绝规则内容写入；Windows 删除/改名仍有下述已接受限制。

### D4. 决策表是纯函数,输入全部显式

```cpp
enum class ExecVerdict { Allow, Prompt, Forbidden };
enum class SandboxMode { FullAccess, WorkspaceWrite, ReadOnly };
struct ExecDecisionInput {
    PermissionMode mode; bool dangerous_mode;
    CommandKind kind;
    RuleDecision rule;           // NoMatch / Allow / AllowSandboxed / Prompt / Forbidden
    bool sandbox_available;
    bool escalation_requested;
    SessionAllowKind session_allow;   // None / Sandboxed / Bypass
};
struct ExecDecision {
    ExecVerdict verdict; SandboxMode sandbox;
    std::string reason;   // dangerous_command / escalation_requested / rule_prompt /
                          // rule_forbidden / unknown_command_without_sandbox /
                          // default_mode / plan_mode / known_safe / rule_allow / ...
};
ExecDecision decide_exec(const ExecDecisionInput&);
```

优先级(从上到下,先命中先返回):

| # | 条件 | 结果 |
|---|---|---|
| 1 | rule = Forbidden | Forbidden(任何模式,含 yolo / --dangerous) |
| 2 | dangerous_mode 或 mode = yolo | Allow, FullAccess |
| 3 | rule = Prompt | Prompt(rule_prompt);显式升级批准后 FullAccess,否则 auto → WorkspaceWrite(可用时),其它 → FullAccess |
| 4 | escalation_requested 且没有 Bypass 前缀记忆 | Prompt(escalation_requested);批准后 FullAccess |
| 5 | session_allow = Bypass 且不是 Dangerous | Allow, FullAccess |
| 6 | rule = Allow(全局) | plan:ReadOnly(可用)否则 Prompt;其它:Allow, FullAccess |
| 7 | kind = Dangerous | Prompt(dangerous_command);显式升级批准后 FullAccess,否则按模式审批边界执行 |
| 8 | rule = AllowSandboxed 或 session_allow = Sandboxed | 沙盒可用:Allow + 模式沙盒(auto/default → WorkspaceWrite,plan → ReadOnly);不可用:一律 Prompt(unknown_command_without_sandbox),不能把已有沙盒批准升级 |
| 9 | kind = KnownSafe | Allow;auto → WorkspaceWrite(可用)否则 FullAccess;default/plan → ReadOnly(可用)否则 FullAccess |
| 10 | kind = Unknown, mode = auto | 沙盒可用:Allow WorkspaceWrite;否则 Prompt(unknown_command_without_sandbox),批准后 FullAccess |
| 11 | kind = Unknown, mode = default / plan | Prompt(default_mode / plan_mode);批准后 FullAccess |

「批准后的沙盒」= 用户在 prompt 上点允许后实际执行用的策略,和 verdict 一起放在 `ExecDecision::sandbox` 里,AgentLoop 不再二次推导。

### D5. 升级协议照 Codex:参数在工具上,提示在结果里

bash 新增 `with_escalated_permissions: boolean` 与 `justification: string`。决策表把 `escalation_requested` 当作最高优先级的 Prompt 来源(仅次于 forbidden / yolo),批准后 FullAccess。沙盒内失败时,`sandbox::is_likely_sandbox_denied(exit_code, output)` 为真则在工具输出末尾追加固定提示(告诉模型可写根与网络状态,并指明用 `with_escalated_permissions=true` + 一句 justification 重试),`metadata.sandbox_denied=true`。模型不带 justification 就申请升级 → 工具直接返回参数错误,不弹 prompt。

### D6. 会话级「总是允许」对 bash 记前缀

`PermissionManager` 新增 add_session_command_allow(prefix, bypass) / session_command_allow(segments)。
前缀保留原始可执行 token；多级 CLI 取子命令，第二项是全局选项时不记忆。
解释器、通用启动器和不透明脚本不提供前缀；多段命令逐段记忆。
bypass 仅来自显式升级批准。旧的整个 bash 工具会话授权通道不再接受授权。
新危险参数仍需确认；切换模式、cwd、沙盒配置或开关清除会话授权。

### D7. Windows 免管理员令牌与 ACL

- 从当前用户派生 WRITE_RESTRICTED | DISABLE_MAX_PRIVILEGE | LUA_TOKEN 令牌；
  限制 SID 含合成 SID、登录 SID、Everyone。设置新令牌默认对象 DACL 与启动桌面，
  使 cmd/PowerShell 初始化和 IPC 可用，不修改宿主令牌。
- 合成 SID 绑定模式、规范化可写根和只读子路径集合。ReadOnly 无写根且使用独立身份；
  不同工作区策略不得共享授权。可写根授予读写/执行/DELETE，不授予 FILE_DELETE_CHILD。
- 敏感子路径仅拒绝写类权限，不拒绝 READ_CONTROL/SYNCHRONIZE。
  不存在的 rules/hooks/modules 目录及配置文件先准备实体再保护。
- 每次执行核对实际 DACL，所需 ACE 存在时不重写，不依赖失效的路径缓存。
  准备/启动失败返回错误或重新进入审批，不在工具中完整访问重试。
- Windows 默认临时写根使用系统临时目录下的 `acecode-sandbox/<规范化工作区哈希>`，
  并覆盖子进程 TEMP/TMP/TMPDIR。路径稳定，避免重开会话产生新的策略 SID；不再
  向整棵系统临时树传播 ACL。准备阶段创建专用目录并拒绝其被 junction/symlink 重定向。
  exclude_tmpdir 禁用此追加根及环境覆盖；其他平台保留原系统临时目录语义。
- linked worktree 追加写根前验证 gitdir/commondir 布局和反向登记；
  重叠写根必须继承只读排除项。
- 2026-09-13 对照 Codex dfaf451426868c22e6859f5494150fd6338c3257 的 token.rs、acl.rs、
  spawn_prep.rs，在私有测试目录复现其完整令牌/ACL 配置。内容写被拒绝，但 DeleteFileW、
  MoveFileW 和 cmd 删除受保护文件仍成功；这是免管理员后端的已接受限制。
  不为修复它而收紧 Shell 外部读取；/sandbox 和用户文档明确说明，不声称完整文件系统隔离。
- Windows 不隔离网络，不设置 ACECODE_SANDBOX_NETWORK_DISABLED。
  Everyone/登录 SID 已有写权限的公开目录仍是已知限制，不更改用户的公开 ACL。

### D8. POSIX 后端

macOS 使用系统 sandbox-exec：deny-default、file-read*、参数化写根与只读排除项，
network_access=true 才追加网络权限。Linux 使用系统 bwrap：根 ro-bind，
写根 bind，保护路径最后 ro-bind；使用 --unshare-user / --unshare-pid / --unshare-ipc / --new-session，network_access=false 使用 --unshare-net。
不自动下载后端。Linux 探测复用实际执行的命名空间/挂载参数，限时 3 秒并保存绝对程序路径。
父进程预先准备环境/argv/程序路径，fork 后只调用 execve 等异步信号安全 API。
沙盒执行失败不得隐式完整访问重试。

### D8a. 分类与会话前缀的保守边界

词法按实际 shell 区分引号；cmd 单引号不能隐藏操作符。
畸形语法、转义、变量/命令替换和控制流关闭前缀 allow。
尽力解析的段及包装器内层段仅用于更严格的 forbidden/prompt；
内层不透明标记必须传播到外层。安全名单排除可写参数、执行预处理器/分页器、
任意程序的 --version 和路径限定的同名程序。cargo check/build 按未知命令处理。

会话前缀保留原始可执行 token，不能把 ./git 的批准当成 git。
解释器、通用启动器、不透明脚本和全局选项开头的多级 CLI 不提供前缀记忆。
新危险参数优先于项目 allow 与会话记忆；沙盒批准不得随失效而自动升级。

### D9. 沙盒策略与可用性通过 ToolContext 注入,决策在 AgentLoop

`ToolContext` 新增 `std::optional<sandbox::ExecSandboxRequest> exec_sandbox`(`{SandboxMode mode; SandboxPolicy policy; std::string backend;}`)。AgentLoop 在写工具分支里为 bash 计算 `ExecDecision`,prompt(如需)通过后复制一份 `ToolContext`、填上本次的 `exec_sandbox`,再进 `execute_single_tool`。bash_tool 只按 `exec_sandbox` 行动,不知道模式 / 规则的存在;独立调用 ToolExecutor(旧路径 / 单测)时 `exec_sandbox` 为空 = 不沙盒,行为与今天一致。

每个 AgentLoop 持有 SandboxRuntime 配置、失效状态和会话开关；OS 探测结果进程级缓存。AgentLoop 按回合缓存系统提示中的沙盒状态，保证同一回合逐字节稳定。`/sandbox off` 只翻 AgentLoop 上的会话级开关。

### D10. 提示原因随 prompt 一起下发,前端 / TUI 只做展示

权限 prompt 的 args JSON 对 bash 扩成:

```json
{"command": "...", "cwd": "...", "with_escalated_permissions": true, "justification": "...",
 "permission": {"reason": "escalation_requested", "sandbox": "full-access",
                "always_allow_prefix": "git commit", "classification": "unknown"}}
```

Web `permissionRequestPresentation.js` 按 `permission.reason` 生成标题 / 正文(危险命令 → 「模型要执行一条危险命令」;升级申请 → 「模型申请在沙盒外执行」+ justification;无沙盒 → 「本平台没有可用沙盒,未知命令需要确认」),「本次会话允许」按钮带前缀。TUI 确认框在参数区上方多一行同样的原因文案。

## Risks / Trade-offs

- **首次 ACE 传播耗时**:大仓库上 `SetNamedSecurityInfoW` 可能十几秒。缓解:ACE 幂等检查 + 日志;后续可加"后台预热"。
- **Everyone 可写目录是 Windows unelevated 档的已知绕过**(`C:\Users\Public`、部分 `ProgramData` 子目录):与 Codex 相同,写进文档,不在本期解决。
- **不断网**:Windows 本期沙盒不拦网络;`network_access=false` 只在 macOS / Linux 生效,system prompt 的沙盒行如实写「network: not enforced」。
- **规则 `allow` 的项目作用域降级**是对 Codex 的有意偏离:Codex 有 trusted-project 层,acecode 没有;偏离方向是更保守。
- **分类器名单是有界的**:名单外的只读命令在 auto 下走沙盒(无感),在 default 下弹确认(与今天一致);漏判的危险命令在 auto 下仍在沙盒里跑,内容写入受到后端约束，但 Windows 删除/改名和公开可写目录仍有已接受限制。名单变更只改一处常量表并加用例。
- **`git commit` 走沙盒内**:`.git` 整体可写、只锁 `hooks` / `config` / `modules`,这是为支持提交而保留的策略选择。内容保护降低改写 hooks/config 的风险，但 Windows 删除/改名限制仍可能绕过路径保护，不能宣称完全封闭该攻击面。
