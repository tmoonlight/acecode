---
name: inline-visuals
description: 在 ACECode 网页 / 桌面版对话里用 ```visual 代码块画可交互的示意图、数据图表与对比卡片的规范。第一次画 visual 之前加载；写明隔离框提供的主题变量与类名、SVG 布局与配色、交互与脚本、图表库与示例。
---

# 对话内可视化（visual 代码块）

ACECode 网页 / 桌面版会把回复里**顶层**的 ```visual 代码块画在对话中的隔离框里：你一边写，图一边长出来；代码块闭合后才执行其中的脚本。用户看到的是图，不是代码。

## 先选对形式

| 内容 | 用什么 |
| --- | --- |
| 模块依赖、调用 / 请求流程、时序、状态机、类图、ER 图 | mermaid（更省 token，两端都能画） |
| 机制原理示意（数据怎么流动、缓存怎么命中、算法每一步） | visual：手写 SVG |
| 可交互讲解（滑块调参数、开关对比、分步播放） | visual：HTML + 少量脚本 |
| 真实数据的图表（趋势、占比、分布、对比） | visual：手写 SVG，数据多时用图表库 |
| 方案 / 选项并排对比 | visual：卡片网格；只有几行时用表格 |
| 一两句话、简单列表、用户要的是自己项目里的 HTML / SVG 代码 | 不画；代码用 ```html / ```svg 代码块 |

一条回复通常最多一张图。图放在最终回复里，前后用一两句话交代结论，不要再用文字把图的内容复述一遍，也不要解释标记本身。

## 代码块格式

- 写成顶层代码块：行首 ```visual，不要缩进在列表或引用里（那样只会显示源码）。
- 内容是一个片段：以 `<svg`、`<div` 等元素开头；不写 `<!DOCTYPE>`、`<html>`、`<head>`、`<body>`。
- 顺序固定：`<style>`（可选）→ 标记 → `<script>`（可选，放最后）。
- 边写边画：先写外层容器和整体布局，再逐个写节点；长数据放进末尾脚本里，别堆在开头。
- 控制体量：一般 60–200 行。超过 300 行通常说明该拆成文字 + 一张更简单的图。

## 隔离框提供的环境

宽度等于对话正文宽度（桌面约 640–760px，手机更窄），高度随内容自动撑开（上限 4000px）。背景透明，与对话融为一体。

### 主题变量（会随浅色 / 深色切换自动更新）

| 变量 | 用途 |
| --- | --- |
| `--fg` / `--fg-2` / `--fg-3` | 主文字 / 次要文字 / 弱化的线条与标注 |
| `--surface` / `--surface-2` | 卡片底色 / 稍深一级的底色 |
| `--border` / `--border-strong` | 普通边框 / 需要看清的边框与分隔线 |
| `--accent` / `--accent-bg` | 主强调色 / 浅强调底色 |
| `--ok` `--ok-bg` / `--warn` `--warn-bg` / `--danger` `--danger-bg` | 成功 / 注意 / 错误 |
| `--font` / `--mono` | 正文字体 / 等宽字体 |

`<html>` 上有 `data-mode="light"` 或 `data-mode="dark"`，确实需要按模式区分时写 `[data-mode="dark"] .x { … }`。**不要写死 `#000`、`#fff`、`black`、`white` 这类颜色**，深色模式下会看不见。

### 色调类

`tone-blue` `tone-teal` `tone-green` `tone-amber` `tone-red` `tone-purple` `tone-pink` `tone-gray` 设置三个变量：`--tone-bg`（浅底）、`--tone-line`（描边）、`--tone-ink`（底上的文字）。下面的类都读这三个变量，所以把色调类加在 `<g>` 或卡片上，里面的形状和文字一起变色；嵌套时最近的一层生效。没有色调时是中性色。

一张图最多用 2–3 种色调，并且含义固定：蓝 = 主流程，绿 = 成功 / 输出，琥珀 = 注意 / 瓶颈，红 = 错误 / 风险，灰 = 基础设施 / 次要，紫 = 模型 / AI。

### SVG 辅助类与箭头

| 类 | 作用 |
| --- | --- |
| `node` | 形状填 `--tone-bg`、描边 `--tone-line` |
| `edge` | 连线：无填充、`--fg-3` 描边；加 `dashed` 变虚线 |
| `t` | 14px 文字，颜色 `--tone-ink`（中性时为 `--fg`） |
| `t-sm` / `t-lg` / `t-mono` | 12px 次要文字 / 16px 加粗标题 / 等宽 |
| `t-mid` | 以 x、y 为中心水平垂直居中 |
| `clickable` | 手形光标，悬停时描边加粗 |

预置标记：连线加 `marker-end="url(#arrow)"` 画箭头（颜色跟随连线），`marker-start="url(#dot)"` 画起点圆点。不用再自己写 `<defs><marker>`。

### HTML 辅助类

`card`（带边框圆角的卡片，可加色调类）、`grid`（自动换行的等宽网格）、`row`（横向排列）、`stack`（纵向排列）、`badge`（小标签，可加色调类）、`muted`（次要文字）、`small`（12px）、`mono`（等宽）。`button`、`input`、`select`、`table`、`code`、`h1`–`h3` 已有与 ACECode 一致的默认样式；`button.primary` 是主按钮。

### 框内接口 `ace`

- `ace.send(text)`：把 `text` 作为用户的下一条消息发出。只在用户真实点击的处理函数里调用才生效；会话正忙时只填进输入框。别名 `sendPrompt(text)`。适合「点节点追问细节」「点选项继续」。
- `ace.openLink(url)`：用系统浏览器打开 http / https 链接。普通 `<a href>` 点击也会自动走这里。
- `ace.palette`：8 个解析好的十六进制颜色，给 canvas 图表库用（它们读不了 CSS 变量）。
- `ace.mode`：`"light"` 或 `"dark"`；`ace.onThemeChange(fn)` 在切换主题时回调 `fn({ mode, palette })`，canvas 图表在这里重设颜色后 `update()`。

### 限制

- 没有网络：只能从 cdnjs.cloudflare.com、cdn.jsdelivr.net、unpkg.com、esm.sh 加载脚本、样式与字体，其它地址（包括本机）一律拦截。
- **Chart.js 4 与 d3 7 是内置的**：照常写它们的 CDN 地址（见下面「图表」），隔离框会改用 ACECode 自带的副本，断网也能用。其它库（ECharts 等）只有连得上 CDN 才能加载，离线或受限网络常常连不上，所以简单的图**优先纯 SVG / HTML**。
- 图片只能是 `data:` / `blob:` 或内联 SVG，不能引用网络图片。
- `localStorage` / `sessionStorage` 只在内存里，刷新即丢。
- 不能弹 `alert` / `confirm` / `prompt`，不能提交表单、打开新窗口或导航页面。
- 不要用 `position: fixed`、`vh` / `vw` 单位、`height: 100%` 撑满高度：框的高度跟着内容走，按视口算会让它越撑越高。

## SVG 布局规则

- 根元素写 `<svg viewBox="0 0 680 H" width="100%">`，H 按内容算；不要写固定 `height`，不要写 `style="width:680px"`。
- 先在心里排网格：四周留 16–24px；同层节点等宽等高、等间距；主流程从左到右或从上到下，不要回折交叉。
- 估算文字宽度：14px 下英文约 7.5px / 字符，中文约 14px / 字。节点宽度 = 最长一行文字宽度 + 左右各 16px；一行放不下就用多个 `<text>` 或 `<tspan x="…" dy="18">` 换行，**绝不让文字溢出形状**。
- 节点用 `<rect class="node" rx="8">`；标签用 `<text class="t t-mid" x="中心x" y="中心y">`。
- 连线从形状边缘出发、到另一个形状边缘结束（箭头不要扎进形状里），平行连线之间至少 12px；连线标签用 `t-sm`，放在线段中点旁，不压在线上。
- 分组框用 `class="node dashed"` 加 `tone-gray`，标题写在框内左上角。
- 小于 12px 的文字看不清；标题最多一个，用 `t-lg`。

## 交互与脚本

- 脚本在代码块闭合后按顺序执行一次（外链脚本加载完才执行下一段），之后会补发 `DOMContentLoaded` 与 `load`，挂在它们上的初始化照常运行。流式输出期间脚本和 `on…` 属性都不生效，所以页面在没有脚本时也要能看。
- 用 `addEventListener` 绑定事件；状态放在脚本变量里；DOM 用 `document.getElementById` / `querySelector` 取。
- 可点的追问：给元素加 `clickable`，点击时调 `ace.send('…')`，文字写成用户会说的完整问题，例如「展开讲讲缓存失效的三种情况」。
- 动画用 CSS transition 或 `requestAnimationFrame`，单次不超过 1 秒，不要无限循环闪烁。
- 内置库以外的外链脚本可能加载失败：失败时框内会显示一行提示，你的脚本要能在库不存在时不报错（`if (window.echarts) …`）。

## 图表

- 数据必须来自对话、工具输出或用户提供的材料，**不要编造数字**；写清楚单位和数据来源。
- 30 个点以内的柱状 / 折线 / 占比图优先手写 SVG：坐标轴用 `--border-strong`，网格线用 `--border`，数据用色调类或 `--accent`，数值标签用 `t-sm`。
- 数据多或需要悬停提示时用内置的 Chart.js 4（写 4.x 的任意 CDN 地址都会用本地副本；2.x / 3.x 的配置写法不兼容，不要用）：

```html
<div style="position:relative;height:300px"><canvas id="c"></canvas></div>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<script>
  const css = getComputedStyle(document.documentElement);
  const ink = css.getPropertyValue('--fg-2').trim();
  const grid = css.getPropertyValue('--border').trim();
  const chart = new Chart(document.getElementById('c'), {
    type: 'line',
    data: { labels: ['一月', '二月', '三月'], datasets: [{ label: '请求数', data: [120, 180, 150], borderColor: ace.palette[0], backgroundColor: ace.palette[0] }] },
    options: { maintainAspectRatio: false, scales: { x: { ticks: { color: ink }, grid: { color: grid } }, y: { ticks: { color: ink }, grid: { color: grid } } }, plugins: { legend: { labels: { color: ink } } } }
  });
  ace.onThemeChange(({ palette }) => {
    const next = getComputedStyle(document.documentElement);
    chart.data.datasets[0].borderColor = chart.data.datasets[0].backgroundColor = palette[0];
    chart.options.scales.x.ticks.color = chart.options.scales.y.ticks.color = next.getPropertyValue('--fg-2').trim();
    chart.update();
  });
</script>
```

- canvas 一定要包在有固定高度的容器里，并设 `maintainAspectRatio: false`。
- 需要自由布局的数据图（力导向、树、地图投影、自定义坐标轴）用内置的 d3 7：`<script src="https://cdn.jsdelivr.net/npm/d3@7/dist/d3.min.js"></script>`，之后用全局的 `d3`。不要用 `<script type="module">` 的 import 写法引库，那种写法用不上内置副本。

## 示例

机制示意（可点追问）：

```visual
<svg viewBox="0 0 680 200" width="100%">
  <g class="tone-blue">
    <rect class="node" x="24" y="70" width="150" height="56" rx="8"/>
    <text class="t t-mid" x="99" y="98">客户端请求</text>
  </g>
  <g class="tone-amber clickable" id="cache">
    <rect class="node" x="265" y="70" width="150" height="56" rx="8"/>
    <text class="t t-mid" x="340" y="91">缓存层</text>
    <text class="t-sm t-mid" x="340" y="110">命中率 92%</text>
  </g>
  <g class="tone-gray">
    <rect class="node" x="506" y="70" width="150" height="56" rx="8"/>
    <text class="t t-mid" x="581" y="98">数据库</text>
  </g>
  <line class="edge" x1="174" y1="98" x2="261" y2="98" marker-end="url(#arrow)"/>
  <line class="edge dashed" x1="415" y1="98" x2="502" y2="98" marker-end="url(#arrow)"/>
  <text class="t-sm t-mid" x="458" y="84">未命中</text>
</svg>
<script>
  document.getElementById('cache').addEventListener('click', () => ace.send('展开讲讲缓存层的失效策略'));
</script>
```

可交互讲解（滑块）：

```visual
<div class="card stack">
  <div class="row"><strong>重试等待时间</strong><span class="badge tone-blue" id="total"></span></div>
  <label class="small muted">重试次数 <input type="range" id="n" min="1" max="8" value="4"></label>
  <div class="grid" id="bars"></div>
</div>
<script>
  const n = document.getElementById('n');
  function draw() {
    const waits = Array.from({ length: +n.value }, (_, i) => 2 ** i * 100);
    document.getElementById('bars').innerHTML = waits
      .map((ms, i) => `<div class="card small">第 ${i + 1} 次<br><span class="mono">${ms} ms</span></div>`).join('');
    document.getElementById('total').textContent = `共 ${waits.reduce((a, b) => a + b, 0)} ms`;
  }
  n.addEventListener('input', draw);
  draw();
</script>
```

## 发出前自检

1. 代码块在顶层，语言是 `visual`，内容没有 `<html>` / `<body>`。
2. SVG 有 `viewBox` 和 `width="100%"`，没有固定高度；文字都在形状里，没有重叠。
3. 颜色全部来自变量或色调类；深色模式下也看得清。
4. 脚本在最后；没有 `position: fixed`、`vh`、网络图片；外链只来自允许的 CDN。
5. 图之外的文字只交代结论，没有复述图的内容。
