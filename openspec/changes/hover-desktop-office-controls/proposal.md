## Why

办公室顶部控件常驻会遮住桌面，且当前原生窗口始终置顶。用户希望控件按需出现，并能直接切换置顶或关闭桌宠。

## What Changes

- 鼠标进入办公室立即显示顶部菜单；移出后延迟 1000ms 隐藏，重新进入取消隐藏。键盘操作保留可见焦点。
- 菜单最右侧增加 SVG pin 与关闭按钮。默认保持置顶，取消 pin 后按普通窗口层级显示；缩放、停靠和 DPI 更新不重置层级。
- 隐藏的菜单不占用原生点击区域；关闭仅影响桌宠。

## Capabilities

### New Capabilities
- `desktop-office-window-controls`: 桌宠菜单显隐、置顶开关及关闭操作。

### Modified Capabilities

无。

## Impact

共享桌宠 HTML、Windows/WebView2 与 macOS/WKWebView 宿主、原生命中区域解析、现有办公室浏览器和几何测试。保持 Claude 已增强的场景动画、会话快照与美术实现。
