# 设计：会话标题 hook 与 Herdr tab 同步

## Context

ACECode 的标题写入分散在 TUI `/title`、TUI/CLI resume、daemon registry 自动标题和 Web 标题 API 等入口。`SessionManager` 负责持久化标题，但不持有 `HookManager`；`AgentLoop` 已经是通用 Codex hook 的派发边界，并同时存在于 TUI 与 daemon session 中。

Codex command hook 通过 stdin 接收 JSON。默认 Herdr seed 需要把标题作为一个不受 shell 注入影响的 CLI 参数传给 `herdr tab rename`。为避免在 POSIX 依赖 `jq`/Python，也避免 Windows `cmd.exe` 对标题中的引号或元字符二次解释，runner 需要为这个事件提供等价的进程环境变量。

## Goals / Non-Goals

**Goals:**

- 对成功的用户标题修改、标题清除、自动标题生成和会话恢复派发统一事件。
- 让事件载荷同时表达当前标题、触发来源与持久化标题来源。
- 让默认 Herdr seed 使用精确的 `HERDR_TAB_ID` 安全更新 tab 名称。
- 保持标题 hook 为观测型能力；hook 失败或输出不能回滚标题。
- 在 Herdr 环境或 CLI 缺失时保持静默降级。

**Non-Goals:**

- 不把 Herdr reporter 或 Herdr 环境检测写进 session 标题核心逻辑。
- 不为未激活、仅在磁盘上修改的历史会话派发事件。
- 不让无参数 `/title` 查询产生标题变化事件。
- 不用空标题清空或猜测 Herdr tab 的默认名称。

## Decisions

### 1. 由 AgentLoop 统一派发观测事件

新增 `AgentLoop::dispatch_session_title_changed_hook(...)`，复用公共 hook 字段并调用 `HookManager::dispatch_codex`。调用方只在标题状态已经成功提交后调用它。

相比让 `SessionManager` 依赖 `HookManager`，该方案保持存储层可独立测试，也避免持久化锁内启动外部进程。相比每个 UI 直接操作 Herdr，它保留通用 hook 扩展点。

### 2. 载荷区分 trigger source 与 title source

载荷新增：

- `title`：当前完整标题，可为空。
- `source`：本次触发来源，取值 `user`、`generated` 或 `resume`。
- `title_source`：当前持久化所有权，例如 `user`、`user-cleared`、`generated`、`legacy`。

`matcher` 使用 `source`，使用户 hook 可以只订阅恢复或生成事件。

### 3. resume 表示会话标题状态切换

成功的 `/resume`、`acecode --resume` 与 daemon resume 都派发一次，即使恢复出的标题为空。失败的 resume 不派发。这样外部集成能够区分“切到无标题会话”和“没有事件”。

### 4. 事件字段通过子进程环境安全暴露给默认 seed

`HookManager` 在派发 `SessionTitleChanged` command hook 时，除 stdin JSON 外，还向该子进程注入 `ACECODE_HOOK_SESSION_TITLE`。环境只作用于该 child process，不修改 ACECode 进程全局环境。

POSIX runner 通过 `/usr/bin/env NAME=VALUE <shell> -c ...` 传递覆盖值；Windows runner 构造继承环境块并追加/替换变量，再用 `CREATE_UNICODE_ENVIRONMENT` 启动。这样标题不会被拼接进 shell 命令正文。

相比要求 seed 安装 `jq`、Python 或 PowerShell JSON parser，该方案跨平台依赖更少；相比字符串模板替换，它不会把用户标题当成 shell 语法。

### 5. Herdr seed 只同步非空标题

seed 在 Herdr 必需环境、`HERDR_TAB_ID`、CLI 和非空标题同时有效时执行：

```text
herdr tab rename <HERDR_TAB_ID> <ACECODE_HOOK_SESSION_TITLE>
```

空标题仍向其他 hook 完整派发，但 seed 不重命名 tab，避免猜测应恢复的 Herdr 默认标签。调用始终使用 pane 继承的 `HERDR_TAB_ID`，不使用 UI 当前焦点。

### 6. 已知历史官方定义可修复 seed 状态漂移

部分已安装用户的 `seed.version` 已经前进，但 `.seed_skills_state.json` 因早期官方 hook 更新未同步记录而把磁盘上的官方定义标成 `preserved_user_modified`。后续 reconcile 因而无法升级，即使 `hooks.json` 与已发布官方定义完全一致。

`DefaultHookSeed` 记录当前与已知历史官方定义的规范化 SHA-256。reconcile 仅在 hook 目录只含 `hooks.json`，且解析后的定义精确匹配其中一个官方指纹时恢复 ACECode ownership 并升级。任何命令、matcher、事件或额外文件变化都继续按用户修改保留。新 bundle 必须再次提升版本，确保已经错误写入上一版本戳的用户会重新进入 reconcile。

## Risks / Trade-offs

- [同一 tab 中多个 ACECode pane 可能依次更新同一 tab] -> 每次严格使用所属 `HERDR_TAB_ID`；最终标题遵循最近一次真实标题事件，不猜测 pane 主从关系。
- [用户手动重命名后，后续 ACECode 标题变化会再次覆盖] -> 这是启用自动同步的明确语义；用户可修改/禁用 managed hook 副本，seed reconcile 会保留其修改且撤销 managed trust。
- [同步 command hook 增加标题操作延迟] -> seed 保持一秒超时并吞掉 Herdr 错误；自动标题在后台线程派发。
- [环境变量是 stdin JSON 之外的附加接口] -> 仅为标题字段提供、文档化并保持 payload 为权威数据源。

## Migration Plan

提升 seed 版本并更新 manifest/官方指纹，同时登记已发布旧 Herdr 定义的指纹。新包启动时，未修改的旧 managed seed（包括状态漂移但仍精确匹配历史官方定义的副本）自动升级；用户修改过的副本继续保留。回滚时旧二进制会忽略未知 `SessionTitleChanged`，且 seed 版本协调逻辑继续保护用户文件。

## Open Questions

无。
