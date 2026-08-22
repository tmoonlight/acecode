## 为什么要做（Why）

自定义 OpenAI 兼容 Provider 目前只能手工输入 `Model ID`，虽然 Daemon 已能按当前 Base URL、API Key 和请求头探测真实模型列表，但新增模型弹窗没有入口可用。用户需要在保留直接输入的同时，通过明确、不会误关闭的多选弹窗快速加入探测到的模型。

## 变更内容（What Changes）

- 在自定义 OpenAI 兼容 Provider 的 `Model ID` 标题行右侧增加“探测模型”按钮。
- 点击按钮后立即打开独立模型选择弹窗，并复用现有 `POST /api/models/probe` 请求显示加载、错误、空结果和真实模型列表。
- 新增模式允许勾选多个探测结果；只有至少选中一个模型并确认后才回填草稿并关闭选择弹窗。
- 选择弹窗只能通过右上角叉号取消，或通过有效确认完成；点击遮罩和按 Escape 均不得关闭。
- 保留原有 `Model ID` 文本框，探测失败、结果为空或用户取消时不覆盖当前草稿。

## 能力（Capabilities）

### 新增能力（New Capabilities）

无。

### 修改能力（Modified Capabilities）

- `web-model-management`：自定义 OpenAI 兼容模型新增流程恢复可选的真实模型探测入口，并明确多选确认与选择弹窗关闭约束。

## 影响（Impact）

- 前端组件：`ProviderCatalogPicker.jsx` 及新增的聚焦模型探测选择弹窗。
- 前端辅助逻辑与测试：模型选择回填、弹窗结构、键盘/遮罩关闭行为和多选回归测试。
- Daemon API 不变，继续复用现有 `POST /api/models/probe` 合同。
