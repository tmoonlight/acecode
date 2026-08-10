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

## Herdr 示例

独立的 Codex 格式 hook JSON 通过 Herdr 已公开的 CLI 上报，生命周期来源为 `custom:acecode`。配置同时包含 POSIX `command` 和 Windows `commandWindows` handler，在 Herdr 外部运行时成功退出且不产生调用。

状态映射如下：

- `SessionStart` -> metadata 与 `idle`
- `UserPromptSubmit` -> `working`
- `PermissionRequest` -> `blocked`
- `PermissionResolved` -> `working`
- `AskUserQuestion` 的 pre/post -> `blocked` / `working`
- `Stop` -> `idle`

命令同步执行且超时为一秒，因为 ACECode 当前会跳过异步 Codex 命令钩子。失败输出会被重定向并通过条件守卫处理，因此缺少 Herdr 时不会产生副作用。
