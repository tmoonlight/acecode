# HTML 网页批注 —— WIP 对齐清单

- 日期：2026-11-22
- 对象：worktree `C:/Users/Administrator/.codex/worktrees/html-page-annotations/acecode`（分支 `codex/html-page-annotations`，`b5a2d5c4`）
- 对照基线：`docs/specs/2026-11-21-html-page-annotations-technical-design.md`（Q1–Q36 / U1–U5 / D1–D8）
- 规模：45 个跟踪文件（+4445 / −375）+ 6 个未跟踪新文件 = 51 个改动
- 已合并 `origin/master`：`0b862f32`（3 个提交：ACEModel 配额错误处理），无冲突，WIP 原样保留

---

## 0. 一句话结论

**这批 WIP 是「旧路线的完成度打磨」，不是新方案的实现。** 它把 Codex 的视觉对齐、样式面板、原生表面几何（圆角挖洞 / 模态阻断输入）、已发消息卡片都做完了；而新方案推翻的三处骨架——**记录归属上提到会话（Q33）、发送链路换骨（Q35：组装收到 composer，触发点保留三处）、截图前移到 save（D2）**——一处都还没落地。

所以对齐不是"改改就行"，而是：**几何与样式面板整块留下（约 2600 行），发送链路整条换骨（约 700 行），记录层重写（约 300 行）。**

| 判定 | 文件数 | 行数量级 | 说明 |
|---|---|---|---|
| **保留**（不改或只加注释） | 19 | ~2100 | 样式面板、表面几何、模态阻断、i18n 文案、既有测试 |
| **改造**（同一份代码改行为） | 24 | ~1800 | 浮窗、工具栏、controller、消息卡片、记录层 |
| **废弃**（删） | 8 | ~500 | `pageAnnotationBatches.*`、截图通道、旧 openspec 文档 |

---

## 1. 保留（19）

这些改动服务的能力在新方案里没变，动了只会引入回归。

| 文件 | 改动量 | 内容 | 依据 |
|---|---|---|---|
| `web/src/components/PageAnnotations.jsx`（样式面板部分） | +1024 中的大头 | 间距锁 `spacingLockState`、色板 `colorSwatchValue`、字体建议 `fontSuggestions`、下拉菜单、tooltip、透明度、`adjustBounds` 拖拽 | 与新方案无关；是「调整」按钮的全部能力，D8 还要求保留它 |
| `web/src/styles/globals.css` | +878 | 浮窗 / 工具栏 / 卡片的视觉与动效 | 同上（卡片样式部分见 §2） |
| `web/src/lib/agentBrowserSurfaceCoordinator.js` + `.test.js` | +209 / +321 | 圆角挖洞（`occlusionSquareCorners`、`occlusionSquareCornerMask`）、`overlayBleed`、合并同半径矩形 | 纯表面几何，与批注数据模型无关 |
| `src/apps/desktop/agent_browser_host.hpp` / `.cpp` | +13 / +13 | `interaction_blocked`（模态期间 `EnableWindow(FALSE)` 阻断输入）、`radius` / `square_corners` 校验 | 服务 Q10「按住时样式面板整体隐藏」与模态弹窗，属于必须能力 |
| `src/apps/desktop/agent_browser_host_mac.mm` | +110 | `add_occlusion_cornered_hole` 每角独立半径 | 同上，mac 侧对应实现 |
| `src/apps/desktop/agent_browser_windows_helpers.cpp` | +30 | Windows 圆角窗口区域 | 同上 |
| `src/apps/desktop/main.cpp` | +3 | 解析 `interaction_blocked` / `radius` / `square_corners` | 同上 |
| `web/src/components/PageAnnotationTargets.jsx` | +21 | 目标标签渲染 | 记录结构里 `targets[]` 不变 |
| `web/src/components/PageAnnotationTextField.jsx` | +15 | 多行描述输入 | Q36 `Shift+Enter` 换行要用 |
| `web/src/components/Modal.jsx` | +16 | 模态呈现 | D7 / 路径 B 的弹窗要复用它 |
| `web/src/lib/annotationChromeSuspension.js` + `.test.js`（新） | 新文件 | 浮窗/工具栏悬浮时暂停 WebUI chrome，配合 `set_modal_suspended` | Q10 的实现基础 |
| `web/src/lib/pageAnnotationMessage.js` + `.test.js`（新） | 新文件 | `parsePageAnnotationMessage` / `pageAnnotationMessageCopyText` / 截图剥离 | 消息载体沿用（§4.9），只需放宽解析（见 §2） |
| `web/src/lib/pageAnnotationStyles.browser.test.js` | +212 | 样式层测试 | 跟样式面板保留 |
| `web/src/lib/pageAnnotationText.test.js` | +10 | 文本层测试 | — |
| `web/src/lib/pageAnnotationTextField.test.js` | +6 | 输入框测试 | — |
| `web/src/lib/agentBrowserArchitecture.test.js` | +30 | 表面架构断言 | 跟几何保留 |
| `web/src/lib/composerMessageRendering.test.js` / `htmlPageFrame.test.js` / `modalPresentationArchitecture.test.js` / `sessionStartMaskArchitecture.test.js` / `themeSurfaces.test.js` | 各 +2~4 | 架构守卫 | — |

---

## 2. 改造（24）

### 2.1 记录层：`page_annotation_session.hpp`（+48）

**WIP 已经做对的**（保留）：

- 新增 `hide_markers` / `set_modal_suspended` / `discard` 三个 action 进入白名单（`:182-189`）；
- `page_background_` 字段与 `markers_hidden_` / `modal_suspended_` 状态进入 `snapshot()`（`:68-75`）；
- `discard` 清空 `records_` 并把 `next_number_` 归 1（`:385-388`）——Q22 的「手动丢弃」正好用它；
- `replace_document` 复位 `markers_hidden` / `modal_suspended`（`:149-152`）。

**必须改的**：

| 问题 | 现状 | 新方案要求 |
|---|---|---|
| 记录归属 | `records_` 是 `PageAnnotationSession` 的成员，`close_page` 即析构 | Q33 修订：上提到会话级 `SessionAnnotationStore`，关页签转 `orphan` |
| 目标有效性 | `record["valid"] = false`（布尔） | 四态 `live / replayable / stale / orphan` |
| 命令面 | 所有 action 统一 `busy_ = true` | quiet 档（`read` `preview_*` `original` `hide_markers` `set_modal_suspended` `sync`）不占闸 |
| 缺失 action | 无 `sync` / `reapply` / `delete_record` / `update_description` / `open_record_popup` | 全部要加（§4.1 表） |
| `in_flight` | 不存在 | 发送中记录移出 `records_`，不画图钉、不计 `pending_count` |

### 2.2 页内运行时：`page_annotation_runtime.hpp`（+209）

保留：整层 `hide_markers`、原图态 `markersVisible = !markersHidden && !original`、`page_background` 注入。
改造：内部编号逻辑要移出（改由 `sync` 全量下发）、新增 `sync` / `remove` / `reapply` 分支、保存时输出扩展 locator（`body>div#root>…` 之外补 testid / aria / 文本指纹）、打 `data-ace-hit-*`。

### 2.3 截图链路：`page_annotation_host_adapter.hpp`(+10) / `page_annotation_bridge.cpp`(+15)

WIP 新增了 `screenshot` 命令 + `aceDesktop_agentBrowserAnnotationScreenshot` 绑定，**时机是发送时**（工具栏还有「截取屏幕截图」按钮）。
D2 改为：`save` 时视口捕获 + 按 rect 裁剪 + 缩到 ≤512px 存 `record.thumbnail`；发送只上传。**命令保留、调用点前移、删掉工具栏截图按钮。**

### 2.4 工具栏：`PageAnnotationSendBar.jsx`（+272）

WIP 现状（`PageAnnotationSendBar.jsx:94-192`）：发送（含「发送中/重试」三态）、截取屏幕截图、隐藏标记（`aria-pressed`）、按住原图、丢弃批注、退出批注模式、拖动把手、tooltip。

改动：

- **保留「发送」按钮，换掉实现**（Q35 修订二次：组装唯一在 composer，触发三处）——按钮不再调 `pageAnnotationBatchSender`，改为调 composer 的 `submitWithAnnotations()`；连带删掉 `send()` 与 `sender` 依赖、删掉「发送中/重试」三态文案；按钮显示会话级总条数，禁用条件与输入框一致；
- **删「截取屏幕截图」按钮**（D2，图在 save 时就截好了）；
- 保留：隐藏标记（锁存）、按住原图（补 `blur` / `visibilitychange` 兜底，**删掉键盘 Space/Enter 那条路径**，Q2 修订为纯指针）、丢弃、退出、拖动、tooltip；
- 新增：`N 条待发` 计数徽标（点击把焦点交给输入框上方的卡片）。

### 2.5 浮窗：`PageAnnotations.jsx`（结构部分，非样式面板）

保留结构与样式；新增：

- **「编辑已有记录」态**：`open_record_popup` 回填 `description`、光标置末尾、`focus()`、图钉高亮、差异还原成 draft；
- `Enter` = 添加 / `Shift+Enter` 换行 / `Ctrl+Enter` = 保存当前 + 触发整批发送（U4）；
- `read` 失败**不终止**轮询（现 `:43-46` 一次 `ok:false` 就 `return`）；
- `adjustable` 单点判定（`live` 且页签存活且 `document_id` 匹配），供路径 A / 路径 B 共用。

### 2.6 controller 与消息层

| 文件 | 改动 |
|---|---|
| `web/src/lib/pageAnnotations.js`（+25） | 支持 quiet 命令并发；新增 `reapply`；新增 `open_record_popup` / `update_description` / `delete_record` 的调用封装 |
| `web/src/components/Message.jsx`（+31） | **已接好 `PageAnnotationMessageCard`**，保留；`parsePageAnnotationMessage` 放宽为「末尾 `\nPage annotations\n` 分隔段」，以便承载「用户文本 + 批注」合并消息 |
| `web/src/lib/pageAnnotationMessage.js` | 解析放宽；按 `page_url` 分组；`copyText` 保留（见下） |
| `web/src/components/PageAnnotationMessageCard.jsx`（新） | 保持只读，但行布局改为「缩略图 + tag + 描述」，缩略图取 payload 里该记录的裁剪图 |
| `web/src/i18n/sourceCatalog.generated.js` / `i18n-en-overrides.mjs` | 随文案增删同步（删「发送中/重试/截取屏幕截图」，加「页面已关闭」「回到页面修改」等） |
| `web/src/lib/runTests.js` | 去掉 `pageAnnotationBatches`，加新模块 |
| `tests/desktop/page_annotation_session_test.cpp`（+74） | 四态、分组编号、关页签转 `orphan` |
| `tests/desktop/page_annotation_lifecycle_smoke.mjs`（+29） | 同上 |

---

## 3. 废弃（8）

| 文件 | 原因 |
|---|---|
| `web/src/lib/pageAnnotationBatches.js` | 整条批次链路：`createAnnotationBatchSender`（发送状态机 + 截图 + 回执对账 + `fullPage` 断言）、`formatAnnotationBatch`、`pageAnnotationBatchSender`、`localizedSendError`、`annotationIsSendable` —— Q26 要求删净 |
| `web/src/lib/pageAnnotationBatches.test.js` | 同上 |
| `openspec/changes/add-html-page-annotations/design.md`（+187） | 描述旧路线；WIP 还在往里加「screenshot bridge / message card」小节。整个 change 标 superseded，不再维护 |
| `openspec/changes/add-html-page-annotations/tasks.md`（+67） | 同上 |
| `openspec/changes/add-html-page-annotations/specs/html-page-annotations/spec.md`（+59） | 同上 |
| （连带）`annotation_batch_id` 全部 43 处引用 | 前端、请求体、`metadata` 落库 |
| （连带）`reconcileOnly` 全部 11 处引用 | 服务端 404 协议与前端重试 |
| （连带）原生租约 `capture` / `validate` / `acknowledge` / `release` | 状态机已取消 |

**注意**：`pageAnnotationBatches.js` 的外部引用只有 5 处（`PageAnnotationSendBar.jsx`、两个测试、`pageAnnotationIntegration.browser.testHelper.js`、`runTests.js`），删除面可控。

---

## 4. 三个需要单独确认的点

1. **消息级「复制」按钮要留**。D6 说的是「卡片里不给复制按钮」，但 `Message.jsx:409` 那个是**所有消息共用的页脚操作**，且它调 `pageAnnotationMessageCopyText` 把 JSON 剥成干净文本。删了反而让用户复制到 JSON。**建议：保留，只在文档里注明"卡内无按钮"**。
2. **`annotationChromeSuspension.js` 与新方案的模态阻断是两套**（一套在 WebUI 侧暂停 chrome，一套在原生侧 `EnableWindow(FALSE)`）。两者互补，都要留，但要在文档里写清边界，否则后面有人以为重复实现。
3. **`page_annotation_bridge.cpp` 的 screenshot 通道**：可以保留同一个 binding、只把调用点从「点发送」挪到「save」。这比删掉重写省事，也避免 WebUI 侧再开一条 bridge。

---

## 5. 建议的落地顺序（每批独立可评审）

| 批次 | 内容 | 依赖 |
|---|---|---|
| **P0** | 提交当前 WIP 作为基线（或标 `wip/` 分支），让后续 diff 可读 | — |
| **P1** | 记录上提：新增 `SessionAnnotationStore`，`PageAnnotationSession` 改为转发（**行为零变化**，纯搬运） | P0 |
| **P2** | 四态 + `orphan`：`close_page` 不再析构；会话广播所有页签 | P1 |
| **P3** | 截图前移：`save` 时捕获 → `thumbnail`；删工具栏截图按钮 | P1 |
| **P4** | 发送换骨：删 batches，三触发点（输入框 / 工具栏 / `Ctrl+Enter`）收口到 `submitWithAnnotations`，用户文本合流，40s 自查 | P1 / P3 |
| **P5** | 待发卡片 + 铅笔两条路径 | P2 / P4 |
| **P6** | 消息卡片只读化 + openspec 新 change 落地 + 测试补齐 | P4 / P5 |

P1 之前的 WIP 建议**先提交**——否则记录上提的 diff 会混在 4445 行里，评审时谁也看不出哪些是搬运、哪些是新逻辑。
