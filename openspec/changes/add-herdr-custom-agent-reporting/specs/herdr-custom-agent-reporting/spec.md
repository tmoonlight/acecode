## ADDED Requirements

### Requirement: 权限请求的解决过程可观测

ACECode 在派发 `PermissionRequest` 后，MUST 在授权决定确定后恰好派发一次 `PermissionResolved`。

#### Scenario: 交互拒绝

- **WHEN** 工具需要确认且用户拒绝
- **THEN** `PermissionResolved` 以决定 `deny`、来源 `interactive` 运行
- **AND** 该事件在拒绝结果返回模型前运行

#### Scenario: 交互授权

- **WHEN** 工具需要确认且用户授权
- **THEN** `PermissionResolved` 以决定 `allow` 或 `always_allow` 运行
- **AND** 该事件在工具开始执行前运行

#### Scenario: 钩子决定

- **WHEN** `PermissionRequest` 自身允许或拒绝工具
- **THEN** `PermissionResolved` 以来源 `hook` 运行

#### Scenario: 未派发请求

- **WHEN** 策略自动允许工具且未派发 `PermissionRequest`
- **THEN** ACECode 不派发 `PermissionResolved`

### Requirement: PermissionResolved 保持通用且只用于观测

事件 MUST 公开公共 hook 字段、工具标识与输入、`permission_decision` 和 `permission_source`。hook 输出不得更改已经确定的授权决定。

#### Scenario: 仅配置解决事件

- **WHEN** Codex hook 源采用只含 `PermissionResolved` 的裸事件对象
- **THEN** ACECode 将其识别为 Codex hook 配置并加载 handler

### Requirement: Herdr 支持采用默认 hook seed

仓库 MUST 把跨平台 hook JSON 作为版本化默认 seed 安装，并通过 Herdr custom-agent CLI 上报 ACECode 生命周期状态。

#### Scenario: 默认应用首次启动

- **WHEN** 用户打开包含该 seed 的默认 ACECode 应用
- **THEN** ACECode 在首次 hook registry 加载前安装 seed hook
- **AND** 与官方定义指纹一致的 seed hook 以 `ManagedTrusted` 状态加载

#### Scenario: 用户已有 hook 配置

- **WHEN** 用户已有 `~/.acecode/hooks.json`、`~/.codex/hooks.json` 或项目 hook
- **THEN** seed reconcile 不改写或合并这些用户配置
- **AND** 默认 seed source 与用户 source 独立加载

#### Scenario: 重复启动

- **WHEN** 同一 seed 版本已经成功安装
- **THEN** 后续启动不重复安装或重复注册 seed hook

#### Scenario: seed hook 被用户修改

- **WHEN** 已安装 seed hook 的内容不再匹配官方定义指纹
- **THEN** ACECode 保留用户内容
- **AND** 不把该内容作为 managed hook 自动信任

#### Scenario: 在 Herdr 外运行

- **WHEN** 任一默认 seed hook 在缺少完整 Herdr pane 环境时运行
- **THEN** 它成功退出且不调用 Herdr

#### Scenario: 在 Herdr 内运行

- **WHEN** 默认 seed hook 已安装并在 Herdr pane 中运行
- **THEN** 它通过通用生命周期事件上报 `idle`、`working` 与 `blocked`
- **AND** ACECode core 不包含 Herdr 专属的运行时检测或 reporter
