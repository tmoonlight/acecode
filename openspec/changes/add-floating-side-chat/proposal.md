## Why

现有 `/side` 在主聊天内显示一次性问题和回答，不能继续追问，也不能实时查看或停止输出。用户需要参考截图中的独立浮窗，在阅读主会话时连续讨论而不影响主任务。

## What Changes

- Web/Desktop 的 `/side`、`/btw`、“侧边聊天”菜单和会话栏右上角气泡入口统一打开顶层浮窗，标题栏右侧提供清空和最小化按钮，支持拖动和四边四角缩放。
- 旁路聊天保留独立的多轮记录，复用主会话的安全上下文快照，但不写入主历史或执行工具。
- 提供真实流式输出、等待动画和停止生成，等待及输出期间禁用旁路输入框。
- 最小化或切换主会话时停止生成，按主会话隔离保留临时记录、草稿与窗口开闭状态。
- 在设置「常规」增加侧边聊天存续时间，单位分钟、默认 30 分钟；从最后一次侧聊完成、停止或失败开始计时，达到时间后下次切回该会话时清空，不在当前界面自动清空。
- 点击垃圾桶停止当前侧聊请求并清空记录、草稿和追问上下文，窗口保持打开；底部输入区域压缩至含边框 60px。
- 保留旧同步单轮接口及 TUI 行为，新增独立的流式旁路通信接口。

## Capabilities

### New Capabilities
- `floating-side-chat`: 全局浮窗布局、隔离多轮上下文、流式输出、取消和生命周期。

### Modified Capabilities

无。当前 checkout 没有 `openspec/specs`；本变更替代 `add-desktop-file-editing-and-preview-tab-controls` 中主聊天内嵌旁路输入的设计。

## Impact

- `web/src/components/ChatView.jsx`、旁路组件、共享 API 客户端和 i18n catalog。
- `src/web` 路由及 `SessionRegistry` / `AgentLoop` 旁路调用边界，`docs/daemon-api.md`。
- 前后端回归测试及真实浏览器浮窗交互验证。不增加运行时依赖。
