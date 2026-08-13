## ADDED Requirements

### Requirement: 会话标题变化可通过 hook 观测

ACECode MUST 在当前活动会话的标题状态成功改变或因恢复会话而切换后派发 `SessionTitleChanged`。该事件 MUST 包含公共 hook 字段、`title`、`source` 与 `title_source`，且事件输出不得回滚或替换已经提交的标题。

#### Scenario: 用户设置标题

- **WHEN** 用户成功执行 `/title 修复登录问题`
- **THEN** ACECode MUST 在标题持久化后恰好派发一次 `SessionTitleChanged`
- **AND** 载荷 `title` MUST 为 `修复登录问题`，`source` MUST 为 `user`，`title_source` MUST 为 `user`

#### Scenario: 用户清除标题

- **WHEN** 用户成功执行 `/title clear` 或 `/title ""`
- **THEN** ACECode MUST 派发一次 `SessionTitleChanged`
- **AND** 载荷 `title` MUST 为空，`source` MUST 为 `user`，`title_source` MUST 为 `user-cleared`

#### Scenario: 用户只查询标题

- **WHEN** 用户执行不带参数的 `/title`
- **THEN** ACECode MUST NOT 派发 `SessionTitleChanged`

#### Scenario: 自动标题生成成功

- **WHEN** TUI 或 daemon session 接受一个自动生成标题
- **THEN** ACECode MUST 派发一次 `SessionTitleChanged`
- **AND** 载荷 `source` MUST 为 `generated`，`title_source` MUST 为 `generated`

#### Scenario: 标题更新被拒绝

- **WHEN** 自动标题因用户标题优先级、过期 session ID 或校验失败而未被接受
- **THEN** ACECode MUST NOT 派发 `SessionTitleChanged`

### Requirement: 恢复会话派发标题状态

成功的活动会话恢复 MUST 在恢复完成后派发一次 `SessionTitleChanged`，并使用 `resume` 作为触发来源。恢复失败 MUST NOT 派发该事件。

#### Scenario: /resume 恢复有标题会话

- **WHEN** 用户通过 `/resume` 成功恢复标题为 `部署脚本` 的会话
- **THEN** 事件 `title` MUST 为 `部署脚本` 且 `source` MUST 为 `resume`
- **AND** `title_source` MUST 保留该会话元数据中的来源

#### Scenario: CLI 参数恢复无标题会话

- **WHEN** 用户通过 `acecode --resume <id>` 成功恢复无标题会话
- **THEN** ACECode MUST 派发 `title` 为空、`source` 为 `resume` 的事件

#### Scenario: daemon 恢复会话

- **WHEN** daemon registry 成功恢复一个活动会话
- **THEN** 对应 session 的 AgentLoop MUST 派发一次恢复标题事件

### Requirement: 标题事件命令获得安全字段环境

`SessionTitleChanged` command hook MUST 继续从 stdin 获得完整 JSON，并 MUST 在该 child process 环境中获得与载荷 `title` 完全相等的 `ACECODE_HOOK_SESSION_TITLE`。ACECode MUST NOT 为此修改进程全局环境。

#### Scenario: 标题含 shell 元字符

- **WHEN** 标题包含空格、中文、引号、百分号或 shell 元字符
- **THEN** command hook 读取的 `ACECODE_HOOK_SESSION_TITLE` MUST 与原始标题逐字节等价
- **AND** 标题内容 MUST NOT 被执行为 shell 语法

#### Scenario: 空标题

- **WHEN** 标题被清除或恢复的会话无标题
- **THEN** child process MUST 收到已定义但值为空的 `ACECODE_HOOK_SESSION_TITLE`

### Requirement: 配置解析器识别标题事件

`SessionTitleChanged` MUST 被识别为 Codex hook 事件，且 matcher MUST 与载荷的 `source` 匹配。

#### Scenario: 裸标题事件配置

- **WHEN** hook source 是只含 `SessionTitleChanged` 数组的裸对象
- **THEN** ACECode MUST 将其识别为 Codex hook 配置并加载 handler
