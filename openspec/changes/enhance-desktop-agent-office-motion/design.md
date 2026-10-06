## Context

见 proposal.md。前一变更 `integrate-desktop-agent-office` 已交付 Windows 办公室；本变更只增强表现层与平台覆盖。

## Decisions

### 1. 状态来源

服务端 `SessionActivityState` 在分发器锁内折叠事件：token → `responding`，reasoning → `reasoning`，各自保留有界尾巴，换阶段或开始工具即清空；工具开始记录 `display_override`（为空取 `command_preview`）。这样快照在任意时刻都说得清「正在做什么」，打开办公室或断线重连后不依赖重放。实时性由 Web 控制器负责：token/reasoning 事件按会话写入叠加层，`seq` 大于快照才生效，快照追上即丢弃；连续正文在快照尾巴后拼接。不为每个 token 发 HTTP 请求。

### 2. 文案

动作动词与工具图标同表（`toolIcons.js`）；「适合日常工作」模式下的具体进度标题优先，引擎默认的笼统文案（等待模型响应、正在调用工具 X…）不进气泡。细节按半角宽度从开头裁剪，保留最新的字和文件名。

### 3. 交接动画

走路沿 2 单位网格 BFS（8 向、不斜穿桌角），家具占地外扩 1 单位；先到派活的人（`agent_path` 的上一级，找不到时是主 agent）桌前停顿并交接信封，再回工位或出门。离开中的成员又接到任务时掉头回座。只在本办公室已显示过真实快照后演，切换办公室和重连恢复直接落座。减少动态效果时不走路但保留信封。

### 4. 原生窗口区域

Windows `SetWindowRgn` 同时裁剪绘制，气泡冒出墙顶会被切掉，所以页面把气泡矩形（含尾巴与引线）节流上报，原生端与成员列表/提示一起并入区域。macOS 没有窗口区域，定时检测鼠标是否命中房间、控制条或这些矩形，切换 `ignoresMouseEvents`。命中判定与消息解析是纯函数，跨平台单测。

### 5. macOS 宿主

NSPanel（无边框、非激活、不可成为 key window、浮动层级、所有空间可见），WKWebView 关闭背景绘制并重载 `acceptsFirstMouse`。页面按 WebView2 接口编写，宿主在文档开始前注入同名垫片，消息经 `WKScriptMessageHandler` 回到 C++，宿主经 `evaluateJavaScript` 派发。拖动与把手缩放由 60Hz 定时器跟随鼠标。ObjC 回调只持有控制器 weak_ptr；关闭时移除脚本消息处理器（否则内容控制器强引用导致泄漏）。默认 1.25 倍逻辑尺寸，设置不写入 ACECode 数据目录。

## Risks / Trade-offs

- [macOS 未在本机编译] → 只用稳定的 AppKit/WebKit 公共接口与 ARC；需 macOS CI 或实机构建确认。
- [气泡拥挤] → 优先级 + 错开/抬高/压缩三级避让；极端满员时仍可能重叠。
- [流式尾巴进入快照] → 只保留 160 字节、换阶段清空，接口仍需鉴权，与聊天页可见内容一致。
