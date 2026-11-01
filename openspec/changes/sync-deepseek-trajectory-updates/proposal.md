## Why

ACECode 的轨迹界面源自 DeepSeek Harness 的早期版本。本机 `C:\Users\shao\deepseek-harness` 的 `687f282e86` 已包含后续的请求状态、详情阅读和附件改进，需要将适用变化同步到 ACECode 的事件与界面适配层。

## What Changes

- 同步请求等待、流式、中断及重试恢复状态，分开显示历史重试错误与当前失败。
- 同步 JSON 长字符串展开、换行偏好、代码详情与精确复制、思考 Markdown、详情轮次/步骤标题。
- 同步图片和普通附件的有序展示与原始信息折叠，保留 ACECode 图片读取方式。
- 核对历史分页、虚拟列表、时间线与提示词恢复，保留现有回合身份和旧 WebView 兼容修复。
- 记录上游来源、适配边界和可复现验证结果。

## Capabilities

### New Capabilities
- `trajectory-inspection`: 轨迹记录、请求状态及代码、JSON、附件详情的检查交互。

### Modified Capabilities
无。

## Impact

影响 `web/src/components/trajectory/`、`web/src/lib/trajectoryModel.js` 及对应测试。继续使用 ACECode 的轨迹接口、存储和现有依赖；不引入 DeepSeek 的插件运行时或会话存储格式。
