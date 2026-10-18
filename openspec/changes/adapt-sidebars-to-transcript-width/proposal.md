## Why

窗口缩小时，左右边栏与预览会挤压 transcript，导致最重要的对话内容不可读。需要优先为 transcript 让出空间，并在窄窗口里保留通过 popup 使用边栏的能力。

## What Changes

- 以 transcript 的可用宽度为依据，先自动收起整个右侧工作区，再收起左栏。
- 所有字号统一保留 350 CSS px 阅读宽度，恢复时增加 64 px 余量，避免临界抖动。
- 窄窗口 expand 打开对应侧 popup，栏内正常操作，点击外部、再次点击按钮或 Escape 收起；两侧 popup 互斥。
- 自动折叠与 popup 不修改持久化开关和栏宽；仅恢复用户原先开启的栏。
- 保留预览页签、编辑草稿和子对话状态；窗口极窄时优先让 transcript 使用可用空间。

## Capabilities

### New Capabilities
- `responsive-workbench-layout`: transcript 优先的空间分配、自动折叠、popup 与偏好恢复。

### Modified Capabilities
- `webui-side-panel`: 单会话侧面板不再要求所有窗口宽度下固定占位，支持响应式隐藏与浮层。

## Impact

- Web/Desktop 共用的 App、ChatView、TopBar、布局求解器及独立响应式样式。
- 复用现有全局左栏偏好、工作台右栏偏好与全局栏宽，不改变存储作用域或后端协议。
- 增加纯逻辑与 Chromium 真实组件交互验证；保留当前工作区其他未提交修改。
