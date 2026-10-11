# 窗口激活后的聊天焦点验收

日期：2026-10-11。当前 master 工作区；保留原有未提交改动，未提交、推送、发布或替换运行中的 Desktop。

## 实现

- Windows 继续接收 WM_ACTIVATE，并补齐最大化/还原通知；前端请求经过活动窗口校验后，WebView2 MoveFocus 交还系统键盘焦点。
- macOS 补齐窗口成为 key、最小化恢复和全屏/最大化状态通知，使用 makeFirstResponder；Linux 接通 focus-in-event、窗口状态通知并使用 gtk_widget_grab_focus。
- 前端统一管理恢复请求、帧调度和有限重试。终端不再阻止聊天恢复；原生通知先于页面可见时保留意图。失焦、隐藏、卸载或用户点击/输入会取消旧请求，阻塞弹框维持焦点保护。
- 复用 Slate 已有选区和 DOM 就绪保护，并将取消条件传入其延迟重试。

## 已通过

- `node web/src/lib/composerCaretRestore.test.js`：覆盖原生桥成功/拒绝/失败、通知合并、激活早于可见、终端切回、弹框、失焦、交互和卸载取消。
- `node web/src/lib/richComposerFocus.test.js`：7 项，包含 Slate 待提交时撤销恢复。
- `pnpm test`、`pnpm build`：完整前端测试与生产构建通过；旧 WebView 正则兼容扫描通过。
- `web/scripts/test-desktop-focus.mjs`：Chromium 与 Playwright WebKit 均通过真实编辑器键盘输入、原生桥重入去重、终端切回、草稿选区恢复、弹框保护和卸载检查。未发现页面运行错误。
- Windows MSVC Release/x64 定向编译：本次 `web_host.cpp`、`main.cpp` 成功；独立原生测试宿主链接成功。
- `python -B scripts/layers/check_layers.py --repo . --layout final --strict`、OpenSpec strict、定向 `git diff --check` 通过；Impeccable detector 结果 `[]`。分层检查使用包含新增文件的临时 Git 索引，实际用户索引未改变。

## 原生实测限制

独立宿主 `tests/desktop/fixtures/window_focus_host.cpp` 使用生产 WebHost、生产 RichComposer 和 Win32 SendInput，检查窗口切换、最小化恢复、最大化/还原、隐藏恢复、后台拒绝和选区/弹框保护。

本次运行环境的 `query session` 显示 shao 会话 2 为 Disc；当前线程桌面为 WinSta0/Default，输入桌面不可访问，`GetForegroundWindow()` 返回空。测试不能获得真实前台或发送可验证键盘输入，最终以退出码 77 明确跳过。此前诊断运行中后台聚焦入口返回 false，与保护约定一致。**Windows 真正前台键盘恢复仍需在连接、解锁的桌面补验，不宣称已通过。**

已有静态库混用编译器版本导致独立宿主第一次链接出现 `__std_find_end_1` / `__std_mismatch_*`。在临时目录以同一 MSVC 重编译所需的三个 helper 对象后链接通过；未清理或覆盖用户构建目录。

初次分层检查发现入口文件超过行数限制，随后将原生聚焦与观察者提取到 `web_host_focus.hpp`，保持原限制不变。已有文件拖放架构测试同步追踪新的公共聚焦路径与后台保护，最终分层检查为零问题。

macOS、Linux 已完成实现及源码审查，未在对应操作系统编译或真机实测；Windows 上的 Playwright WebKit 不等同于原生 WKWebView。

## 复测入口

```powershell
$env:ACE_PLAYWRIGHT_MODULE = 'C:/Users/shao/AppData/Local/Python/pythoncore-3.14-64/Lib/site-packages/playwright/driver/package/index.mjs'
node web/scripts/test-desktop-focus.mjs

# 在连接且解锁的 Windows 桌面运行；原生宿主需链接当前 web_host.cpp 和 desktop support。
$env:ACE_FOCUS_NATIVE_HOST = "$env:TEMP/ace-focus-native/window_focus_host.exe"
node web/scripts/test-desktop-focus.mjs
```

本次编译命令和日志位于 `%TEMP%/ace-focus-native/compile.cmd`、`link.cmd`、`compile.log`、`link.log`、`smoke.log`；前端完整日志位于 `%TEMP%/ace-focus-web-tests.log` 和 `ace-focus-web-build.log`。
