## Why

窗口缩小时，左右边栏与预览会挤压 transcript。按 2026-10-20 用户要求，自动折叠仅作用于左边栏，右侧文件列表与详情保持手动开关控制；窄窗口保留通过 popup 临时使用左栏的能力。

## What Changes

- 以包含右侧文件列表、详情与子对话占宽的 transcript 可用宽度为依据，仅自动收起左栏；右栏不因窗口缩放自动收起或变为浮层。
- 所有字号统一保留 350 CSS px 阅读宽度，恢复时增加 64 px 余量，避免临界抖动。
- 窄窗口左栏 expand 打开可调宽 popup，栏内正常操作，点击外部、再次点击按钮或 Escape 收起；右栏按钮始终按手动开关展开或收起。
- 自动折叠与 popup 不修改持久化开关和栏宽；仅恢复用户原先开启的栏。
- 保留预览页签、编辑草稿和子对话状态；左栏收起后仍不足 350 px 时保持右栏手动选择，transcript 使用剩余空间。

## Capabilities

### New Capabilities
- `responsive-workbench-layout`: transcript 优先的空间分配、自动折叠、popup 与偏好恢复。

### Modified Capabilities
- `webui-side-panel`: 单会话右侧面板在所有窗口宽度下均由手动开关决定是否占位。

## Impact

- Web/Desktop 共用的 App、ChatView、TopBar、布局求解器及独立响应式样式。
- 复用现有全局左栏偏好、工作台右栏偏好与全局栏宽，不改变存储作用域或后端协议。
- 增加纯逻辑与 Chromium 真实组件交互验证；保留当前工作区其他未提交修改。
