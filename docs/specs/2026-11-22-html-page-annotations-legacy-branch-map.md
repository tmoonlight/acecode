# HTML 网页批注 —— 已实现未合并代码 × 旧 spec 映射

- 日期：2026-11-22
- 目的：回答「这批已写、未进主干的批注代码，对应哪份 spec、由哪个分支/文件实现、新方案下留还是弃」
- 旧 spec：`openspec/changes/add-html-page-annotations/specs/html-page-annotations/spec.md`（11 条 Requirement，已标 Superseded）
- 新方案：`openspec/changes/redesign-html-page-annotations/`（10 条 Requirement）
- 对照基线：`docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`

---

## 0. 先纠正一个前提

**主干里一行批注代码都没有。** 实测：

| 检查 | 结果 |
|---|---|
| `origin/master`（`0b862f32`）中 `page_annotation*` / `pageAnnotation*` 文件数 | **0** |
| `origin/master` 中 `openspec/changes/add-html-page-annotations/` | **不存在** |
| 当前分支 `codex/html-page-annotations`（`b5a2d5c4`）相对 master | **ahead 30 / behind 0** |
| 该分支中批注相关文件 | 29 个（另加工作区 4 个未跟踪模块） |

所以「#194 已合入 master」只成立于 PR 层面的讨论，代码层面 master 是干净的。**整条批注功能是一次性、待合并的 30 个提交 + 51 个未提交改动。**

## 1. 分支拓扑：十个分支是同一份代码的递进快照，不是可分别合并的十条线

十个分支全部从 `0155a863` 拉出，**内容上层层累积**：194 是地基，195/196/197 是三条互斥的能力增量，199 接发送链路，200 把 194–199 合流，201 再加截图布局，`annotation-send-seam-fixes` 是最终快照。

| 分支 | commit | 相对 194 的增量 | 主题 | 与当前 HEAD 的关系 |
|---|---|---|---|---|
| `…-194` | `c4e9a59a` | —（基线 +1680） | 工具栏入口、元素单击开卡、isolated world 注入、session 雏形 | 内容已吸收 |
| `…-195` | `a89c3338` | +1170 / −247 | 样式面板 + `page_annotation_style_runtime.hpp`(213) + popup 测试(163) | 内容已吸收 |
| `…-196` | `26917e51` | +727 / −242 | 文本预览 + `page_annotation_text_runtime.hpp`(148) + text smoke | 内容已吸收 |
| `…-197` | `3fa087b6` | +610 / −302 | 多目标 + 单区域 + `page_annotation_targets.hpp`(44) | 内容已吸收 |
| `…-199` | `f28429c6` | +2365 / −240 | 发送链路：`agent_browser_capture.hpp`(205)、`pageAnnotationBatches.js`(148)、服务端提交协议 | 内容已吸收 |
| `…-200` | `d4347848` | +7209 / −805 | 导航/轮询/host 适配器拆分（含 195–199 全部） | 内容已吸收 |
| `…-201` | `5752ff65` | +8815 / −827 | 截图布局、集成 driver、租约测试（201 ⊃ 200） | 内容已吸收 |
| `annotation-receiver` | `7a172eb5` | +793 / −1（纯服务端） | 服务端原子接收回执，无前端改动 | 内容已吸收 |
| `annotation-send-seam-fixes` | `d298130f` | +10398 / −348 | **全量最大快照**：capture layout(208)、session 测试(806)、回执分类 | 核心文件逐字一致 |
| `html-annotations-native-boundaries` | `c1b57f80` | +3701 / −277 | 原生捕获边界抽取 + standards gate | 内容已吸收 |

验证方式：
- `git diff --numstat` 显示 194→200 的增量覆盖了 195/196/197/199 的全部产物，201 ⊃ 200，`annotation-send-seam-fixes` ⊃ 201；
- `git diff HEAD <branch> -- <核心文件>` 对 `page_annotation_session.hpp`、`page_annotation_host_adapter.hpp`、`page_annotation_poller.hpp`、`page_annotation_navigation.hpp`、`pageAnnotationBatches.js`、`pageAnnotations.test.js`、`page_annotation_session_test.cpp` 全部返回空；
- `comm` 比对显示这些分支**没有任何 HEAD 缺失的批注文件**。

**结论：十个分支都已经在内容上并入 `codex/html-page-annotations`，没有一条还有「待合并的独立工作」。** 它们只能当「某阶段代码长什么样」的只读参照；真正待处理的是当前分支那 30 个提交 + 51 个未提交改动。

## 2. 旧 spec 11 条 Requirement × 实现 × 新方案判定

判定口径：**保留**＝新方案下行为不变；**改造**＝代码留、行为改；**废弃**＝删净。

| # | 旧 Requirement | 主要实现（文件 / 行数） | 判定 | 新 spec 对应 | ticket |
|---|---|---|---|---|---|
| R1 | Desktop page annotation controls | `page_annotation_bridge.cpp`(30)、`agent_browser_host.cpp` 注入、`PageAnnotations.jsx`、`AgentBrowserPanel.jsx` | **保留**（入口形态不变）；工具栏按钮清单要改 | 「发送组单一、触发多路」 | #213 |
| R2 | Pointer-accurate annotation surface | `page_annotation_runtime.hpp` hover 段、`page_annotation_style_runtime.hpp`(213)、`pageAnnotationPopup.test.js`(163)、**工作区** `PageAnnotations.jsx` +1024 / `globals.css` +878 | **保留**（样式面板整块留，D8 要求）；hover 需在 hidden/original 态抑制 | 「按住看原图只认指针」 | #220 |
| R3 | Reversible actual-change previews | `page_annotation_runtime.hpp` preview 段、`page_annotation_text_runtime.hpp`(148)、`PageAnnotationTextField.jsx` | **保留**；`original` 态改纯指针，删键盘路径 | 同上 | #220 |
| R4 | Multiple targets and one region | `page_annotation_targets.hpp`(44)、`PageAnnotationTargets.jsx`(26)、runtime +132/−25 | **改造**：多目标/单区域留；「已保存不可编辑不可删」被 Q36 推翻 | 「逐条编辑批注」 | #217 #218 |
| R5 | Acknowledged conversation batches | `pageAnnotationBatches.js`(148)、`page_annotation_submission.hpp`(47)、`agent_browser_capture.hpp`(205)、`annotation_submission_request.hpp`(55)、`routes_sessions.cpp`、`domain/session/annotation_submission.hpp`、agent worker/queue、`page_annotation_poller.hpp`(89) | **废弃**（整条链路）；仅 `client_message_id` 自查可留 | 「发送超时自查」 | #214 #215 |
| R6 | Page lifecycle and invalidation safety | `page_annotation_navigation.hpp`(88)、`page_annotation_host_navigation.hpp`(101)、`page_annotation_requests.hpp`(61)、`page_annotation_navigation_test.cpp`(250) | **改造**：导航提示留；`valid:bool` → 四态；关页签转 orphan | 「记录状态」「关闭页签不丢批注」 | #209 #211 |
| R7 | Native overlay edge fidelity | **工作区** `agent_browser_host.cpp` +13、`_mac.mm` +110、`windows_helpers.cpp` +30、`agentBrowserSurfaceCoordinator.js` +209；`html-annotations-native-boundaries` 分支的边界抽取 | **保留**（纯几何） | 无对应条款（隐含能力） | — |
| R8 | Layered modal suspension / native input blocking | **工作区** `interaction_blocked` → `EnableWindow(FALSE)`、`annotationChromeSuspension.js`（新）、`Modal.jsx` +16 | **保留**（两套互补，见 wip-alignment §4.2） | 无对应条款 | — |
| R9 | Send feedback and error localisation | `localizedSendError`（在 batches 内）、i18n `sourceCatalog.generated.js`、`请先选择对话` 内联 | **改造**：错误映射要重挂到 `submitWithAnnotations`；「截图正在进行中」失效；「已复制截图」随按钮删 | 「发送组单一、触发多路」 | #213 |
| R10 | Annotation message presentation | **工作区** `PageAnnotationMessageCard.jsx`（新）、`pageAnnotationMessage.js`（新）、`Message.jsx` +31 | **改造**：卡片只读化；解析放宽为末尾分隔段；修 `Message.jsx:369-376` 吞文本 | 「已发消息只读」 | #219 |
| R11 | Acceptance and compatibility | `html-page-annotations-acceptance.md`、`capture_test.cpp`(272)、`native_boundaries_test.cpp`(198)、`navigation_test.cpp`(250)、`session_test.cpp`(531)、`integration_driver.cpp` | **改造**：框架与运行器留，用例按新流程重写 | — | #208 |

### 汇总

| 判定 | 条数 | 说明 |
|---|---|---|
| 保留 | 5（R1/R2/R3/R7/R8；R2、R3 有局部改动） | 样式面板、表面几何、模态阻断、预览能力 |
| 改造 | 5（R4/R6/R9/R10/R11） | 幅度从「加四态」到「换错误挂点」不等 |
| 废弃 | 1（R5） | 整条批次/回执/租约链路，约 700 行 + 服务端 3 文件 |

## 3. 只在未提交工作区里、分支上拿不到的

这几块是 201 之后才做的打磨，**十个分支里都没有**：

| 文件 | 内容 | 为什么重要 |
|---|---|---|
| `PageAnnotations.jsx` +1024 | 间距锁、色板、字体建议、拖拽 `adjustBounds`、透明度 | 样式面板的完整实现，D8 要求保留 |
| `globals.css` +878 | 浮窗 / 工具栏 / 卡片视觉与动效 | 同上 |
| `agentBrowserSurfaceCoordinator.js` +209 | `occlusionSquareCorners`、`overlayBleed`、同半径合并 | R7 的实际实现 |
| `annotationChromeSuspension.js`（新） | WebUI 侧 chrome 暂停 | R8 的一半 |
| `pageAnnotationMessage.js`（新） | 解析 / 复制 / 截图剥离 | R10 的载体 |
| `PageAnnotationMessageCard.jsx`（新） | 已发消息卡片 | R10 |
| `agent_browser_host_mac.mm` +110 | 每角独立半径 | R7 mac 侧 |

含义：**这批改动一旦丢弃，样式面板与表面几何要重写约 2600 行。** 这是「先把 WIP 提交为基线」（tasks.md P0）的实际理由。

## 4. 现成可复用 vs 必须放弃

### 可直接复用（新方案不改行为）

- 原生遮挡孔：`radius` / `square_corners` 位掩码 / `overlayBleed` / 每角独立半径
- 模态阻断：`interaction_blocked` → `EnableWindow(FALSE)` + `annotationChromeSuspension` 双套
- 样式面板全部控件与测试（`pageAnnotationStyles.browser.test.js` 215 行）
- 文本预览与文本节点保留（`page_annotation_text_runtime.hpp`）
- 多目标 + 单区域数据结构（`targets[]`）
- 导航提示（`page_annotation_navigation.hpp`）—— 只要把「默认停留」与「从不自动发送」保留
- 验收运行器与 fixture（`html_annotation_fixture.html`、`integration_driver.cpp`）

### 必须放弃（新方案直接冲突）

| 对象 | 冲突点 |
|---|---|
| `pageAnnotationBatches.js` + `.test.js` | Q26：批次状态机、截图、回执对账、`fullPage` 断言全部删 |
| `annotation_batch_id`（全仓 43 处） | 批次身份不再存在 |
| `reconcileOnly`（11 处） | 服务端 404 对账协议取消，改 `client_message_id` 自查 |
| 原生租约 `capture` / `validate` / `acknowledge` / `release` | 状态机已取消 |
| 工具栏「截取屏幕截图」按钮 | D2：截图在 save 时按 rect 裁剪 |
| 工具栏「发送中 / 重试」三态 | Q35：回执状态统一由 composer 广播 |
| 「已保存批注不可编辑不可删」 | Q36 |
| `page_annotation_session.hpp` 的 `records_` 归属 | Q33：上提到 `SessionAnnotationStore` |
| `valid: bool` | 四态 `live / replayable / stale / orphan` |

## 5. 落地建议

1. **先 P0**：把当前 51 个改动提交成一条 `wip:` 提交（或推到 `wip/html-page-annotations` 分支）。否则 #209 的搬运 diff 会混在 4445 行里，评审无法分辨。
2. **分支只读**：十个分支保留作历史参照，不合、不删、不再往上加东西。它们的内容都已在当前分支里，合过来只会产生空 commit 或冲突噪音。
3. **按 ticket 重做，不按分支合**：#208 先出记录标识结论（`record_id` 在 HMR 换 `document_id` 后失配的隐患），再开 #209。
4. **合入策略**：#209–#220 全部落在 `codex/html-page-annotations` 上，最后一个 PR 整体对 master。中间不要分十次 PR——它们互相依赖且会把 30 个提交的历史切成碎片。
