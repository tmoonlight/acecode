## Why

Windows Desktop 目前通过拖放后的 `file://` 导航事件间接恢复本地路径；该事件不是可靠的文件拖放数据源，可能完全不触发，也可能把一次多选拆成若干单条回调，最终造成文件夹无响应或多文件丢失。会话框需要直接接收同一次拖放中的完整原生文件列表，才能稳定区分文件与文件夹。

## What Changes

- 使用 WebView2 的附加 WebMessage 对象一次传递完整的 Windows 拖放 `File` 批次，并由 Desktop host 读取每个对象的绝对路径。
- 会话框接收批次后继续复用现有文件系统物化逻辑：普通文件分别成为附件卡片，文件夹只在当前光标处插入绝对路径 reference。
- 终端沿用同一原生批次桥接和既有落点归属规则，避免修复会话框时破坏终端拖放。
- 对不支持新 WebView2 接口的运行时保留原有 `file://` 导航兜底，不改变非 Desktop Web 与其他平台行为。
- 补齐 WebView2 110 兼容：旧运行时通过 Windows OLE `CF_HDROP` 接收原生绝对路径，复用当前富文本 tag/图片插入与按释放坐标路由的流程。

## Capabilities

### New Capabilities

- `windows-native-filesystem-drop`: 规定 Windows Desktop 使用原生可验证路径处理单文件、文件夹和多文件拖放批次，并保持会话框与终端落点隔离。

### Modified Capabilities

无。

## Impact

- Windows Desktop WebView2 SDK 版本与 `src/desktop/web_host.cpp` 的 WebMessage 接收逻辑。
- `web/src/` 中会话框、终端和拖放桥接辅助函数。
- JavaScript 拖放回归测试、Desktop Windows 编译验证，以及既有文件系统物化测试。
