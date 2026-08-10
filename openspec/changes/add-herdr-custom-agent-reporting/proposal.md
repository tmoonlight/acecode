# 变更：新增 PermissionResolved 钩子与 Herdr 示例

## 背景

`PermissionRequest` 会在交互授权前运行，但集成方无法得知等待何时结束。通用状态、审计与遥测钩子因此无法区分“正在等待用户”和“正在执行已授权操作”。

## 变更内容

- 新增通用的 `PermissionResolved` 命令钩子事件，并与 `PermissionRequest` 配对。
- 在其标准输入 JSON 载荷中加入授权决定和解决来源字段。
- 明确该事件只用于观测，不能更改已经完成的授权决定。
- 提供可选的跨平台 Herdr `hooks.json` 示例，把现有生命周期事件映射为 Herdr custom-agent 状态。

## 不变范围

- ACECode 不检测 Herdr 环境变量，也不直接调用 Herdr。
- 不修改 Herdr。
- 不新增 Herdr 安装器、上报类、provider 或运行时分支。
- 未安装示例 JSON 的用户不会获得任何 Herdr 专属行为。
