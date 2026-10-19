## Why

Windows Desktop 长时间挂机后，侧栏刷新与桥接回包保留无用对象，可能使渲染进程内存耗尽。现有修改已完成原任务验证，本变更记录其发布集成与补验。

## What Changes

- 侧栏相同内容不写入 React 状态，动态事件回调保持稳定身份。
- 桌面桥接回包后释放 Promise，重复托盘菜单限频。
- Windows 主渲染进程退出后自动重载，十分钟内最多三次；宿主析构后不执行恢复。
- 开发者工具设置使用调用内引用，消除所有权检查的延迟注入误判。

## Capabilities

### New Capabilities
- `desktop-idle-memory`: 空闲资源释放与有界渲染恢复。

### Modified Capabilities

## Impact

涉及 Sidebar、桌面桥接、Windows WebView2 与对应测试。不修改 seed 或协议；macOS/Linux 不新增崩溃恢复事件。ChatView/App 活跃切换的剩余增长留后续任务。
