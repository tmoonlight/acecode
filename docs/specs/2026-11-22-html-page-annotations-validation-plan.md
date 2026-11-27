# HTML 网页批注 —— 技术验证清单

- 日期：2026-11-22
- 目的：把新方案里**三个还没被任何代码证明过的假设**先测掉。它们一旦不成立，会反过来改设计，所以必须排在实现之前。
- 判定原则：**能不改设计就不改**；只有通过标准明显不达标才走退路。

---

## 已静态确认的事实（不用测）

| 事实 | 证据 |
|---|---|
| 现有捕获是「3 跳 CDP + 整页」 | `agent_browser_capture_cdp.cpp:30`（`cssContentSize`）、`:61`（读 device scale）、`:71-74`（`captureBeyondViewport:true` + `scale:1/dpr`） |
| 捕获参数可离线断言 | `tests/test_support/desktop/agent_browser_capture_probe_command.hpp` + `tests/desktop/agent_browser_capture_probe_test.cpp`（`FakeClient` 记 method/params） |
| 批注状态事件目前只带面板页签 | `AgentBrowserPanel.jsx:83`（`event.detail.page_id !== pageId` 直接 return） |
| 消息可写 `client_message_id` 到 metadata | `routes_sessions.cpp:524`，WS 回推 `:734-738` |
| 没有「编辑消息」的 API | 全仓无 `PATCH /messages`、无 `editMessage` |

---

## V1 —— save 时捕获会不会拖垮「保存」手感

**假设**：`save` 时做一次视口捕获 + 本地裁剪，用户感知不到延迟。

**为什么重要**：D2 把截图从「发送时」前移到「每次 save」。Windows 每条命令本来要 3 跳 CDP（`agent_browser_host.cpp:2212-2258`，尚无 `executionContextId` 缓存），再加一次 `Page.captureScreenshot`，可能让"保存批注"从瞬时变成卡顿。

**方法**

1. 在 `save` 分支的首尾各打一条 `LOG_INFO` 时间戳（或复用 `capture_probe` 的计时），跑 20 次保存；
2. 分别在三个尺寸下测：1280×800（笔记本）、1920×1080（外接屏）、2560×1440（高分 + DPR 2）；
3. 记录 P50 / P95。

**通过标准**：P95 ≤ 150ms（一次点击的可感知阈值约 100–200ms）。若开了 `executionContextId` 缓存，目标 ≤ 120ms。

**退路（按代价排序）**

1. **异步补图**：`save` 立即落记录（无图），捕获完成后回填 `thumbnail` 并推一次卡片刷新。用户点保存立刻看到图钉，缩略图后到。改动小，体验几乎无损。
2. 捕获降到 `quality` 更低的 jpeg（体积小、编码快）。
3. 退回「发送时截图」，接受 `orphan` 记录无图（D2 作废，Q25/Q28 恢复原样）。**最后才选这条**——它会重新引入"关页签就没图"。

---

## V2 —— 跨页签广播会不会刷爆 WebView2 通道

**假设**：把状态事件从「只发面板页签」改成「发会话内所有页签」，在多页签 + 多条批注时仍然可承受。

**为什么重要**：`sync` 已经是每次增删一次全量下发；广播面从 1 个页签扩到 N 个，事件量按页签数线性增长，而 WebView2 的 `postMessage` 是同步序列化。

**方法**

1. 构造 5 个页签 × 每页 10 条批注；
2. 在一个页签上快速增删（连续 20 次 save + delete），统计 1 秒内的事件数与单事件字节数；
3. 同时观察另一端：另一个页签的浮窗是否出现掉帧或输入延迟。

**通过标准**：峰值 ≤ 20 事件/秒，单事件 ≤ 8KB，另一端无可感知卡顿。

**退路**

1. 广播节流 50ms 合并（同一 `page_id` 的多次变化只发最后一次）；
2. 只推 diff（新增/删除/改编号的 id 列表），由 WebUI 应用；`sync` 仍全量，但只发给**有图钉的页签**；
3. 极端情况：非面板页签不推送，卡片在被切到该页签时再拉一次全量。

---

## V3 —— `document_id` 变化后 `open_record_popup` 还能不能定位

**假设**：点铅笔时（尤其 dev 环境 HMR 之后）能可靠地把浮窗开在那条记录上。

**为什么重要**：`open_record_popup` 要拿 `record_id` 找到 DOM。但 `replace_document` 会换 `document_id`，`record.id` 目前是 `<document_id>:annotation:<n>`（见 §5.1），HMR 之后 id 就对不上了。

**方法**

1. 打开一个 dev 页面（Vite HMR 可用），保存 2 条批注；
2. 改一次源码触发 HMR（不刷新）；
3. 立刻从卡片点铅笔；
4. 再重复「手动刷新页面」这个变体。

**通过标准**：HMR 后能定位并回填描述（此时记录应已重放为 `live`）；刷新后若重放未完成则**必须**降级为弹窗而不是报错。

**退路**

1. `record_id` 改为与 `document_id` 无关的稳定 id（`page_id` + 创建序号），`document_id` 只作为"当前绑定"字段；
2. 定位失败一律走路径 B（弹窗改文本），并在卡片行标「目标已失效」；
3. 若 HMR 场景定位成功率低于 80%，就把「点铅笔」默认走弹窗，只在 `live` 且 `document_id` 匹配时才激活页签。

---

## 执行顺序与产出

| 顺序 | 项 | 需要什么 | 产出 |
|---|---|---|---|
| 1 | V1 | 能跑起来的桌面壳（`run_build.py` 编 desktop 目前缺 winrt 头，见本机笔记的绕行脚本） | 三个尺寸的 P50/P95 数字 + 是否走退路 |
| 2 | V3 | dev 服务器 + HMR | 定位成功率 + 降级是否触发 |
| 3 | V2 | 5 页签构造脚本 | 峰值事件数 / 单事件大小 |

**V1 与 V3 可以并行**（一个测性能、一个测定位）。V2 依赖跨页签广播**已经实现**，所以它天然排在 P2 之后——如果不想等，可以先按「预计 5 页签 × 10 条」的量级做一次假数据压测（WebUI 内部模拟，不碰原生）。

---

## 结论怎么用

- V1 通过 → P3 按原计划；不通过 → 走退路 1（异步补图），设计只需补一句"缩略图可迟到"。
- V3 通过 → `open_record_popup` 沿用 `record_id`；不通过 → 先做稳定 id 改造，它会顺带影响 P1 的 store 结构，**必须在 P1 之前定**。
- V2 通过 → 广播方案照写；不通过 → 加节流，属于纯实现细节，不影响设计。
