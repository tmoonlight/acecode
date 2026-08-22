## MODIFIED Requirements

### Requirement: Model ID Picker Discovery
ACECode 的 Web/Desktop 模型管理表单必须（SHALL）在适用时提供真实 OpenAI 兼容模型列表探测，同时始终保留可直接编辑的模型 ID 输入。

#### Scenario: Probe succeeds
- **WHEN** 用户为 OpenAI 兼容草稿配置可用的 Base URL 和认证信息并发起模型探测
- **THEN** 页面请求 Daemon 探测当前 Provider，并把返回的模型 ID 显示为可选择项

#### Scenario: Probe fails
- **WHEN** 探测端点返回错误或没有可用模型
- **THEN** 选择弹窗显示脱敏后的失败信息或空状态
- **THEN** 主表单中的自定义模型 ID 输入保持可用且原值不变

#### Scenario: Manual model is entered
- **WHEN** 用户直接在自定义 OpenAI 兼容 Provider 的 `Model ID` 输入框中输入模型 ID
- **THEN** 草稿立即保留该值，无需先探测或执行额外添加步骤

#### Scenario: Open custom provider probe selector
- **WHEN** 用户在自定义 OpenAI 兼容 Provider 的 `Model ID` 标题行点击“探测模型”
- **THEN** 页面立即打开独立模型选择弹窗并使用当前 Base URL、API Key 和自定义请求头发起探测
- **THEN** 新增模式使用复选框并允许同时选择多个探测结果

#### Scenario: Probe selector resists implicit dismissal
- **WHEN** 模型选择弹窗打开后用户点击遮罩或按 Escape
- **THEN** 选择弹窗保持打开，当前勾选状态和主表单草稿均不得改变

#### Scenario: Cancel probe selection with close icon
- **WHEN** 用户点击模型选择弹窗右上角叉号
- **THEN** 选择弹窗关闭且主表单中的模型 ID 和其他草稿字段保持不变

#### Scenario: Confirm probe selection
- **WHEN** 用户至少选择一个探测模型并点击“添加所选模型”
- **THEN** 所选模型 ID 一次性回填主表单，选择弹窗随后关闭
- **THEN** 没有选中模型时确认操作保持禁用且弹窗不得关闭

### Requirement: Multi-Model Add Persistence
ACECode 的 Web/Desktop 新增模型表单必须（SHALL）支持选择或输入多个模型 ID，并把每个选中模型持久化为独立的已保存模型预设。

#### Scenario: Single model add
- **WHEN** 新增表单包含一个模型 ID 并提交
- **THEN** 系统使用用户填写的名称，或在名称为空时使用该 Model ID，创建一个已保存模型预设

#### Scenario: Multiple model add
- **WHEN** 用户从自定义 OpenAI 兼容探测弹窗勾选多个模型，或直接输入多个模型 ID 后提交
- **THEN** 系统为每个模型 ID 创建一个独立预设
- **THEN** 空预设名称按每个 Model ID 精确生成，非空名称继续使用稳定的批量命名规则

#### Scenario: Existing profile edit remains single-model
- **WHEN** 用户编辑已有模型预设并打开探测选择弹窗
- **THEN** 弹窗只允许选择一个模型，提交仍只更新当前预设
