## 1. Windows 原生拖放桥接

- [x] 1.1 将 Windows WebView2 SDK 固定到支持 WebMessage additional objects 与 dropped `File` path 的稳定版本。
- [x] 1.2 在 Desktop host 中接收带版本标记的 additional-object 消息，按顺序提取完整绝对路径批次，并保留接口缺失与导航拦截兜底。

## 2. 会话框与终端拖放入口

- [x] 2.1 新增可测试的 WebView2 拖放批次辅助函数，优先保留 `DataTransferItemList` 顺序并在不可用时安全失败。
- [x] 2.2 会话框成功桥接 drop 后接管默认行为，并复用现有物化逻辑生成多文件附件与文件夹 reference。
- [x] 2.3 终端复用同一批次桥接，保持既有 hover 落点隔离和完整路径注入行为。

## 3. 回归与验证

- [x] 3.1 增加单文件、文件夹形态、多文件有序批次、FileList fallback 与桥接失败的 JavaScript 回归测试。
- [x] 3.2 运行 focused Web 测试、Web 全量测试与生产构建、i18n 审计、Windows Desktop Release 编译和严格 OpenSpec 校验。
- [x] 3.3 检查 diff 与工作区基线，确认未覆盖或改变无关的既有脏改动。

## 4. WebView2 110 compatibility follow-up

- [x] 4.1 增加仅供旧 Runtime 使用的独立 Windows OLE 文件接收层，保留原生路径批次、释放坐标、非文件让出接收层与 RAII 关停。
- [x] 4.2 接入 WebHost，外部 capture 时启用、导航后更新尺寸并在 WebView 销毁前撤销自有资源；保留浏览器原目标与现代 Runtime 的 additional-object 入口。
- [x] 4.3 补充 OLE 批次、取消、失效窗口、原目标不变、窗口层级和析构清理测试，以及 Chromium 110 生产 composer 的路径 tag/继续输入回归。
- [x] 4.4 完成定向检查、Web 全量测试和构建、Windows 编译、严格 OpenSpec 校验与 diff 审查，记录实际 Runtime 验证范围。

### 2026-10-30 verification

- `WindowsFileDrop*` 8 项本机原生测试通过：有序 Unicode/长路径与文件夹、一次批次、取消、非文件让出接收层、关闭后的迟到调用、原目标注册不变、自有接收层的窗口层级与焦点、失效窗口。
- 生产 InputBar 浏览器夹具在 Chromium `110.0.5481.38` 和 `145.0.7632.6` 各 22 项通过，包括缺少 additional objects 的路径 tag、native hover、立即输入、终端/其他控件隔离和原有 paste/undo/异步事务回归。浏览器夹具的原生物化器为桩，不声称是实际 WebView2 110 宿主实测。
- 标准 CMake `windows_file_drop_smoke` target 构建并运行通过，输出 `external_capture=1 hidden_after_release=1 ok=1`。它使用本机真实 WebView2 与独立自有 capture 来源，验证跨进程启用/隐藏，不读取或替换浏览器进程中的 OLE 指针。
- `pnpm test`、`pnpm build`（含正则兼容检查）、Windows Desktop Release 完整增量构建通过；产物位于 `build/windows-x64-desktop-release/acecode-desktop.exe`。
- 严格 OpenSpec 校验、包含新增未跟踪源文件的严格 layer lint、`git diff --check` 通过；未提交或发布。
- 验证边界：本机未安装实际 WebView2 110 Runtime，尚未在该宿主中完成 Explorer 物理拖放实测。
