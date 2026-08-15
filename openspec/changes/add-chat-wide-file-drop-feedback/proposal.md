## Why

ACECode 当前只有文件进入输入卡片时才显示拖放反馈，用户把文件拖到会话正文或标题区域时无法确认能否释放；同时连续附件 tag 之间仅有 1px 尾距，视觉上容易粘连。需要把整个聊天主栏变成统一落点，并让落入输入框后的附件保持清晰、可辨的间距。

## What Changes

- 文件进入当前聊天主栏任意位置时，显示覆盖标题、正文和输入区的轻度虚化/降对比反馈，并在中央提示“松开即可添加”。
- 文件在聊天主栏内移动时持续保持反馈；离开、取消或完成释放后立即恢复正常界面。
- 在聊天主栏任意位置释放文件时，复用现有输入框附件入口，把文件加入当前 composer；保留浏览器上传、Windows WebView2 原生路径桥接和 macOS 原生拖放行为。
- Desktop 中的文件第一次进入聊天主栏时，仅请求一次无置顶兜底的窗口前置；释放文件后只恢复 composer 光标，不再延迟请求原生前台。
- 同一文件在单次拖入或当前 composer 中重复出现时只保留一个附件；移除附件或上传失败后允许再次添加。
- 连续附件 tag 使用明确的水平 gutter 和轻微垂直间距，换行时不再彼此贴合。

## Capabilities

### New Capabilities

- `chat-wide-file-drop-feedback`: 规定聊天主栏的文件拖放命中范围、反馈生命周期、释放路由与附件 tag 间距。

### Modified Capabilities

无。

## Impact

- 影响 `web/src/components/ChatView.jsx`、`web/src/components/InputBar.jsx`、`web/src/lib/composerCaretRestore.js`、`web/src/lib/composerFileTransfer.js`、`web/src/styles/globals.css` 与 `src/desktop/main.cpp`。
- 增加前端纯函数和架构回归测试，锁定聊天区级拖放事件、一次性窗口前置、附件去重、遮罩提示、现有文件处理链路复用和附件 tag gutter。
- 不改变附件上传 API、附件持久化结构、Provider 序列化语义或文件大小限制。
