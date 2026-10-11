## Context

目前 Windows 的 WM_ACTIVATE 只发送网页事件，没有调用 WebView2 MoveFocus；macOS/Linux 的 set_window_focus_handler 为空实现。前端通过多次未托管的延迟调用恢复 Slate 焦点，并额外保留终端焦点。

## Goals / Non-Goals

目标：让原生激活、网页选择和实际键盘输入一致；保留草稿、选区以及 Slate 等待 DOM 更新的保护。
不涉及：消息发送、布局、浏览器普通网页的自动聚焦策略、发布或替换运行中的用户程序。

## Decisions

1. 增加仅聚焦当前活动窗口 WebView 的 bridge。Windows 使用 MoveFocus，macOS 使用 makeFirstResponder，Linux 使用 gtk_widget_grab_focus。先由网页确认聊天可用，再调用原生层，成功后恢复输入。不同于现有拉前窗口功能，此入口不得激活后台窗口。
2. 各平台接通原生窗口激活通知，最大化/恢复状态变化也发送同一通知。回调沿用 GUI 线程模型；macOS 观察者析构退订，Linux 信号随窗口销毁。
3. 将窗口激活后的帧调度集中在 composerCaretRestore，合并重复通知；失焦、隐藏、卸载或用户开始其他交互后撤销待执行恢复。ChatView 直接提供聚焦回调，不再叠加无人管理的重试。
4. 按本次要求移除终端焦点例外。保留设置、提问和模态框等阻塞表面的焦点管理，它们打开时底层聊天不是可用输入区域。
5. 三平台原生聚焦和观察者资源管理收敛到 `web_host_focus.hpp`；WebHost 保留窗口事件转发。文件拖放复用同一活动 WebView 聚焦入口，保持入口文件不超过分层行数上限。

## Risks / Trade-offs

### 2026-10-12 中文输入回归

在当前 Windows 的真实 WebView2 中复现：`Input.imeSetComposition` 已启动中文合成时，窗口激活通知仍调用原生 `MoveFocus`，合成文本被提前提交；随后正常提交再次插入相同文字。普通 Chromium 模拟的网页焦点没有此行为。现有恢复只由 pointerdown / keydown 撤销，遗漏 compositionstart、beforeinput 和 input，并且新激活通知仍可在合成中进入原生桥。

修复在自动恢复调度器记录合成状态：合成开始、文本输入与合成结束均撤销过期任务，合成中拒绝新恢复；页面已经获得真实焦点时不重复调用原生 MoveFocus，仅恢复聊天元素。RichComposer 的所有命令式聚焦入口在 active / settling 阶段直接退出，避免其他恢复入口改动原生候选词及选区；未渲染的 React 更新也延迟聚焦。保留旧 DOM 光标采纳和未渲染操作保护，不重建编辑器。以原生 WebView2 的合成中间状态、最终文字以及 DOM / Slate 光标偏移一致作为验收依据。

- 原生和 DOM 通知可能重入 → 合并在途请求，回调完成后复核页面状态。
- 快速切走或打开弹框 → 在每次恢复前校验，取消过期异步工作。
- 原生 WebView 获焦与 Slate 内部状态可能不同步 → 使用真实编辑器验证实际键盘输入和选区；保留已有 Slate 防护。
- 当前验收机器为 Windows → 优先完成原生 Windows 验证，WebKit 自动化与 macOS/Linux 真机结果分别记录。

## Migration Plan

无配置或数据迁移。前端兼容缺少新 bridge 的旧桌面壳；回滚本次源文件即可恢复原行为。
