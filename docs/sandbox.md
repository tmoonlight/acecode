# Auto 模式与 Shell 沙盒

Auto 自动接受文件编辑，并让普通模型命令在工作区写入沙盒中执行。危险命令仍需确认。
无法启用沙盒时，未知命令回到确认流程。这个功能约束模型调用的 `bash` 工具；
用户手动执行的 Shell、插件和其它工具继续使用各自的权限边界。

## 模式与审批

| 条件 | 执行行为 |
|---|---|
| 任意模式匹配 forbidden 规则 | 直接拒绝，包括 yolo / dangerous |
| Yolo / dangerous | 完整访问；其它既有文件路径规则仍按原流程处理 |
| 无人值守（active goal）下申请沙盒外执行或额外权限 | 直接拒绝，工具结果引导模型去掉参数留在沙盒里重试；普通确认仍自动放行 |
| 显式申请沙盒外执行（`require_escalated`） | 展示理由，批准后完整访问 |
| 申请额外权限（`with_additional_permissions`） | 展示申请的路径 / 网络，批准后仍在 workspace-write 沙盒内执行，额外条目并入策略 |
| Auto：已知安全或普通未知命令，沙盒可用 | 工作区写入沙盒，免确认 |
| Auto：未知命令，沙盒不可用或会话关闭 | 确认后完整访问 |
| Default / Plan：已知安全命令 | 优先只读沙盒；不可用时直接运行 |
| Default / Plan：未知命令 | 确认后完整访问 |
| 危险命令 | 确认；Auto 沙盒可用时仍在沙盒内，显式升级批准后才完整访问 |
| 项目 allow 规则 / `default.sandboxed.rules` | 仅允许沙盒内运行；不越过危险命令确认 |
| 全局 allow 规则 / `default.rules` | 完整访问；Plan 优先只读沙盒，不可用时确认 |

已知安全名单只覆盖可静态判断的命令和参数，如 `git status`。
重定向、变量替换、脚本块、解释器脚本及可能写文件或启动其它程序的参数会保守处理。
分类器不能证明任意脚本安全，规则也不等于恶意代码扫描。

确认框的选项（Web 与 TUI 一致）：

| 选项 | 决策值 | 效果 |
|---|---|---|
| 允许一次 | `allow` | 只执行这一条 |
| 本次会话允许 `<前缀>` | `allow_session` | 记住界面显示的命令前缀（如 `pnpm install`），不会放行整个 Bash 工具；额外权限申请记住的是那些权限而不是前缀 |
| 只放行写入 `<目录>` | `allow_scoped` | 仅在上一次沙盒拒绝抽到路径时出现：把该目录加进会话授权，命令留在沙盒里重跑 |
| 以后都允许 `<前缀>` | `allow_remember` | 把前缀写进全局规则文件；解释器、`rm`、`sudo`、`git` 单独等禁用前缀不提供此项 |
| 拒绝 | `deny` | 不执行 |

升级批准所记住的前缀可在沙盒外执行；普通沙盒内批准不会在沙盒失效后自动升级。
新出现的危险参数仍需确认。切换权限模式、工作目录或沙盒开关会清除会话记忆与会话授权。

旧模式名称 `accept-edits`、`acceptEdits` 接受为 `auto`；写盘与 API 使用 `auto`。

配置里的普通 Deny 规则（例如 `.env` / `.git/**` 的文件写入）对文件工具回到「弹确认」；
只有内置保护规则（`.acecode/rules/**`）和 yolo 模式保持硬拒绝。bash 的配置 Deny 仍是
forbidden。这样没有 `--dangerous` 的 Desktop 也有逃生口。

## 权限清单

策略由三类条目派生（对齐 Codex 的 `FileSystemSandboxEntry`）：

| 条目 | 含义 |
|---|---|
| `read` | 可读根。为空 = 全盘可读（默认）；非空时只放行这些根、可写根与工作区根 |
| `write` | 可写根，与旧的 `writable_roots` 合并 |
| `deny` | 读写都拒绝（含子树），允许 glob（`**/.env`） |

判定「最长前缀命中优先，同深度 deny > write > read」。条目可用记号：`~`（家目录）、
`:workspace_roots[/子路径]`（写边界根与 linked git 目录）、`:tmpdir`（专用临时根）、
`:acecode_home`（数据目录）。未知记号跳过并记日志。

默认 deny 名单（`deny_defaults=true`）：`~/.ssh`、`~/.aws`、`~/.gnupg`、`~/.netrc`、
`~/.docker/config.json`、`~/.kube`、数据目录里的 `config.json`。落在可写根之下的 deny 路径
同时列为只读子路径，三平台的写拒绝一致；读拒绝只在 macOS / Linux 生效（见平台后端）。

## 可写范围

工作区写入沙盒允许会话写边界根（worktree / LOOP / 继承根，否则 cwd）、显式配置的
绝对路径、临时写目录，以及用户批准的会话授权路径。合法 Git linked worktree 会追加登记的
gitdir 和 common .git 目录，以支持普通 Git 操作；必须通过 commondir 布局与反向 gitdir
登记验证，任意 `gitdir:` 文本不能扩大权限。

`.git/hooks`、`.git/config`、`.git/config.worktree`、`.git/modules`、
已有 worktree 的 `config.worktree`、工作区的 `.git` 指针文件和
`.acecode/rules` 列为只读子路径。准备阶段会创建缺失的空敏感目录/配置文件，让随后新建
文件也受到保护；deny 名单里不存在的路径不会被创建。Windows 的内容写入限制不等于删除/改名隔离，
具体边界见下文。用户可在 ACECode 之外编辑这些规则和配置。

只读模式不添加可写根。Windows 默认临时写根是系统临时目录下的 `acecode-sandbox/<工作区哈希>`，
Shell 的 `TEMP`、`TMP`、`TMPDIR` 自动指向这里。目录按规范化工作区稳定派生，重开会话后复用，
首次准备无需给整个系统临时树传播 ACL。目录被 junction/symlink 重定向时准备失败。
macOS/Linux 沿用系统临时目录；`exclude_tmpdir=true` 不追加临时写根，也不覆盖上述环境变量。
会话临时文件 `ACECODE_TMPDIR` 位于工作区的 `.acecode/tmp/session-*`。

## 平台后端

| 平台 | 实现 | 限制与不可用条件 |
|---|---|---|
| Windows | WRITE_RESTRICTED 令牌、目录 ACL、CreateProcessAsUserW、Job Object | 免管理员；不隔离网络（准断网环境见下）；deny 只拦写不拦读；不支持 ACL 的文件系统、写 ACL 或创建进程被策略拒绝时不可用 |
| Windows（`windows_backend=mxc`） | 微软 MXC（AppContainer）口子 | 本构建未捆绑，探测恒不可用并给出原因；接入后可真断网、拦删除/改名 |
| macOS | 系统 sandbox-exec / Seatbelt | 工具缺失或启动失败时不可用；network_access 决定网络放行；支持受限读、deny 路径 / glob、防改名 |
| Linux | 系统 bubblewrap | 需已安装 bwrap 且用户/PID/IPC 命名空间可用；不自动下载；deny 路径用 tmpfs / /dev/null 遮住；受限读与 glob 展开未实现 |

### Windows

合成 SID 绑定模式和完整路径策略，工作区之间、写入和只读令牌之间不共享授权。
ACL 只针对合成 SID；宿主用户的正常权限不变。授权不包含 FILE_DELETE_CHILD、WRITE_DAC、
WRITE_OWNER；敏感路径添加拒绝写入 ACE。每次使用前核对 ACL，仅缺失时写入。
这些 ACE 会保留在文件系统中（每条约 36 字节，DACL 上限 64KB）；需要清理时用 `icacls`
移除主体为 `S-1-5-80-` 开头且无法解析为账户的条目。

限制 SID 还包含 Everyone 与当前登录 SID，因此它们已有写权限的目录可能仍可写。
WRITE_RESTRICTED 不完整约束 DELETE / FILE_DELETE_CHILD：即便合成 SID 有拒绝 ACE，
仍可能删除工作区外或受保护的文件、改名受保护目录。读取不受限制，deny 名单在 Windows 上
只保证写不通。本实现不是 AppContainer、虚拟机或对抗恶意本地程序的完整隔离边界。

`network_access=false` 时子进程套用**准断网环境**：`HTTP_PROXY` / `HTTPS_PROXY` / `ALL_PROXY` /
`GIT_HTTP(S)_PROXY` 指向 `http://127.0.0.1:9`，`NO_PROXY=localhost,127.0.0.1,::1`，
`PIP_NO_INDEX=1`，`NPM_CONFIG_OFFLINE=true`，`CARGO_NET_OFFLINE=true`，`GIT_SSH_COMMAND=cmd /c exit 1`，
`GIT_ALLOW_PROTOCOLS=`，`ACECODE_SANDBOX_NETWORK_DISABLED=1`，并把 `<数据目录>/sandbox/denybin`
（`ssh` / `scp` 桩）插到 PATH 最前、PATHEXT 把 `.BAT;.CMD` 提前。这只是劝退，不是隔离：
状态显示 `best-effort offline (env)`，不承诺断网。批准的 `network.enabled=true` 申请让本次不套用。

bash 子进程以挂起方式启动后挂进 Job Object 再放行；超时或中止用 `TerminateJobObject` 杀整棵
进程树。Job 不设 KILL_ON_JOB_CLOSE，正常结束时有意留下的后台孙进程照旧存活。嵌套 Job 被拒时
退回只杀直接子进程。

后端准备/启动失败只返回错误或重新进入确认流程，绝不会静默取消沙盒重跑命令。

### macOS

Seatbelt 策略按 Codex `create_seatbelt_command_args_with_profile` 组装：全盘可读时
`(allow file-read*)` 并追加 preferences policy；配置了 `read` 条目时只放行可读根并追加
平台默认项（系统框架、`/etc`、终端设备）。deny 路径经 `-D` 参数变成
`(deny file-read* file-write*)`，deny glob 转成正则并对祖先目录加 `file-write-unlink`；
每个可写根与只读子路径的祖先目录都拒绝 unlink，改名边界目录不能把受保护子树搬走。
这些 deny 排在 allow 之后。

## 升级协议

```json
{
  "command": "pnpm install",
  "sandbox_permissions": "with_additional_permissions",
  "justification": "需要写入共享依赖缓存。",
  "additional_permissions": {
    "file_system": { "write": ["~/.cache/pnpm"] },
    "network": { "enabled": false }
  },
  "prefix_rule": ["pnpm", "install"]
}
```

`sandbox_permissions` 取 `use_default`（默认）、`with_additional_permissions`（留在沙盒里但
临时加上申请的路径 / 网络）、`require_escalated`（申请沙盒外执行）；旧参数
`with_escalated_permissions=true` 等价 `require_escalated`。非 `use_default` 必须带非空
`justification`；`with_additional_permissions` 必须带非空 `additional_permissions`，路径必须是
绝对路径或 `~` 开头；deny 名单里的路径不能通过申请变可写 —— 判定前申请路径与 deny 条目都按真实
路径规范化，经 junction / 符号链接别名（如 `C:\Users\x` 指向 `N:\Users\x`）写的路径同样被拦。`prefix_rule` 是模型建议的可记住
前缀，必须覆盖全部命令段且不在禁用名单里才被采用。

请求确认时，`args.permission` 给出原因、批准后的沙盒边界、可记忆前缀、申请的额外权限、
建议的只放行目录与上一次被拒路径（见 [daemon-api.md](daemon-api.md)）。疑似沙盒拒绝会附
`metadata.sandbox_denied=true`、`metadata.sandbox_violation{reason,path,snippet}` 与升级指引：
指引先建议只加被拒路径所在目录，再提沙盒外执行。后端准备/启动错误附
`metadata.sandbox_unavailable=true`。普通编译失败不应被当作隔离拒绝。

## 规则文件

全局位置是 ACECode 数据目录的 `rules/*.rules`（通常 `~/.acecode/rules/`），
项目位置为 `<cwd>/.acecode/rules/*.rules`。会话创建或切换 cwd 时加载。
全局规则由用户维护；项目 allow 只能给予沙盒内免确认权限。

「以后都允许」写回：沙盒外批准追加到 `default.rules`（全局 allow），沙盒内批准追加到
`default.sandboxed.rules`（加载后 allow 降级为免确认仍沙盒）。追加前去重，写后立即重载。
两个文件都是标准 `prefix_rule(pattern=[...], decision="allow")`，与 Codex 互通。

```python
prefix_rule(
    pattern = ["git", ["add", "commit"]],
    decision = "prompt",
    justification = "提交前复核",
    match = ["git commit -m fix"],
    not_match = ["git push"],
)
prefix_rule(pattern = ["curl"], decision = "forbidden")
```

支持 `prefix_rule` 的字符串/列表子集、注释，decision 默认 allow；
`host_executable(...)` 仅兼容解析，不执行。其它语法、重复参数、错误的 decision、
失败的 match/not_match 验证或超限文件会使整份文件跳过，日志记录原因。
首 token 支持显式规则的可执行文件 basename 回退，多条规则以
forbidden > prompt > allow 合并；多段命令须每段都获 allow。
不透明脚本不使用宽前缀 allow；包装脚本内能识别的 forbidden/prompt 仍参与约束。

## 配置与状态

```json
{
  "sandbox": {
    "enabled": true,
    "network_access": false,
    "writable_roots": [],
    "exclude_tmpdir": false,
    "filesystem": {
      "read": [],
      "write": [],
      "deny": ["~/.aws", "**/.env"]
    },
    "deny_defaults": true,
    "windows_backend": "restricted-token"
  }
}
```

首次运行包含一次性沙盒迁移的版本时（包括新安装），应用会将 `sandbox.enabled` 设为
`false`，并同时保存 `migrations.disable_sandbox_once: true`。此迁移只执行一次：之后在
安全中心重新开启沙盒，重启、保存其他设置及后续升级都会保留开启状态。

上面的示例展示重新开启后的配置。除 `filesystem.deny` 示例外，其余为字段默认值；
保存时默认字段可省略，迁移完成标记始终保留。迁移不修改网络策略、路径清单或权限模式。
额外根必须为绝对路径或记号。`windows_backend` 取 `restricted-token`（默认）或 `mxc`。
`/sandbox` 显示后端、原因、模式、可写/只读/可读路径、deny 名单、会话授权和网络能力。
`/sandbox off` / `/sandbox on` 只改变当前会话，不写配置；关闭后 Auto 未知命令仍需确认。
后端准备或启动失败会把本会话的沙盒标记为不可用（状态里显示原因）；修好环境后
`/sandbox on` 会重新探测，不必重启。

活跃 goal 的无人值守回合里，bash 的确认与其它写工具一样自动放行，但执行边界仍按
决策表：Auto 下危险命令留在 workspace-write 沙盒内；显式的沙盒外 / 额外权限申请直接拒绝
（没有人能批），forbidden 规则照常拒绝。

## 安全中心（Desktop / Web 设置页）

设置 > 编码 > 安全中心 是这套沙盒模型的界面（openspec add-security-center），分四页：

- **概览**：沙箱总开关（`sandbox.enabled`）、网络访问、内置默认禁止名单开关、平台探测状态
  （后端、是否可用、能否断网、能否拦读）以及审计摘要。开关即改即存。
- **文件安全**：可读白名单 / 可写白名单 / 黑名单三张清单，对应 `filesystem.read / write / deny`，
  一行一条，保存后写 `config.json` 并实时下发到 daemon 内所有活跃会话（与回合串行，进行中的
  回合结束后才换策略）。下方列出审计里最近被沙箱拦截的路径，可一键加入可写或黑名单。
  Windows 受限令牌管不了读，界面会标明可读白名单与黑名单在 Windows 上只拦写。
- **命令安全**：托管规则文件 `default.rules` / `default.sandboxed.rules` 的表格编辑（放行·沙箱外 /
  放行·沙箱内 / 询问 / 禁止 + 说明）。这两个文件本来就是「以后都允许」写的，界面整体重写它们
  （注释与 `match` / `not_match` 会丢），其它手写的 `*.rules` 只读展示。禁用前缀名单同样约束
  界面上的放行规则。保存后活跃会话重载规则；TUI 进程下次启动生效。
- **审计中心**：审批门每一次「决定已作出」的记录 —— bash 的自动放行 / 规则禁止 / 用户或 hook /
  无人值守 / headless 决策、写文件工具与其它需确认工具的决策、沙盒拒绝（含被拒路径）、
  规则写回与会话授权。只读工具的自动放行不记。按类型 / 结果 / 时间 / 关键字筛选，导出 JSONL
  或 CSV，可清空。存储在 `<data_dir>/security/audit.sqlite3`，daemon / TUI / headless 共用，
  最多保留 20000 条。

REST 契约见 [docs/daemon-api.md](daemon-api.md) 的 Security center 一节。

实现参考：[OpenAI Codex](https://github.com/openai/codex) 的 execpolicy 与 sandboxing、
[微软 MXC](https://github.com/microsoft/mxc)、
[CreateRestrictedToken](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-createrestrictedtoken)、
[Windows 文件权限](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights)。
