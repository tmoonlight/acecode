## Why

现有的 `add-html-page-annotations` 变更按「预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认」实现，已经跑通了一部分（#194 合入，#195–201 散在多个分支）。它把批注记录绑在页签上、把工具栏发送做成与输入框发送并列的另一套链路，导致三件事做不到：

1. 关掉页签，用户写到一半的批注连同描述一起消失；
2. 两个发送按钮各发各的：工具栏只发页面批注、看不见用户刚敲的话，输入框又不知道有批注在等；
3. 发送时才发现截图失败（mac 视口外），或者 40 秒没等到确认就回滚，用户重发一次就变成两条一样的批注，agent 把同一处改两遍。

本变更把这三点从根上改掉，其余能力（页内预览、按住看原图、隐藏标记、刷新重放、源文件定位）沿用。

## What Changes

- **记录权威上提**：`records` / `next_number` 从 `PageAnnotationSession`（页签级）挪到会话级 `SessionAnnotationStore`；页签降级为渲染与编辑的端点。关页签 → 记录转 `orphan`，**不销毁**。
- **记录四态**：`valid: bool` 换成 `live / replayable / stale / orphan`，每态的可发送、可画图钉、可重放、可调整都明确。
- **发送组单一、触发多路**：消息只在 composer 组装一次（用户敲的字与整批批注**合并成一条消息**）；输入框发送、页内工具栏发送、浮窗 `Ctrl+Enter` 三处都调同一个提交动作，效果等价于「把聊天框里的内容和所有批注批量提交」。
- **待发批注卡片**：挂在输入框上方，跨页签聚合，逐条可编辑（回到页面浮窗 / 无页签时弹窗）可删除。
- **截图前移**：从「发送时一次整页捕获 + 逐条裁剪」改为「保存批注时按 rect 裁剪，存 `record.thumbnail`」，发送只上传。
- **超时自查**：40s 没等到 WS 确认时，先按 `client_message_id` 查一次是否已落库，落库则正常收尾，没落才回滚。
- **删净批次链路**：`annotation_batch_id`、`reconcileOnly`、`annotation_batch_not_accepted`、原生租约（`capture`/`validate`/`acknowledge`/`release`）、`pageAnnotationBatches.js`。

## Impact

- 原生：`page_annotation_session.hpp`（重写记录层）、新增 `session_annotation_store.hpp`、`agent_browser_host_*`（`close_page` 不再析构记录、状态事件改为广播会话内所有页签）、`page_annotation_host_adapter.hpp`（截图命令调用点前移）。
- 前端：`PageAnnotationSendBar.jsx`（去发送/截图按钮）、`PageAnnotations.jsx`（编辑已有记录态）、新增 `PageAnnotationPendingCard.jsx` / `PageAnnotationEditDialog.jsx` / `pageAnnotationPendingStore.js`、删除 `pageAnnotationBatches.js`。
- 服务端：`routes_sessions.cpp` 删 `annotation_batch_id` 校验与 404 分支；新增按 `client_message_id` 查询消息的接口（读 `metadata`，不需要新列）。
- 契约：消息 `text` 允许「用户文本 + `\nPage annotations\n` + JSON」形态，解析层从 `startsWith` 放宽为末尾分隔段；`composer_content` 封闭枚举**不动**。
- 取代：`openspec/changes/add-html-page-annotations/`（标记 superseded，不再维护）。
