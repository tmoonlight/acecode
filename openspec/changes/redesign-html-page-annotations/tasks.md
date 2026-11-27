## 交付

> 顺序即依赖顺序。P1 必须行为零变化——它是纯搬运，任何行为差异都算 bug。

- [x] **P0 基线**：把当前 WIP（51 个改动）提交为一条 `wip:` 提交，让后续每批的 diff 可读。
  - 54 个改动落成两条提交：`9da82a61 docs: 批注重设计文档基线` + `44c3fad4 wip: codex visual parity, style panel, native overlay geometry`
- [x] **P1 记录上提（纯搬运）**
  - [x] 新增 `src/apps/desktop/session_annotation_store.hpp`：`records` / `in_flight` / 按 `page_id` 分组的 `next_number`
  - [x] `page_annotation_session.hpp` 的 `records_` / `next_number_` 改为转发到 store
  - [x] 既有 49 条批注测试全绿（行为不变）+ 新增 5 条 store 契约测试
  - 偏差：记录标识按 #208 结论改为 `<page_id>:annotation:<n>`（原计划留给后续，但那会让 P1 之后再改一次主键，索性一次到位）。`locator` 生成仍留给 #210/#217。
  - 未做：host 层按 `owner.session_id` 共享 store —— 属 P2（#211 关页签不丢），此处每个页签仍自建。
- [x] **P2 四态与 orphan**
  - [x] 记录有效性按 `live / replayable / stale / orphan` 语义落地
  - [x] `close_page` 不再析构记录，记录留在会话级 store 并广播
  - [x] 状态事件广播会话内所有页签的 `annotations`
  - [x] `discard` 只清当前页签分组，不动别的页签
- [x] **P3 截图前移**
  - [x] `save` 时视口捕获 + 按 rect 裁剪 + 缩到 ≤512px → `record.thumbnail`
  - [x] 删 `captureBeyondViewport` / `cssContentSize` 整页路径
  - [x] 删工具栏「截取屏幕截图」按钮与 `send()` 里的截图步骤
  - [x] 视口外目标退化为标签 + 文本片段
- [x] **P4 发送换骨**
  - [x] 删 `web/src/lib/pageAnnotationBatches.js` 及其测试，清 `annotation_batch_id` / `reconcileOnly`
  - [x] 服务端删 `annotation_batch_id` 校验与 `annotation_batch_not_accepted` 404 分支
  - [x] 删原生租约 `capture` / `validate` / `acknowledge` / `release`
  - [x] 工具栏发送按钮改为调 `submitWithAnnotations()`（保留按钮，删三态）
  - [x] 工具栏「按住看原图」只留指针路径 + `blur` / `visibilitychange` 兜底（D-8）
  - [x] 用户文本 + 批注合并一条消息；批注结构走 `contexts` 的 `page_annotations` 上下文，正文只保留自然语言
  - [x] 已发卡片只从 `page_annotations_context` 部件渲染，不解析正文内嵌载荷
  - [x] 40s 未确认 → 按 `client_message_id` 自查再决定回滚
- [x] **P5 待发卡片与逐条编辑**
  - [x] 会话级待发集合由会话 store 供给，关页签后仍可编辑/删除
  - [x] `PageAnnotationPendingCard.jsx`：缩略图 + tag + 铅笔 + 垃圾桶，5 行封顶内部滚动
  - [x] `PageAnnotationEditDialog.jsx`：页面已关闭时只改文本，无「调整」
  - [x] `open_record_popup` / `edit_record` / `remove` 三个 action
  - [x] 删除后提供撤销窗口
- [x] **P6 收口**
  - [x] `PageAnnotationMessageCard.jsx` 只读化，行布局改为缩略图 + tag + 描述
  - [x] `read` 失败不终止轮询；浮窗 `Enter` / `Shift+Enter` / `Ctrl+Enter` 语义
  - [x] 旧 change `add-html-page-annotations` 标 superseded
  - [x] 测试矩阵补齐（四态、orphan 发送、合并消息解析、超时自查三分支）
  - [x] 发送链路修复：三入口等待同一提交的真实结果；正文不再被空 composer_content 覆盖；失败/未知保留待发记录；浮窗保存后先把新记录同步进会话待发箱；成功后同步清理页内图钉
  - [x] 发送成功后编号按存活记录重算：清空记录后下一次批注从 1 开始，部分删除时从当前最大存活编号继续
  - [x] 已发卡片悬停明细修复：卡片本身不再裁切浮层，圆角裁切只作用于页面截屏
  - [x] 文档刷新恢复：`stale_document` 与 `replaced_document` 统一回收旧代；旧快照的 start 不再卡住，校验失败的 start 不再预分配 document id
  - [x] 铅笔路径按 `page_id` / `url` 定位未聚焦页签，先激活再等待新面板装载后编辑；只有页签确实不存在或目标失效才进入无页签弹窗
  - [x] 无页签弹窗描述保持可编辑，按钮在「编辑 / 保存」间切换，保存后写回待发卡片并停留在弹窗
  - [x] 页内工具栏按当前页签统计待发数，待发卡片仍按会话总数聚合
  - [x] 新建元素批注和从待发卡片进入编辑后，焦点落到批注描述框
  - [x] 页内批注浮层改用矩形 native 挖洞 + 页面背景色衬底，圆角由 Chromium 抗锯齿绘制，消除黑底锯齿

## PR #202 评审修复

- [x] 恢复 `external/cpp-mcp` 指针到 `62a8c88`
- [x] 删除导航围栏，移除发送后 `remove` 的拦截，并恢复 modal suspension 下的轮询边界
- [x] 批注截图改为视口捕获，校验返回 `{valid:true}`，截图命令单独放宽大小上限并补测试
- [x] macOS 源列表补入 `agent_browser_host_common.cpp`
- [x] 铅笔编辑清理生命周期状态，按运行时目标引用定位，保存时替换旧图钉
- [x] 待发删除立即生效且支持单槽撤销；超时先按 `client_message_id` 自查，重试复用同一 ID
- [x] 服务端净化并限量 `page_annotations`，分段展开进模型正文，持久化 `page_annotations_context`
- [x] 更新 API/设计文档，注册遗漏测试，删除旧正文解析、缺失 helper 依赖和无关改动
- [x] Windows C++ 定向测试、完整 Web 测试和 Web build 收口
- [ ] Windows 完整 CTest 收口
- [ ] macOS 编译及 Windows/macOS Desktop 实机验收
