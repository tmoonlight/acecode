## Why

部分主题的明暗标记与对话区实际底色不同，Mermaid 按标记选择深色预设后，会把浅色生命线和文字画在浅色背景上，难以辨认。

## What Changes

- 根据每张图所在区域的实际背景选择对比清晰的默认线条、箭头和文字颜色。
- 主题或背景配色变更时自动重绘，并丢弃旧配色的异步渲染结果。
- 保持图表原始尺寸、透明无外框、预览和导出行为。

## Capabilities

### New Capabilities

- `web-mermaid-theme-contrast`: 对话中的 Mermaid 默认配色随实际背景保持可读，覆盖主题切换与异步渲染。

### Modified Capabilities

无。

## Impact

影响 Web 与桌面客户端共用的 Mermaid 渲染器、颜色对比度辅助函数及测试；无协议或依赖变更。
