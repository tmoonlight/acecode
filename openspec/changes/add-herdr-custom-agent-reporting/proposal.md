# 变更：新增 PermissionResolved 钩子与默认 Herdr seed

## 背景

`PermissionRequest` 会在交互授权前运行，但集成方无法得知等待何时结束。通用状态、审计与遥测钩子因此无法区分“正在等待用户”和“正在执行已授权操作”。

## 变更内容

- 新增通用的 `PermissionResolved` 命令钩子事件，并与 `PermissionRequest` 配对。
- 在其标准输入 JSON 载荷中加入授权决定和解决来源字段。
- 明确该事件只用于观测，不能更改已经完成的授权决定。
- 把跨平台 Herdr `hooks.json` 纳入版本化默认 seed，把现有生命周期事件映射为 Herdr custom-agent 状态。
- 默认应用启动时安装并加载该 hook；加载器只自动信任与官方 seed 指纹一致的内容。
- seed hook 使用独立的 ACECode 管理目录，不改写或合并用户已有的 `hooks.json`。
- 修正真实 Herdr managed pane 不提供 `HERDR_BIN_PATH` 时 Windows hook 静默跳过的问题；将该变量降为可选提示，并通过平台安装位置或 `PATH` 解析 Herdr CLI。

## 不变范围

- ACECode 不检测 Herdr 环境变量，也不直接调用 Herdr。
- 不修改 Herdr。
- 不新增 Herdr 安装器、上报类、provider 或运行时分支。
- 在 Herdr 环境外，默认 hook 成功退出且不产生外部调用。
- 用户修改过的 seed hook 不会被覆盖，也不会继续按 managed hook 自动信任。
- ACECode 不通过当前焦点猜测 pane 身份；上报目标始终来自 Herdr 注入的 `HERDR_PANE_ID`。
