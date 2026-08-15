## Context

`InputBar` 目前同时拥有拖拽深度计数、浏览器 `FileList` 解析、URI 路径物化、Windows WebView2 additional objects 桥接、macOS 原生拖放放行和附件上传入口，但 React 的 `onDrag*` 只绑定在 `.ace-composer-card`。因此处理链路本身完整，命中范围和反馈层级却被限制在输入卡片。

聊天的空态与已有会话分别由 `ChatView` 的首页主栏和会话主栏承载；右侧预览面板与侧边栏不是 composer 的语义落点。附件进入 Slate 后渲染为 `.ace-cmd-token.ace-slate-inline-tag`，当前共享徽标只有 1px `margin-right`，连续附件会视觉粘连。

## Goals / Non-Goals

**Goals:**

- 让空态和已有会话的整个聊天主栏都能接收外部文件拖放。
- 复用 `InputBar` 唯一的文件解析、平台桥接和上传链路，不在 `ChatView` 重写文件处理。
- 用 Codex 式轻度虚化/降对比遮罩和中央提示明确表示当前可释放。
- 让拖放反馈在进入、跨子元素移动、离开、取消和释放时保持正确生命周期。
- 让 Desktop 窗口在文件首次进入时立即尝试前置，同时避免释放后的抢焦点和任务栏闪烁。
- 同一文件在一批文件和当前 composer 中只产生一个附件 tag 与一次上传。
- 仅调整 composer 内联 tag 的间距，不改变已发送消息徽标的布局。

**Non-Goals:**

- 不把侧边栏、右侧预览、控制台或模态框变成附件落点。
- 不改变附件上传 API、持久化结构、大小限制、图片处理、路径物化或 Provider 序列化。
- 不重设计附件 tag 的颜色、图标、删除按钮或预览交互。

## Decisions

### 1. `ChatView` 定义命中范围，`InputBar` 保留文件处理所有权

`ChatView` 在空态根节点和已有会话的主栏根节点绑定 `dragenter`、`dragover`、`dragleave`、`drop`，并通过 `inputRef` 调用 `InputBar` 暴露的文件拖放方法。`InputBar` 继续维护拖拽深度、平台分支、路径物化和 `onMediaFiles` 调用，同时通过状态回调通知 `ChatView` 是否显示全栏反馈。

没有把上传逻辑上移到 `ChatView`，因为这会复制 WebView2 与 WKWebView 的细节；也没有用 document/window 级落点，因为那会错误覆盖侧边栏和预览面板。`InputBar` 在没有外部命中范围时保留输入卡片级回退，避免组件被独立复用时失去拖放能力。

### 2. 使用独立、无指针命中的遮罩节点

主栏激活时渲染绝对定位遮罩，半透明主题背景负责基础降对比；支持 `backdrop-filter` 的环境再叠加轻度模糊与去饱和。遮罩 `pointer-events: none`，因此底层主栏继续接收完整拖放事件。中央提示使用现有 surface、border、foreground 与 shadow 变量，文案固定为“松开即可添加”。

没有直接给主栏内容设置 `filter: blur(...)`，因为这会创建新的绘制/堆叠上下文，并容易连同提示一起模糊；独立遮罩能让提示始终清晰。遮罩只覆盖聊天主栏，不跨到右侧预览。

### 3. 继续使用深度计数稳定跨子元素拖动

拖拽进入主栏的每个子元素会产生配对的 enter/leave 事件，`InputBar` 继续用深度计数判断真正离开范围。状态更新封装为单一函数，同时驱动输入卡片边框与 `ChatView` 遮罩；window 的 `dragend`、`drop`、`blur` 后备清理继续防止遮罩滞留。

非文件拖拽和 disabled/read-only composer 不激活落点，也不截获默认行为。

### 4. gutter 只作用于 Slate 内联 tag

使用组合选择器 `.ace-cmd-token.ace-slate-inline-tag` 覆盖共享徽标的 1px 尾距，设置 5px 水平尾距和 1px 上下间距。这样连续附件与换行后的多行 tag 都有呼吸空间，而已发送消息里的 `.ace-cmd-token` 保持原布局。

### 5. 用架构测试锁定事件边界与视觉合同

新增静态架构测试，确认 `ChatView` 主栏委托到 `InputBar`、两种聊天状态都渲染同一遮罩、`InputBar` 仍包含原有 Windows/路径处理入口，以及 CSS 包含主题遮罩、backdrop blur 和 scoped gutter。现有文件传输纯函数测试继续覆盖数据解析。

### 6. 拖入前置与落下后的光标恢复分离

`InputBar` 只在文件拖拽深度从 0 变为 1 时调用新的 Desktop 文件拖入激活桥接。原生桥接显示并尝试前置主窗口，但不复用通知/托盘唤醒中的临时 `TOPMOST` 兜底；同一次拖拽中的嵌套 `dragenter` 与持续 `dragover` 不重复请求。拖拽真正离开、取消或释放后，深度归零，下一次独立拖入才可再次请求。

文件落下或通过原生路径物化加入附件时仍恢复 composer 的 DOM 光标，但该恢复不再调用通用 `aceDesktop_focusWindow`。能力菜单等明确由当前窗口内操作触发的现有光标恢复继续保留原生窗口唤醒语义。这样前置发生在用户仍把指针停留于目标窗口的时刻，附件上传和布局更新不会在稍后再次改变 Windows 前台状态。

### 7. 用稳定附件身份预留去重

文件身份优先采用规范化的绝对来源路径；Windows 盘符路径和 UNC 路径统一分隔符并按大小写不敏感比较。浏览器拿不到来源路径时，回退到文件名、大小、MIME 类型与 `lastModified` 组成的签名。

`ChatView` 在创建会话或开始异步上传前同步预留身份，同一批次内和 composer 已预留身份中的重复文件直接跳过，因此不会创建重复 tag 或发起重复上传。预留与本地附件 ID 绑定：移除、上传失败或清空 composer 时释放；上传成功后保留到附件被移除或消息提交清空。不会把去重状态写入服务端附件结构。

## Risks / Trade-offs

- [React 子元素切换产生多次 enter/leave] → 保留既有拖拽深度计数，并只在计数归零时关闭反馈。
- [浏览器不支持 `backdrop-filter`] → 半透明主题背景仍提供完整降对比反馈，模糊仅作为渐进增强。
- [遮罩阻断 drop] → 遮罩固定使用 `pointer-events: none`，事件由主栏根节点接收。
- [主栏切换或窗口失焦后反馈残留] → 组件卸载和 window `dragend`、`drop`、`blur` 都清理本地与上层状态。
- [扩大范围后误截获文本拖拽] → 继续使用 `hasFileTransfer` gate，只接受文件型 DataTransfer。
- [嵌套 dragenter 重复请求前台] → 仅在拖拽深度首次从 0 进入时调用一次桥接，`dragover` 不调用。
- [Windows 拒绝跨进程抢前台后闪烁] → 文件拖入桥接不使用临时置顶兜底，且附件落下后不再请求原生前台。
- [异步创建会话期间重复拖入] → 在创建会话和上传之前同步预留附件身份，失败时释放预留。
- [浏览器不暴露本地路径] → 使用文件名、大小、类型和修改时间组成回退签名；路径可用时始终优先使用路径。

## Migration Plan

无需数据迁移。前端部署后立即启用新的命中范围；回滚时恢复 `ChatView` 事件绑定、`InputBar` 本地绑定和新增样式即可，附件数据与后端保持兼容。

## Open Questions

无。
