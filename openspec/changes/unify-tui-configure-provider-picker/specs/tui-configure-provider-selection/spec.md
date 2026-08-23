## ADDED Requirements

### Requirement: configure 使用统一 Provider 候选列表

`acecode configure` MUST 在同一个 Provider picker 中展示自定义接口、ACEModel、受管 Copilot 和可运行的 models.dev 预置，不得要求用户先在四项顶层菜单中选择是否浏览 catalog。

#### Scenario: catalog 可用时的固定顺序

- **WHEN** models.dev catalog 含可运行的 OpenAI 兼容 Provider
- **THEN** 候选列表第一项 MUST 是自定义 OpenAI 兼容 API
- **AND** 第二项 MUST 是自定义 Anthropic 兼容 API
- **AND** 第三项 MUST 是 ACEModel
- **AND** 第四项 MUST 是 GitHub Copilot
- **AND** models.dev 预置 MUST 从第五项开始依照 catalog 顺序展示

#### Scenario: Copilot 不重复出现

- **WHEN** models.dev 可运行 Provider 中包含 `github-copilot`
- **THEN** 统一列表 MUST 只显示受管 GitHub Copilot 预置
- **AND** MUST NOT 再把 `github-copilot` 显示为普通 API Key 目录项

#### Scenario: ACEModel 不重复出现

- **WHEN** models.dev 可运行 Provider 中包含 ID 为 `acemodel` 的条目
- **THEN** 统一列表 MUST 保留共享内置 ACEModel 预置
- **AND** MUST NOT 再显示同 ID 的 models.dev 条目

#### Scenario: catalog 不可用

- **WHEN** models.dev catalog 缺失、无效或没有可运行 Provider
- **THEN** 统一列表 MUST 仍显示两个自定义接口、ACEModel 和 GitHub Copilot
- **AND** MUST NOT 显示一个不可选择的 Browse catalog 占位入口

### Requirement: ACEModel 元数据只有一个共享来源

系统 MUST 以一个非 Web 专属的共享定义提供 ACEModel 的 Provider ID、名称、Base URL、API Key 环境变量和内置模型，并由 Web 模型目录与 TUI configure 同时消费。

#### Scenario: Web 与 TUI 读取同一预置

- **WHEN** 系统构造 Web catalog 摘要、查询 ACEModel 模型或构造 TUI Provider 候选
- **THEN** 三条路径 MUST 使用同一个共享 ACEModel `ProviderEntry`
- **AND** ACEModel MUST 使用 `https://ge.bigjuan.xyz/aceapi/v1`、`ACEMODEL_API_KEY`、`moonlight` 与 `starrylight`

### Requirement: Provider picker 提供直接输入 autocomplete

统一 Provider picker MUST 在候选列表上方显示搜索文本框，并在用户直接输入时按 Provider ID、展示名或说明进行不区分大小写的实时子串过滤。

#### Scenario: 直接输入筛选预置

- **WHEN** 用户在 picker 打开后直接键入 `router`
- **THEN** 系统 MUST 无需先按 `/` 即更新建议列表
- **AND** 匹配 ID、展示名或说明中 `router` 的候选 MUST 保留
- **AND** 第一条匹配建议 MUST 自动高亮

#### Scenario: 键盘确认建议

- **WHEN** 搜索结果已显示且用户按上、下、PageUp、PageDown、Home 或 End
- **THEN** 高亮 MUST 在当前建议列表内移动
- **AND** 用户按回车时 MUST 提交当前高亮候选

#### Scenario: 清空与取消

- **WHEN** 搜索文本非空且用户按 Esc
- **THEN** 系统 MUST 清空搜索并恢复完整候选列表，而不是退出 configure
- **WHEN** 搜索文本已空且用户再次按 Esc
- **THEN** 系统 MUST 取消 Provider 选择且不得保存配置

#### Scenario: 非 TTY 回落

- **WHEN** configure 运行在非 TTY stdout 下且用户输入非编号查询文本
- **THEN** stdin picker MUST 用该文本过滤候选并重新打印列表
- **AND** 编号选择、翻页与取消命令 MUST 继续可用

### Requirement: 统一候选路由到既有配置流程

系统 MUST 根据候选类型调用既有配置流程，并保持各流程原有认证、字段填写、模型选择和持久化语义。

#### Scenario: 选择自定义 OpenAI

- **WHEN** 用户选择第一项自定义 OpenAI 兼容 API
- **THEN** 系统 MUST 执行原第 3 项的 OpenAI 兼容 Base URL、API Key、Model 和连接测试流程
- **AND** `models_dev_provider_id` MUST 被清空

#### Scenario: 选择自定义 Anthropic

- **WHEN** 用户选择第二项自定义 Anthropic 兼容 API
- **THEN** 系统 MUST 执行原第 4 项的 Anthropic Profile name、Base URL、API Key 和 Model 流程
- **AND** 保存模型 MUST 使用运行时 Provider `anthropic`

#### Scenario: 选择 Copilot

- **WHEN** 用户选择 GitHub Copilot 预置
- **THEN** 系统 MUST 执行现有 GitHub 设备认证和 Copilot 模型选择流程
- **AND** MUST NOT 要求用户填写 Base URL 或 API Key

#### Scenario: 选择 ACEModel

- **WHEN** 用户选择 ACEModel 预置
- **THEN** 系统 MUST 使用共享 ACEModel Base URL、API Key 环境变量和内置模型进入 OpenAI 兼容预置配置流程
- **AND** 保存时 MUST 使用运行时 Provider `openai`
- **AND** `models_dev_provider_id` MUST 为 `acemodel`

#### Scenario: 选择 models.dev 预置

- **WHEN** 用户选择一个 models.dev 目录 Provider
- **THEN** 系统 MUST 直接进入该 Provider 的 Base URL、env/API Key 和模型配置流程
- **AND** MUST NOT 再打开第二个 Provider 选择器
- **AND** 保存时 MUST 保留所选 `models_dev_provider_id`

### Requirement: 当前配置决定默认高亮

统一 Provider picker MUST 根据当前运行时 Provider 和 `models_dev_provider_id` 确定初始高亮，同时为未知或未配置状态保留旧向导的 Copilot 默认项。

#### Scenario: 已配置自定义接口

- **WHEN** 当前 Provider 是无 models.dev ID 的 `openai`
- **THEN** 初始高亮 MUST 是第一项自定义 OpenAI 兼容 API
- **WHEN** 当前 Provider 是 `anthropic`
- **THEN** 初始高亮 MUST 是第二项自定义 Anthropic 兼容 API

#### Scenario: 已配置受管或目录 Provider

- **WHEN** 当前 Provider 是 `copilot`
- **THEN** 初始高亮 MUST 是 GitHub Copilot
- **WHEN** 当前 Provider 是 `openai` 且 `models_dev_provider_id` 是 `acemodel`
- **THEN** 初始高亮 MUST 是 ACEModel
- **WHEN** 当前 Provider 是 `openai` 且 `models_dev_provider_id` 命中统一列表中的目录项
- **THEN** 初始高亮 MUST 是该目录项

#### Scenario: 未配置状态

- **WHEN** 当前 Provider 为空或无法映射到任何候选
- **THEN** 初始高亮 MUST 是 GitHub Copilot
