# 设计：通用授权解决生命周期

## 核心事件

ACECode 派发 `PermissionRequest` 时，记录一条已开启的授权生命周期。决定确定后，在工具开始执行或拒绝结果返回模型之前，恰好派发一次 `PermissionResolved`。

载荷复用公共字段与工具字段，并新增：

- `permission_decision`：`allow`、`always_allow` 或 `deny`
- `permission_source`：`hook`、`interactive`、`headless` 或 `implicit`

该事件只用于观测。其输出会正常派发，但不能覆盖已经完成的授权决定。

## 配对与配置识别

只有先派发过请求事件，才派发解决事件。实现需要在 hook、headless 与 interactive 分支之间防止重复解决。

`PermissionResolved` 同时属于配置解析器的已知 Codex 事件。无论配置使用 `hooks` 包装对象，还是只含该事件的裸事件对象，都必须按 Codex hook 格式识别。

## 默认 Herdr hook seed

独立的 Codex 格式 hook JSON 通过 Herdr 已公开的 CLI 上报，生命周期来源为 `custom:acecode`。配置同时包含 POSIX `command` 和 Windows `commandWindows` handler，在 Herdr 外部运行时成功退出且不产生调用。

该 JSON 作为第三类版本化默认 seed 资源，与 Skill 和 Expert 一起完成启动期 reconcile：

- 官方资源位于 `assets/seed/hooks/agent-reporting/hooks.json`。
- 用户侧安装到 `~/.acecode/hooks/agent-reporting/hooks.json`，不触碰 `~/.acecode/hooks.json` 或 `~/.codex/hooks.json`。
- 首次安装、版本升级、并发锁、暂存发布、失败恢复与用户改动保护沿用现有 seed 事务。
- 重复启动在 seed 版本相同时不重复安装或注册。

hook registry 把内容与内置官方定义指纹一致的 seed source 标记为 `ManagedTrusted`。若文件被修改、损坏或指纹不匹配，则保留文件但拒绝 managed 自动信任并给出诊断。这样默认应用可直接启用官方 hook，又不会把用户可写路径中的任意命令当成受信任配置。

状态映射如下：

- `SessionStart` -> metadata 与 `idle`
- `UserPromptSubmit` -> `working`
- `PermissionRequest` -> `blocked`
- `PermissionResolved` -> `working`
- `AskUserQuestion` 的 pre/post -> `blocked` / `working`
- `Stop` -> `idle`

命令同步执行且超时为一秒，因为 ACECode 当前会跳过异步 Codex 命令钩子。失败输出会被重定向并通过条件守卫处理，因此缺少 Herdr 时不会产生副作用。

### Herdr CLI 与 pane 身份解析

普通 Herdr managed pane 的必需契约是 `HERDR_ENV=1`、`HERDR_PANE_ID` 与 `HERDR_SOCKET_PATH`。`HERDR_BIN_PATH` 只作为可选的 CLI 绝对路径提示，不能作为是否处于 Herdr pane 的判断条件。

CLI 解析顺序如下：

- POSIX：优先 `HERDR_BIN_PATH`，缺少时通过 `command -v herdr` 查找。
- Windows：优先 `HERDR_BIN_PATH`，其次使用官方安装位置 `%LOCALAPPDATA%\Programs\Herdr\bin\herdr.exe`，最后从 `PATH` 查找 `herdr`。
- 所有候选均不存在时，hook 成功退出且不产生上报。

hook 只向 `HERDR_PANE_ID` 指定的 pane 上报，不使用 UI 当前焦点作为回退。Herdr 必须保证注入的 pane ID 对应承载 ACECode 的终端；若 Herdr 注入了旧 ID 或其他 pane ID，应由 Herdr 修复或在重建 pane 后重新注入，ACECode 不应猜测替换。

Windows 实机验证必须覆盖 `HERDR_BIN_PATH` 缺失的普通 pane 环境，并确认日志中的成功退出确实对应 Herdr agent 列表发生变化，而不是 guard 静默短路。

### Windows shell 命令行边界

Windows hook runner 通过 `cmd.exe /d /s /c` 执行 `commandWindows`。`/c` 后的正文属于 `cmd.exe` 语法，不能再套用普通 C argv 的反斜杠引号转义；否则正文中的 `"..."` 会把反斜杠原样传给子命令。runner 必须只为 `cmd.exe` 路径使用普通 argv 引号，并用 `/s` 的首尾引号包住未经 C argv 转义的命令正文。

Herdr 回归测试必须校验 fake CLI 收到的 pane ID 与 `HERDR_PANE_ID` 完全相等，不能只做子串匹配；这样 `\"w1:p1\"` 一类表面包含正确 ID、实际无法寻址 pane 的参数会直接失败。
