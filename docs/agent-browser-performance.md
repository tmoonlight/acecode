# Agent Browser 性能改造方案

对标项目：[browser-use/jev-ultrafast](https://github.com/browser-use/jev-ultrafast)（2026-09 抓取）。

本文只谈**延迟**。正确性与隐私类问题另见文末「附录 B」，不在本方案范围内。

---

## 一、背景：jev 的「7 秒」是怎么来的

jev-ultrafast 的卖点是一句话：一条自然语言目标，Google Flights 上从苏黎世搜到伦敦，
**7.1 秒**，含模型调用、文本生成、浏览器操作和加载等待。

拆开看，这 7 秒的构成与直觉相反：

| 来源 | 占比 | 说明 |
| --- | --- | --- |
| 模型时间 | 绝对主导 | 每步一次模型请求，约 10 余步 |
| 浏览器协议时间 | 零点几秒 | 整个任务 101 次 CDP 调用，持久会话上每次约毫秒级 |

它自己公布的优化前后对比更说明问题：

| 指标 | 优化前 | 优化后 | 变化 |
| --- | --- | --- | --- |
| 浏览器协议调用 | 1092 | 101 | 降低 91% |
| 任务耗时中位数 | 9.450 秒 | 7.092 秒 | 降低 25% |

**把浏览器协议调用砍掉 91%，任务只快了 25%。** 这是本方案最重要的一条背景事实：
jev 的速度主要不来自浏览器代码，而来自它的**模型形态**和**每步只发一次请求**。

它快的三个真实原因：

1. **模型不是通用 LLM。** 决策模型是 TypeSafe 的 Jev，一个约束解码的分类器：输出一个
   操作枚举（`CLICK` / `TYPE_TEXT` / `SELECT` / `SCROLL_UP` / `SCROLL_DOWN` / `WAIT` /
   `DONE` / `BLOCKED`）加一个元素序号。只有需要输入文本时才调一次小而快的文本模型，
   当前 demo 用 `inception/mercury-2.5` 且关闭 reasoning。
2. **每个决策周期一次网络往返。** 操作头和目标头共享同一份观测状态，投机式扇出，
   一次请求同时拿到「做什么」和「对谁做」。目标头里只包含与该操作兼容的元素。
3. **步数少，且每步的状态是替换而非累积。** 模型每步只看当前元素表，历史是一份
   紧凑的决策列表，不是一堆过期页面全文。

它明确不做的：默认循环里**没有截图**；结构化状态就是全部输入。截图只给它的可视化
inspector 用，演示视频走独立的 screencast。

### jev 在浏览器侧做的事（值得借的部分）

即使浏览器侧只值 25%，这 25% 的做法仍然干净，值得参考：

- **一次快照一次调用。** `jev_ultrafast/snapshot.js` 一个 `Runtime.evaluate` 原子返回
  url、title、viewport、可见文本、动作表、新鲜度标记、逐节点守卫。
- **只发可见内容。** 元素中心点落在视口外的直接丢弃；正文用 Range 的 rect 判断是否
  真在屏幕上，上限 6000 字符。屏幕外的正文和页脚不进模型上下文。
- **动作空间而非元素空间。** 表里每一行都是可执行动作：`<select>` 的每个选项各一行
  select 动作；可编辑 combobox 额外给一行「Open X」的 click；页面能滚动时才追加
  scroll_up / scroll_down。模型只挑序号，不可能的操作根本不在候选里。上限 250 条，
  超出回报 `omitted_actions`。
- **等待极短且有条件。** 输入后最多两个动画帧或 50 毫秒；往 combobox 填字时改为等
  可见的 `[role="option"]` 出现，上限 200 毫秒。而且这个等待挂在**下一次**观测上，
  执行结果先落账。
- **后台标签页不被节流。** `Emulation.setFocusEmulationEnabled` 让自己拥有的后台页
  保持 rAF 和菜单渲染，同时不抢用户正在看的标签页。
- **几何确定化。** `Emulation.setDeviceMetricsOverride` 固定 1120x780。
- **填字一次到位。** selectAll 后 `Input.insertText` 整段写入，不逐字符敲。

### jev 的边界

MVP 明确不处理 shadow root、iframe、canvas、文件上传、弹出标签页、嵌套滚动和任意
键盘组件。`DONE` 仍需独立验证。这些方面它没有可借的实现。

---

## 二、ACECode 的延迟预算与 jev 不同

同一个任务，ACECode 慢一到两个数量级，但**瓶颈不在 `browser_tools.cpp` 里**：

| 项 | jev | ACECode 现状 | 量级 |
| --- | --- | --- | --- |
| 每个动作的模型往返次数 | 1 | 2 | 2 倍 |
| 每次请求携带的页面状态 | 约 3 至 4 千 token，每步替换 | 约 15 千 token，逐次累积 | 数量级 |
| 每次点击的浏览器请求数 | 约 3，持久会话 | 7，每次新建连接 | 每动作百毫秒级 |
| 每次点击的固定等待 | 50 毫秒 | 80 毫秒，导航另加 300 毫秒 | 每动作百毫秒级 |

前两行是数量级差距，后两行是每动作一两百毫秒。**优化顺序必须按这个排。**

---

## 三、现状测绘

以下均为代码核对结果，可逐条复查。

### 3.1 一次 `browser_click` 是 7 次 IPC 请求

`src/tool/agent_browser/browser_tools.cpp:653-686`：

| # | 调用 | 位置 | 备注 |
| --- | --- | --- | --- |
| 1 | `claim_page` 或 `select_page` | `connect_client` `:192` | 每个工具都做 |
| 2 | `Runtime.evaluate` 解析目标 | `resolve_target` `:292` | 返回 rect / role / name |
| 3 | `Runtime.evaluate` 画 AI 指针 | `show_agent_pointer` `:669` | 纯视觉，**随后固定睡 80 毫秒** |
| 4 | `Input.dispatchMouseEvent` mouseMoved | `:672` | |
| 5 | `Input.dispatchMouseEvent` mousePressed | `:673` | |
| 6 | `Input.dispatchMouseEvent` mouseReleased | `:675` | |
| 7 | `Runtime.evaluate` 读页面摘要 | `page_summary` `:234` | 只为 url / title / ready_state 三个字段 |

`browser_drag` 更重：2 次解析 + 2 次指针 + press + 8 步插值 mouseMoved + release + 摘要，
约 16 次请求（`:928-976`）。

`browser_read_page` 是 2 次（claim + 一次快照 evaluate），**快照本身已经是良好的批量设计**。

### 3.2 传输层每个请求新建一条连接

`AgentBrowserCdpClient::request()` 每次调用都完整走一遍：`open_proxy_pipe` /
`open_proxy_socket`（`src/tool/agent_browser/cdp_client.cpp:551` / `:562`）、写 u32 长度 +
JSON、读 u32 长度 + JSON、写 1 字节 ack、`close_pipe()`（`:622-627`）。

即：**一次点击建立并拆除 7 条命名管道或 Unix socket**，没有任何连接复用。

### 3.3 固定 sleep 清单

| 位置 | 时长 | 性质 |
| --- | --- | --- |
| `browser_tools.cpp:520` | 250 毫秒 | `browser_open` 无条件 |
| `browser_tools.cpp:594` | 300 毫秒 | `browser_navigate` 无条件 |
| `browser_tools.cpp:40` `:670` | 80 毫秒 | `kPointerClickLeadTime`，click / drag 前 |

没有任何基于 `Page.loadEventFired` 或生命周期事件的等待。click / type / fill / press /
hover / scroll / drag 之后**零 settle**，`url` / `title` / `ready_state` 在最后一个输入事件
之后立刻采样——所以**一次触发跳转的点击基本都回报旧 URL**，模型据此会误判「没跳转」。

显式等待完全交给模型调 `browser_wait`，它每 100 毫秒轮询一次 evaluate（`:1110`），
默认上限 10 秒，最大 120 秒，即最坏 1200 次 evaluate。

### 3.4 快照体积

`agent_browser_snapshot_script()`（`browser_tools.cpp:1311-1370`）默认值：

- `max_text_chars` 默认 30000，上限 50000（`:1314`）——取的是整页 `document.body.innerText`，
  不区分是否在视口内（`:1365`）。
- `max_elements` 默认 300，上限 500（`:1313`）——文档顺序取前 N 个可见元素，
  不按视口过滤（`:1333-1334`）。
- 选择器里包含 `option`（`:1333`），所以**一个 250 项的国家下拉能吃掉整个元素预算**。
- 截断后没有任何「被省略了多少」的信号。

粗算一次 `browser_read_page` 约 15 千 token（正文约 8 千，元素表约 9 千，中文更高）。
而这份结果作为工具结果**永久留在对话历史里**，后续每一轮都要重发。读五次页面就是
约 75 千 token 的过期页面文本参与此后每一轮的计费与 TTFT。

### 3.5 每个动作需要两次模型往返

`browser_click` 返回 `{target, clicked, url, title, ready_state}`（`:678-686`），
**不含新的页面状态**。模型点完按钮不知道页面变成什么样，只能再发一轮
`browser_read_page`。所以稳态是「一个动作两轮模型」。

一条有利的现状：`window.__aceAgentBrowserSnapshot` 只在快照脚本里重写
（`:1341`），点击**不会**改变 `revision`，所以同一次读取拿到的 `@eN` 在多个动作之间
一直有效。这意味着批量执行不需要引入新的引用机制。

---

## 四、方案

四个杠杆，按收益排序。杠杆 1 与 2 解决数量级问题，3 与 4 解决常数问题。

### 杠杆 1：mutation 工具回带页面状态（每动作 2 轮 → 1 轮）

**动机**：见 3.5。这是最便宜的一倍。

**改法**：把 `success_result_with_page`（`:245-262`）从「只读 url / title / ready_state」
升级为「顺带返回一份新的紧凑元素表」，并与动作脚本合并为同一次 evaluate。
为控制体积，回带的表用比 `browser_read_page` 更严的预算（例如仅视口内、正文上限
2000 字符），并带上新的 `revision`。

**预期**：模型往返减半；顺带干掉 3.1 里的第 7 次请求。

**风险**：每个动作结果都变大。必须配合杠杆 3 先把快照瘦身，否则省下的往返会被
膨胀的历史吃回去。**因此杠杆 3 应先于杠杆 1 落地。**

### 杠杆 2：批量动作工具（N 轮 → 1 轮）

**动机**：jev 一个任务十几步，每步一次小模型请求。ACECode 每步一次前沿模型请求，
无法把单步变快，只能**减少步数**。

**改法**：新增一个批量工具，接一串步骤，例如「click @e3 / fill @e5 "Zurich" /
click @e7」。语义要求：

- 每步执行前做自己的新鲜度与命中校验，第一步不匹配就**停下**，回报已完成到哪一步、
  失败原因、以及失败时刻的页面状态。
- 绝不在中途静默跳过或猜测。
- 步数上限与总时长上限可配置。
- 沿用 `@eN` + `revision`，不引入新的引用机制（依据见 3.5 末段）。

**预期**：填一张表从七八轮往返压到一轮。这是本方案单项收益最大的一条。

**风险**：批量失败的语义必须极其清楚，否则模型会对「做到一半」的页面状态产生错误
前提。这也是必须先做遮挡命中测试（附录 B 第 1 条）的原因——批量执行会放大静默
失败的后果。

### 杠杆 3：快照瘦身（单次读取约 15 千 token → 约 3 千）

**动机**：见 3.4。这一项的收益是复利的，因为它减少的是**此后每一轮**的 prompt 体积。

**改法**，全部在 `agent_browser_snapshot_script()` 内：

1. **按视口过滤元素**。中心点落在 viewport 外的丢弃，照抄 jev 的判据。
2. **正文只取屏幕上可见的文本段**，用 Range 的 rect 判断，上限降到 6000 字符。
3. **`option` 从选择器移除**，折叠进父级 select；参考 jev 的做法，把每个可选项表达
   成父元素上的一个 select 动作。
4. **回报被省略的数量**，对齐 jev 的 `omitted_actions`，让模型知道自己看的是截断视图。
5. 顺带：`checkVisibility({checkOpacity:true, checkVisibilityCSS:true})` 加
   `closest('[aria-hidden="true"],[inert]')` 替换手写的 `getComputedStyle` 三连判断
   （`:1315-1321`），一次原生调用，更便宜也更正确。

**预期**：单次读取压到三四千 token，约 5 倍。**不触碰对话历史，无 prompt cache 风险。**

**关于「抹掉过期快照」**：把历史里被取代的旧快照替换成 stub 能省更多，机制上也已有
先例（PA 兜底里的 `ThreadRepairOptions::clear_tool_outputs`）。但按 CLAUDE.md 的
prompt cache 前缀不变量，改写历史会从改写点截断缓存，后面整条尾巴全价重算。
**本方案不做这一项**，先靠源头瘦身拿掉大部分收益；若将来仍需要，应设计成
低频水位触发而非每轮触发，并单独评估缓存代价。

### 杠杆 4：浏览器侧固定开销（每动作约 380 毫秒 → 约 50 毫秒）

四条互相独立：

1. **连接复用**。`AgentBrowserCdpClient` 持有长连接，7 次连接建立变 1 次。
   需要处理 host 重启、页面关闭与超时后的重连，失败时回落到当前的每请求建连行为。
2. **指针不挡关键路径**。`kPointerClickLeadTime` 那 80 毫秒是纯视觉效果
   （`:40` `:669-670`）。改成不等待返回，或者点击之后再画轨迹。指针本身是
   `pointer-events:none` 的 shadow root 覆盖层，不会造成遮挡，这一点不变。
3. **页面摘要合进动作脚本**。`page_summary`（`:234-243`）为三个字段单独跑一次
   evaluate，应与杠杆 1 一起合并。
4. **固定 sleep 换条件等待**。`browser_open` 的 250 毫秒与 `browser_navigate` 的
   300 毫秒（`:520` `:594`）换成 jev 那种「最多两个动画帧或 50 毫秒」的有上限条件
   等待；往 combobox 填字后改为等可见 `[role="option"]`，上限 200 毫秒。

做完这四条，一次点击从 7 个请求降到 2 个，固定等待从 380 毫秒降到 50 毫秒。

**附带修掉一个已知限制**：`docs/agent-browser.md:161` 记着「非显示页面的 WebView2
处于隐藏状态，Chromium 会节流定时器」。`Emulation.setFocusEmulationEnabled` 正是
jev 用来解决同一问题的手段，让后台页保持渲染而不抢用户正在看的标签页。
建议在这一杠杆里一并接上，同时用 `Emulation.setDeviceMetricsOverride` 固定几何。

---

## 五、更远的选项：`browser_task` 子代理

如果要真正对齐 jev 的架构，而不只是在现有形态里榨常数：

新增一个 `browser_task(goal)` 工具，内部跑 jev 那套「索引动作空间 + 每步一次请求」
的小循环，配一个独立配置的快模型，只把最终结果回给主模型。主模型全程只花一轮。

ACECode 已有两处同形状的先例可以复用设计：`spawn_subagent` 的子会话机制，以及
`vision_analyze` 那种「一次性子调用 + 独立模型连接」的模式（`image_generation` 与
`vision_analyze` 都已经这么做）。

这是换形态，成本和风险都显著高于前四个杠杆，**建议在杠杆 1 至 4 落地并量到收益之后
再单独立项**。

---

## 六、明确不做

- **不动截图路径。** 截图本来就不在默认循环里，只由 `browser_screenshot` 产生，
  这一点已经与 jev 一致。
- **不改填字方式。** 已经在用 `Input.insertText` 整段写入，不是逐字符。
- **不重构快照的批量性。** 快照已经是一次 evaluate 原子返回，设计正确。
- **不抄 jev 的安全立场。** jev 声明「模型输出永不变成选择器、坐标、shell 命令或
  可执行 JS」。ACECode 有意暴露 `selector` 目标与 `browser_evaluate`，按 CLAUDE.md
  的定位这是**永久开发宽松浏览器**，不应照抄收紧。
- **不抄 shadow DOM / iframe 支持。** jev 的 MVP 同样不支持，没有可借的实现。
- **不做历史快照抹除**（理由见杠杆 3 末段）。

上面三条「已经做对」恰好是这类项目最常见的性能坑，先确认无需返工。

---

## 七、实施顺序与验证

建议顺序（杠杆 3 必须先于杠杆 1，理由见杠杆 1 的风险项）：

| 阶段 | 内容 | 预期收益 | 独立性 |
| --- | --- | --- | --- |
| 1 | 杠杆 3 快照瘦身 | 单次读取约 5 倍 | 完全独立 |
| 2 | 杠杆 4 浏览器侧常数 | 每动作约 330 毫秒 | 完全独立 |
| 3 | 杠杆 1 回带页面状态 | 模型往返减半 | 依赖阶段 1 |
| 4 | 杠杆 2 批量动作 | 单项最大 | 依赖阶段 3 与附录 B 第 1 条 |

**量化口径**：改造前后各跑同一组任务，记录三个数字，缺一不可。

1. 单任务的模型往返次数。
2. 单任务的 IPC 请求总数。
3. 单任务墙钟耗时，并分离出模型时间与浏览器时间。

只看墙钟会把模型侧的抖动误判成改造效果。jev 自己也强调它那组数据是「单任务、单
浏览器配置、三次重复」，不是通用可靠性基准；我们的口径同样要写清样本条件。

改动集中在 `src/tool/agent_browser/browser_tools.cpp` 与 `cdp_client.cpp`，
按 AGENTS.md 的命令集跑 `acecode_unit_tests`；杠杆 4 的连接复用需要 Windows 与 macOS
两端实测，因为传输实现不同（命名管道与 Unix socket）。

---

## 附录 A：jev 中可直接移植的片段

| 用途 | jev 位置 | 移植目标 |
| --- | --- | --- |
| 可见性判据 | `snapshot.js` 的 `visible` | 替换 `browser_tools.cpp:1315-1321` |
| 视口过滤与动作表构造 | `snapshot.js` 主循环 | `agent_browser_snapshot_script()` |
| 可见文本提取 | `snapshot.js` 的 TreeWalker + Range | 同上 |
| 执行前命中测试 | `browser.py` 的 target 脚本 | `element_mutation_script` 与 click 路径 |
| 有上限的条件 settle | `browser.py::observe` 的 rAF 脚本 | 替换 `:520` `:594` 的固定 sleep |

## 附录 B：与性能无关但建议顺手修的两条

这两条不在本方案范围内，但杠杆 2 会放大第 1 条的后果，建议同期处理。

1. **点击前没有遮挡命中测试。** 目标在第 2 次请求解析出坐标，随后经过 80 毫秒指针
   等待和至少 4 次 IPC 往返才真正派发，全程不重新验证几何，也没有
   `document.elementFromPoint` 命中测试（可见性判据见 `:269`）。cookie 横幅或 modal
   盖住目标时点击被吞掉，工具仍回报 `clicked:true`。jev 在执行前重新解析并做
   `e.contains(document.elementFromPoint(x,y))`，不过就要求重新观察。

2. **密码会进模型上下文与会话存档。** 快照选择器包含 `input` 且不排除
   `type="password"`（`:1333`），每个元素都带 `value: String(el.value).slice(0, 500)`
   （`:1350`）；`textOf` 在没有 aria-label 时也会把 value 当作 name（`:1322-1325`）。
   用户在 Agent Browser 页面登录后，任何一次 `browser_read_page` 都会把明文密码写进
   transcript 与会话 JSONL。jev 用一行 `safe = e => !['password','file','hidden'].includes(e.type)`
   挡掉。建议独立修复，优先级高于本方案全部性能项。
