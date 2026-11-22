# T0 技术验证结论（#208）

> 日期：2026-11-22　·　对应工单：[#208](https://github.com/tmoonlight/acecode/issues/208)　·　方案：[#207](https://github.com/tmoonlight/acecode/issues/207)
> 输入：`codex/html-page-annotations` @ `44c3fad4`（WIP 基线已提交）

本文件回答两件事：**（一）文档被替换后还能不能按记录里的标识定位目标；（二）保存时做一次视口捕获 + 裁剪的耗时是否可接受。**

两项都有结论，且都改变了后续实现：标识必须改（#209 的存储结构照此落），捕获必须走异步补图（#212 的实现形态照此落）。

---

## 一、记录标识：不是「成功率多少」，是结构上不可能

**结论：现行 `record.id` 在文档替换后 100% 失效，且不是实现 bug，是标识设计的结果。必须改成与文档无关的稳定 id。**

原计划「HMR / 手动刷新各跑 10 次点铅笔定位，给成功率」。读代码后确认这个实验不必跑——失败是构造性的，跑出来必然是 0/10。下面是完整因果链，每一环都有行号：

| # | 事实 | 位置 |
|---|---|---|
| 1 | `document_id` 由页签 id + **自增 epoch** 拼成 | `page_annotation_session.hpp:213` `document_id_ = page_id + ":document:" + std::to_string(epoch_)` |
| 2 | 文档替换时 `epoch_` 自增，`document_id_` 被**清空**；下次 `start` 生成新 id | `page_annotation_session.hpp:143-168` |
| 3 | 记录 id 由 `document_id` 拼成 | `page_annotation_session.hpp:381` `{"id", document_id_ + ":annotation:" + std::to_string(next_number_)}` |
| 4 | 替换时所有既有记录被标 `valid=false` / `invalid_reason=document_replaced` | `page_annotation_session.hpp:161-164` |
| 5 | 页内 runtime 以 `document_id` 为键，换文档即 `dispose()` | `page_annotation_runtime.hpp:9-12` |
| 6 | 元素标识是 runtime 内的 `WeakMap` + 自增序号 | `page_annotation_runtime.hpp:28, 217` `ids.set(target, \`target-${++sequence}\`)` |
| 7 | 脚本一旦发现 `documentElement` 换了就返回 `replaced_document` | `page_annotation_runtime.hpp:377-381` |
| 8 | 替换后图钉（`pins`）随 runtime 一起消失，没有 DOM 可指 | `page_annotation_runtime.hpp:359-375` |

链条合起来：**刷新 → epoch+1 → 记录 id 指向一个不存在的文档代 → runtime 与图钉都已销毁 → 元素标识 `target-N` 在新 runtime 里从 1 重新数，语义完全不同。** 所以「按 record 里的标识定位」在数据层面就没有可解析的东西：`<page>:document:1:annotation:3` 里的 `document:1` 已经是历史。

HMR 分两种情形，都不改变结论：

- dev server 整页 reload（或框架重建 `documentElement`）→ 与手动刷新完全一致，`replaced_document`；
- HMR 原地替换 DOM 子树、`documentElement` 不变 → runtime 存活、`pins` 存活、记录仍是 `valid`，此时**根本不需要重新定位**（本来就没丢）。

也就是说：需要定位的场景一定是 runtime 已销毁的场景；而 runtime 一销毁，记录里除了 `page_url` 和文本，**没有任何跨文档可用的定位信息**——`target.id` 是内存序号，`record.targets[].locator` 只有 `tag#id`（`page_annotation_runtime.hpp:97`），对没有 `id` 的元素退化成 `div`。

### 决策

1. **`record.id` 改为与文档无关**：`<page_id>:annotation:<n>`。`n` 仍按 `page_id` 分组连续（编号语义不变），但不再嵌 `document_id`。
2. **`record` 新增 `locator` 字段**，`save` 时生成：CSS 结构路径（`nth-of-type` 链）→ `data-testid` / `aria-label` → 文本指纹 → 同级索引，逐级降级。这是 §4.6 重放算法里 `locate(record.locator)` 的唯一输入，现有 `tag#id` 不足以支撑。
3. **`target.id`（`target-N`）只在同一 runtime 内有效**，不作为跨文档标识，也不进记录的主键。
4. 重放失败 / 定位不到 → `stale`，按 §4.6 的「stale 可见性」处理（工具栏显示「N 条已失效」，条目置灰可查可删）。

第 2 条是这次唯一新发现的缺口：此前设计把 `locator` 当成既有能力，实际它只是个 `tag#id`。

---

## 二、保存时捕获 + 裁剪的耗时：**取决于有没有做 scale 修正**

测量方法：无头 Edge（`Edg/158`，`--headless=new --disable-gpu`，软件光栅），页面内用 canvas 复现保存路径——取一张视口大小的 PNG → 解码 → 按目标 rect 裁剪 → 缩到宽 512 → 编码回 data URL。每个变体 4 张不同的源图轮换，避免解码缓存命中；宽 512 缩略图，裁剪区 360×220 CSS px。

| 视口 | 捕获图尺寸 | 捕获 PNG | 解码 P50 | 裁剪 P50 | 编码 P50 | **总 P50** | **总 P95** |
|---|---|---|---|---|---|---|---|
| 1280×800 DPR1 | 1280×800 | 225 KB | 10.5 ms | 0.2 ms | 1.7 ms | **12.4 ms** | **16.0 ms** |
| 1920×1080 DPR1 | 1920×1080 | 900 KB | 20.7 ms | 0.4 ms | 1.5 ms | **22.5 ms** | **25.2 ms** |
| 2560×1440 DPR2（`scale=1/dpr`） | 2560×1440 | 463 KB | 29.2 ms | 0.2 ms | 1.2 ms | **30.3 ms** | **36.5 ms** |
| 2560×1440 DPR2（**没做** scale 修正） | 5120×2880 | 4602 KB | 154.1 ms | 0.2 ms | 10.5 ms | **163.3 ms** | **237.6 ms** |

原始数据：`C:/Users/Administrator/AppData/Local/Temp/annotation-bench/`（`index.html` + `drive.mjs`，可重跑）。

**三条读法：**

1. 做了 `scale = 1/dpr`，视口捕获图就是 CSS 像素尺寸，最大档 P95 也只有 37 ms，**远低于 150 ms 门槛**——保存路径里这一段的性能不是问题。
2. **不做 scale 修正，DPR2 下 P95 是 237 ms，直接越线**（图大 4 倍、PNG 大 10 倍，解码 + 编码都翻倍）。所以 `scale = 1/dpr` 不是优化选项，是**硬要求**；这也决定了坐标换算：裁剪 rect 直接按 CSS 像素用，不用乘 DPR。
3. 耗时几乎全在**解码**，裁剪本身 0.2 ms、编码 1~10 ms。想再快只能减少像素，而不是换裁剪算法。

**测不到的部分**：CDP `Page.captureScreenshot` 本身（含 base64 过 IPC）没测——那需要跑起桌面壳并连真实页面。上表是页面侧的成本，不是端到端。

### 决策：走异步补图，不把捕获塞进 save 的返回路径

理由不是「测出来超了」（没超），而是：

- `save` 是直接操作动作（点「保存」→ 浮窗关闭 → 图钉出现），用户对它的感知是「立刻」。把一次 CDP 往返 + 上表 30 ms + base64 过 IPC 串在返回路径上，会让图钉出现的时间取决于捕获快慢，而这部分收益（立刻有缩略图）在待发卡片上并不紧急。
- 捕获本身可能失败（视口外目标、mac 拿不到快照、文档中途变化），串行实现就得让失败路径也阻塞交互，或者加超时兜底——两种都更复杂。
- 异步后，「图还在不在」与 `save` 成败彻底解耦，`screenshot_unavailable` 变成一个普通字段的更新，不是一次交互失败。

具体形态（#212 照此实现）：

```
save  →  记录立刻入库并广播（thumbnail 空，screenshot_pending:true）
      ↘  原生异步：视口捕获（scale=1/dpr）
          → 页内裁剪（目标 rect，宽 ≤512）→ thumbnail
          → 原图存 page_shot（按 page_id 覆盖）
          → 回写记录 + bump revision + 广播（卡片缩略图 / 已发卡片配图就位）
      ↘  失败（视口外 / 文档已换 / 超时 2s）→ screenshot_unavailable:true，不重试
```

超时 2s 是防守线：视口捕获正常是几十毫秒，2s 还没回来基本就是 CDP 挂了，继续等只会让记录长期停在「半成品」。

---

## 三、对下游工单的影响

| 工单 | 变化 |
|---|---|
| #209 记录上提 | 存储主键用 `<page_id>:annotation:<n>`；`document_id` 降为记录的一个属性（用于判断 `live` / `replayable`）；新增 `locator` 字段 |
| #210 读路径迁移 | `record.document_id == snapshot.document_id` 不再是「有效性」判据，改判 `live` 与否 |
| #212 截图前移 | 走异步补图；强制 `scale=1/dpr`；裁剪坐标用 CSS 像素 |
| #217 铅笔路径 A | 依赖 `locator`：`live` 直接激活浮窗；`replayable` 先 `locate(locator)`；`stale`/`orphan` 走 #218 的弹窗 |

设计文档已同步：§1.1 新增 D9（记录标识）、D10（捕获时机改异步）；§4.8 换成异步流程；§4.2 记录模型更新。
