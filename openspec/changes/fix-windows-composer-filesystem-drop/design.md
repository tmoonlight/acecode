## Context

现有 Desktop 已能把绝对路径物化成统一的 file/folder item，并能把 file item 变成附件、把 folder item 变成 composer path reference。缺口位于 Windows 拖放入口：`web_host.cpp` 只监听拖放引起的 `NavigationStarting` / `NewWindowRequested`，再从单个 `file://` URI 猜测被拖入的项目。WebView2 不保证普通 drop 会触发导航，而且一次 Explorer 多选可能产生多个独立事件，因此这条入口不能代表一次完整拖放事务。

WebView2 的 `chrome.webview.postMessageWithAdditionalObjects` 可以把 DOM `File` 作为附加对象发给宿主；宿主通过 `ICoreWebView2WebMessageReceivedEventArgs2` 和 `ICoreWebView2File::get_Path` 读取由 WebView2 提供的绝对路径。这比浏览器脚本读取非标准 `File.path` 或继续依赖导航事件可靠，也不会信任网页自行构造的路径字符串。

## Goals / Non-Goals

**Goals:**

- Windows Explorer 一次拖入的所有文件和文件夹以一个有序批次到达 Desktop host。
- 会话框将每个普通文件显示为独立附件，将每个文件夹插入为绝对路径 reference；混合拖放同时完成两类结果。
- 终端继续接收拖到终端区域的完整路径批次，且不会与会话框重复消费。
- 阻止 WebView2 对已接管拖放执行文件导航，并为旧运行时保留现有兜底。

**Non-Goals:**

- 递归上传文件夹内容。
- 在普通远程 Web 页面中恢复浏览器刻意隐藏的绝对路径。
- 改变附件上传、预览、移除或模型上下文格式。
- 改变 macOS 和 Linux 已有拖放入口。

## Decisions

### 1. 用 WebView2 additional objects 传递拖放批次

Windows Desktop 的 drop handler 从 `DataTransferItemList` 取得全部 `File` 对象，并调用一次 `postMessageWithAdditionalObjects`。若 item 列表不能产出对象，才回退到 `FileList`。Desktop host 仅处理固定版本标记的消息，并从 additional object collection 逐个查询 `ICoreWebView2File`，按顺序收集绝对路径后只调用一次现有 `FileDropHandler`。

选择该方案是因为绝对路径来自 WebView2 的原生对象，而不是网页字符串；批次边界也与用户的一次 drop 一致。继续合并若干 `file://` 导航事件仍无法覆盖“不导航”的正常 drop，也需要猜测批次结束时间。

### 2. 复用现有 materialize 与落点归属

宿主得到路径批次后仍调用现有全局 file-drop handler。会话框和终端各自使用最近拖放悬停标记决定是否接收，因此无需建立第二套附件或 reference 逻辑。会话框只对一次批次调用一次 `materializeNativeFilesystemPaths`，原生层负责规范化、存在性校验和 file/folder 分类。

与在 C++ 中直接修改 React 状态相比，这保留了现有 UI 单一状态入口，并使文件夹引用格式、附件 source path 与手动选择/粘贴保持一致。

### 3. 成功投递后接管浏览器默认 drop

只有 additional-object bridge 存在且确实投递了至少一个对象时，目标组件才 `preventDefault` 并停止传播，防止 WebView2随后导航或 Slate 插入文件名。bridge 不可用或投递失败时不宣称成功，继续允许现有 `file://` 导航拦截作为兼容兜底。

### 4. 固定到支持 dropped File path 的 WebView2 SDK

Windows 构建把 `WEBVIEW_MSWEBVIEW2_VERSION` 固定到稳定版 `1.0.4078.44`，以编译 `ICoreWebView2WebMessageReceivedEventArgs2`、`ICoreWebView2ObjectCollectionView` 和 `ICoreWebView2File`。运行时能力仍由前端 API 存在性和 native `QueryInterface` 动态检查，避免接口缺失时崩溃。

只手写新版 COM interface GUID 会把 SDK ABI 细节散落到项目源码；升级官方 header/loader 更易审计和维护。

## Risks / Trade-offs

- **[旧 WebView2 Runtime 不提供 additional objects]** -> 前端先检测 API，native 也检查 `QueryInterface`；失败时保留原导航拦截，不调用空指针。
- **[目录对象在不同 Runtime 中的 DOM 表现有差异]** -> 优先遍历 `DataTransferItemList` 的所有 file-kind item，并由 native 文件系统重新分类路径，不根据 MIME 或零字节大小猜目录。
- **[同一次 drop 被新旧入口重复处理]** -> 新桥接成功即阻止默认 drop，因而不会再产生 `file://` 导航；native 仍对同批次路径去重并保持首次出现顺序。
- **[依赖版本升级影响 Windows 构建]** -> 只替换官方 WebView2 SDK/header/loader，不升级 `webview/webview` 封装；用 Windows Desktop Release 编译作为必过验证。
- **[用户拖入超限或不可读文件]** -> 继续由现有 materializer 返回统一错误，不绕过 25 MiB 限制或文件权限检查。

## Migration Plan

1. 更新 Windows WebView2 SDK pin 并增加 native additional-object 接收器。
2. 在会话框和终端 drop 入口启用批次投递，同时保留导航 fallback。
3. 运行 JavaScript 单测、Web 全量测试/构建和 Windows Desktop Release 编译。
4. 回滚时移除 additional-object 接收器和前端投递调用，并恢复旧 SDK pin；既有附件和会话数据不需要迁移。

## Open Questions

无。

## WebView2 110 compatibility follow-up

用户在 Windows WebView2 110 上报告外部文件拖入富文本框后不显示 tag。Microsoft 的 [SDK 1.0.1774.30 发布说明](https://learn.microsoft.com/en-us/microsoft-edge/webview2/release-notes/sdk/1-0-1774-30) 将 File/AdditionalObjects API 列为 Runtime 113 开始的能力。110 不暴露该 bridge，而 contenteditable drop 并不保证产生 `file://` 导航，因此原导航兜底不能完成文件接收。

- 对运行时主版本小于 113 的 Windows WebView 安装原生 OLE `IDropTarget` 适配器，从 `CF_HDROP` 一次读取完整路径批次；不根据文件名猜路径，也不把文件另存为缓存副本。
- WebView2 的实际拖放 HWND 属于独立浏览器进程，不能把其 `OleDropTargetInterface` 当作宿主可借用的 COM 指针，也不能直接覆盖该注册。宿主创建独立的透明自有窗口与 OLE target，仅在外部应用持有鼠标 capture 的拖动期间显示。通过系统保证发送的 capture WinEvent 检测该时段，并排除本进程及当前 WebView 子窗口所属进程。接收层显式配对 `OleInitialize`/`OleUninitialize`，使用 owned popup 以兼容没有 Windows 8+ compatibility manifest 的宿主。
- 普通点击和内部 HTML5 拖动保持原 WebView 目标；外部非文件拖入接收层时立即隐藏它，让 OLE 在后续命中原目标。文件 hover 通过独立的 native hover 回调进入既有前端反馈，不发送合成 drop；真实文件 drop 仅调用一次原生路径回调。导航完成和开始外部拖动时按实际 widget 尺寸调整接收层；取消和释放后隐藏，析构撤销 hook、OLE 注册和自有窗口，不改动浏览器注册。
- 文件释放坐标相对 WebView widget 归一化，复用已有 native router 命中唯一 composer 或终端。普通文件、文件夹和图片按当前统一 intake 呈现为路径 tag 或图片附件，不恢复此前已被后续变更替代的附件卡片行为。
- Runtime 113 及以上继续走 additional-object bridge；macOS、Linux 和普通 Web 的入口不变。
- 回归覆盖真实 `CF_HDROP` 的有序多文件、Unicode/长路径、取消、非文件让出接收层及关停；以隔离 WebView2 宿主和独立 capture 来源补验跨进程边界，使用 Chromium 110 验证生产富文本 tag 插入、hover 和继续输入，并运行 Web 全量测试、生产构建及 Windows 编译。没有实际 WebView2 110 Runtime 时明确记录本机验证范围。
