## ADDED Requirements

### Requirement: 默认 Herdr seed 同步非空会话标题

默认 managed Herdr hook MUST 在 `SessionTitleChanged` 的标题非空时，把当前活动 pane 所属 Herdr tab 重命名为完整会话标题。

#### Scenario: 在 Herdr pane 中生成标题

- **WHEN** `HERDR_ENV=1`、`HERDR_TAB_ID`、`HERDR_SOCKET_PATH` 与 Herdr CLI 有效，且 ACECode 派发非空标题事件
- **THEN** seed MUST 调用 `herdr tab rename <HERDR_TAB_ID> <title>`
- **AND** tab ID 与标题 MUST 作为两个精确参数传递

#### Scenario: 用户通过 /title 修改标题

- **WHEN** 用户在 Herdr 中运行的 ACECode 执行 `/title 新标题`
- **THEN** 对应 Herdr tab MUST 更新为 `新标题`

#### Scenario: 用户通过 /resume 切换会话

- **WHEN** 用户成功恢复一个具有非空标题的会话
- **THEN** 对应 Herdr tab MUST 更新为恢复出的会话标题

#### Scenario: 标题为空

- **WHEN** `SessionTitleChanged` 的 `title` 为空
- **THEN** seed MUST 成功退出且 MUST NOT 调用 `herdr tab rename`

### Requirement: Herdr 标题同步保持外部可选

标题同步 MUST 只由 managed hook command 完成。Herdr 环境、tab 身份或 CLI 缺失以及 CLI 调用失败均不得影响 ACECode 标题状态。

#### Scenario: 不在 Herdr 中运行

- **WHEN** `HERDR_ENV`、`HERDR_TAB_ID` 或 `HERDR_SOCKET_PATH` 任一缺失
- **THEN** seed MUST 成功退出且不产生 Herdr 调用

#### Scenario: 找不到 Herdr CLI

- **WHEN** 必需环境有效但 `HERDR_BIN_PATH`、平台官方安装位置与 `PATH` 均无法提供 Herdr CLI
- **THEN** seed MUST 成功退出且不产生外部调用

#### Scenario: Herdr rename 失败

- **WHEN** `herdr tab rename` 返回失败或超时
- **THEN** 已提交的 ACECode 会话标题 MUST 保持不变

### Requirement: seed 升级保留用户边界

包含标题同步的 seed MUST 通过现有版本化 reconcile 升级未修改的官方副本，并 MUST 保留用户修改过的副本及其他用户 hook 配置。

#### Scenario: 已初始化用户安装新包

- **WHEN** 用户已经初始化旧 seed，且旧副本仍与上一版官方定义一致
- **THEN** 新包启动时 MUST 把该副本升级为包含 `SessionTitleChanged` 的定义
- **AND** 新定义 MUST 继续以 `ManagedTrusted` 加载

#### Scenario: 用户修改过 seed

- **WHEN** 用户修改了已安装的 agent-reporting seed
- **THEN** reconcile MUST 保留修改内容且不得自动授予新定义的 managed trust

#### Scenario: 状态漂移但文件仍是历史官方定义

- **WHEN** 本地 seed 状态把 agent-reporting 标记为非 ACECode-owned，但 `hooks.json` 精确匹配一个已知历史官方定义且目录没有额外文件
- **THEN** 新包启动时 MUST 恢复 ownership 并升级为当前定义
- **AND** bundle 版本 MUST 高于已经错误写入本地的旧版本戳

#### Scenario: 状态漂移且定义确实被修改

- **WHEN** 本地 seed 状态不可证明 ownership，且 `hooks.json` 不匹配当前或任一已知历史官方定义
- **THEN** reconcile MUST 继续保留用户内容且不得恢复 managed trust
