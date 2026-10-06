## Why

桌面 agent 办公室接入真实会话后，用户反馈三点：看不到代理之间交互的动画；代理飙字时看不到状态，气泡只出现一个图标、看不出在做什么；需要 macOS 支持。根因分别是：交接只有一个约 1 秒、9×6 像素的小信封，成员直接闪现/消失；默认「用于编程」模式下服务端不发正文进度，快照一直停在「等待模型」，WebSocket 的 token 事件又被控制器整体忽略，忙碌状态没有文字气泡；原生窗口只有 Windows 实现。

## What Changes

- 会话活动快照增加流式正文/推理尾巴（≤160 字节）与工具调用预览（≤120 字节），由 token 事件推断 `responding`。
- 控制器用 WebSocket 的 token / reasoning 事件按 seq 叠加实时文字（约 150ms 节流），不增加快照请求。
- 忙碌 agent 常驻状态气泡「图标 动作 · 细节」，按优先级避让；打字、阅读、思考、等成员各有动作。
- 成员走进门、到派活的人桌前领「任务」信封、回工位；完成后走去递「回报」信封再出门；网状消息带标签信封。减少动态效果时不走路但保留信封。
- 页面上报气泡矩形，Windows 窗口区域并入（避免被裁剪），macOS 用于点击穿透判定。
- 新增 macOS 原生桌宠：非激活 NSPanel + WKWebView，注入 `chrome.webview` 垫片复用同一页面。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `desktop-agent-office`: 状态可读性、交接动画与 macOS 宿主（以新增要求记录，原变更尚未归档）。

## Impact

会话活动状态（domain）、办公室快照文档、Web 办公室状态/控制器与图标动词、桌宠页面、Windows 宿主窗口区域、新增 macOS 宿主与 CMake 登记。不改变 agent 调度、权限或模型请求。macOS 宿主未在本机编译验收，需在 macOS 构建与实机确认。
