## Purpose

为内置消息通道提供与当前会话绑定的权限模式和思考深度指令，使用户在 IM 中查询并调整执行策略，同时保留授权、绑定隔离和忙碌会话的既有行为。

## ADDED Requirements

### Requirement: 通道权限模式指令

内置消息通道 SHALL 支持 `/mode` 查询及 `/mode default|auto|yolo` 切换绑定会话权限，兼容 `accept-edits` 为 `auto`。`/yolo` SHALL 直接等价于 `/mode yolo`，使用现有完全访问权限语义，包括允许已挂起的权限请求。所有回复 SHALL 使用中文并展示模式名称。

#### Scenario: 切换三种权限
- **WHEN** 已授权用户依次发送 `/mode auto`、`/yolo`、`/mode default`
- **THEN** 仅其绑定会话依次切到自动、完全访问权限和默认权限，持久化模式并回复成功

#### Scenario: 查询与错误输入
- **WHEN** 用户发送 `/mode` 或 `/mode invalid` 或 `/yolo extra`
- **THEN** 查询返回当前模式和用法，错误输入返回用法并保持原状态

### Requirement: 通道思考深度指令

通道 SHALL 支持 `/effort` 查询当前思考深度与模型支持的档位、`/effort <档位>` 切换，以及 `/effort default` 恢复模型预设。档位 SHALL 取自当前模型能力；不支持的值、能力不可用或更新失败 SHALL 返回明确错误，保留原状态。

#### Scenario: 忙碌时调整深度
- **WHEN** 会话正在执行，用户发送模型支持的 `/effort high`
- **THEN** 更新会话设置并持久化，从下一次模型请求起生效，当前请求不中断

#### Scenario: 恢复模型预设
- **WHEN** 用户发送 `/effort default`
- **THEN** 移除会话覆盖，回复更新后的有效档位

#### Scenario: 模型不支持指定档位
- **WHEN** 用户请求当前模型不支持的 effort
- **THEN** 回复中文错误和当前可用档位，原设置不变

### Requirement: 命令路由与可发现性

`/help` SHALL 列出新指令，`/status` SHALL 显示当前权限模式和 effort。上述新指令 SHALL 在会话忙碌时直接处理且不进入对话或技能展开。未授权、无绑定、会话已删除及重复消息 SHALL 沿用既有处理，不能修改其他会话或全局配置。

#### Scenario: 控制命令不进入模型
- **WHEN** 已绑定会话收到新的权限或 effort 命令
- **THEN** 回复操作结果，模型输入数量保持不变

#### Scenario: 无绑定或未授权
- **WHEN** 无绑定的联系人或未授权用户发送 `/yolo`
- **THEN** 提示建立会话或完成授权，不新建会话且不修改任何权限
