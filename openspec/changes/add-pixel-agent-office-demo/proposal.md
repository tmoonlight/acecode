## Why

在接入真实 Agent Team 之前，先用独立可运行的像素办公室验证视觉与交互。用户提供了等距像素办公室参考图，并要求用代码绘制、最大化资源复用。

## What Changes

- 在 design-prototypes/agent-office 提供离线可打开的独立演示页。
- 共享等距地砖、家具、人物模板和换色动画，表现六位成员的工作、走动与休息。
- 支持成员选择、演示任务派发、暂停、速度切换与场景缩放。
- 提供资源图鉴、源码结构与验证说明，方便未来接入真实团队状态。

## Capabilities

### New Capabilities
- `pixel-agent-office-demo`: 独立像素办公室的渲染、模拟状态和交互演示。

### Modified Capabilities

无。

## Impact

只增加独立设计原型和本次 OpenSpec 文档，不改变生产 Web、C++、API 或依赖。演示任务和统计必须明确为模拟数据。
