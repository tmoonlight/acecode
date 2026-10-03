## ADDED Requirements

### Requirement: 最近工作完整整合
系统 SHALL 将用户指定时间窗口内有效且未整合的代码合入最终 master，同时保留其后的主线行为及全部相关回归测试。

#### Scenario: 同一修复已部分合入
- **WHEN** worktree 中部分修改已在 master，另有后续补充
- **THEN** 仅集成缺失修改，合并重叠测试，并记录来源与校验结果

### Requirement: 命令拆分保持行为
TUI SHALL 在同一注册顺序下提供既有 `/tasks` 列出和中止行为，并通过严格尺寸和所有权检查。

#### Scenario: 内置命令文件超过基线
- **WHEN** `/tasks` 被拆入独立命令模块
- **THEN** 用户可见命令及行为不变，文件低于既有基线且无需新增豁免

### Requirement: 完整发布之后关机
正式版 SHALL 从验证过的最终 master 生成并完整镜像到更新服务；关机 MUST 等待发布完成且 Claude 任务确认结束。

#### Scenario: 产物或任务尚未完成
- **WHEN** 任一必需产物缺失、验证失败或 Claude 仍在工作
- **THEN** 不报告完整发布成功，不执行关机
