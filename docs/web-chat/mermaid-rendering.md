# Web 对话里的 Mermaid 渲染

回复里的 ```` ```mermaid ```` 代码块在 Web / 桌面版里画成图。这份文档讲这条链路上每一层做什么、哪些地方不能改松、改完怎么验证。TUI 的字符画渲染是另一套实现,不在这里。

## 一张图从源码到画面

1. **Markdown**([web/src/lib/markdown.js](../../web/src/lib/markdown.js)):围栏完整(有收尾)的 mermaid 代码块输出成一个图框,源码放在里面;没写完的(流式输出中、回复被截断)是普通代码块。引用块、列表里的围栏同样算。围栏漏写语言名,或者把图类型当语言名写(```` ```flowchart ````、```` ```mmd ````)时,内容第一行是五种图类型的开头也当图画;写了别的语言名(text、markdown)的是有意展示源码,不算。缩进四个空格的围栏按 Markdown 规则是缩进代码块,认不出来。
2. **源码检查与改写**([web/src/lib/mermaidSource.js](../../web/src/lib/mermaidSource.js)):`inspectMermaidSource` 决定画不画,`prepareMermaidRenderSource` 生成真正交给 Mermaid 的文本。
3. **解析**:官方 Mermaid 解析。报错时由 [web/src/lib/mermaidRepair.js](../../web/src/lib/mermaidRepair.js) 修一遍再解析一次。
4. **模型检查**:`inspectMermaidModel` 检查 Mermaid 解析出来的图模型。
5. **绘制与 SVG 检查**([web/src/lib/mermaidRenderer.js](../../web/src/lib/mermaidRenderer.js)):Mermaid 画出 SVG,`sanitizeMermaidSvg` 按白名单检查,通过后只以 `<img>` 显示。

复制按钮、导出、放大预览里的「源码」始终是模型写的原文,改写和修复只影响交给 Mermaid 的那份。

## 源码检查的原则

只在 Mermaid 当成语法处理的位置做检查,标签里的文字一律当文字。有风险的写法能拿掉而图还成立的,拿掉后照画;拿不掉的才拒绝。合法的图交给 Mermaid 的文本一个字都不改。

| 写法 | 处理 |
| --- | --- |
| `%%{init: ...}%%` 指令、开头的 `---` front matter | 换成等量空行(行号不变),图用界面自己的主题 |
| 标签里的 HTML 标签(`<img ...>`、`List<String>`) | 改写成 `#lt;` / `#gt;`,图上显示尖括号;`<br>` 保留 |
| 标签里的 HTML 实体(`&lt;`、`&#60;`、`&quot;`) | 换成 Mermaid 的实体码 |
| 类图 `<<interface>>`、状态图 `<<choice>>` / `<<fork>>` / `<<join>>` | 整行只有这个标记时原样保留 |
| `click` 语句,类图的 `link` / `callback`,时序图的 `link` / `links` / `properties` / `details` | 整句换成空行 |
| 时序图消息、注释、分组标题里的 `;` `#` | 换成 `#59;` `#35;` |
| 样式语句(style / classDef / linkStyle / cssClass,时序图 rect / box)里的 `url()`、`expression()`、`@import` 等 | 拒绝 |
| `@{ ... }` 新式形状 / 参与者类型 | 拒绝(流程图里引号内的不算) |
| 五种以外的图类型(pie、gantt、mindmap ...) | 拒绝 |
| 超过 64 KiB / 1000 行、带控制字符 | 拒绝 |

标签里出现 `behavior:`、`@Import`、`@media`、`expression (`、`handlers[i](req)`、`$$`、`fa:fa-user`、地址和协议名,节点名叫 call / click / link,类成员叫 `call()`:这些都是文字,不拦。以前按「全文出现某个词」拦,讲代码的图大量被误拒。

## 两条不能松的线

**一、绘制前不许有会让浏览器去取资源的东西。** Mermaid 是在当前页面里绘制的(要量文字尺寸),模型里只要有带地址的图片,浏览器在绘制过程中就会发请求,等画完再检查 SVG 已经晚了。模型的回复可能受到注入内容的影响,这条通道能把会话里的内容带出去。会当场取资源的有:

- 时序图 `properties 参与者: {"icon": "地址"}`;
- 流程图 `@{ img: "地址" }`;
- 标签里的 `<img>` 等标签;
- 样式里的 `url(...)`(`mask-image`、`background-image` 等)。

按写法拦是拦不全的。实测 `properties` 只按「语句开头」丢掉时,下面这些写法照样让页面发出了请求:

```text
end properties A: {"icon": "https://..."}        分组结束后同一行接着写
} properties A: {"icon": "https://..."}          accDescr { ... } 收尾后同一行接着写
```

所以绘制前还有 `inspectMermaidModel`:它检查的是 Mermaid 自己解析出来的模型(任何 `img` 有值、任何 `properties` 带 `icon`、任何名字带 style 的字段下面出现 `url()` / `@import` / `expression()`),与写法无关。**这一步不能删,也不能挪到绘制之后。** 真正的 mermaid 取不到模型时一律不画。

**二、交给 Mermaid 的文本里不能有 HTML 标签。** `neutralizeMermaidMarkup` 把所有 `<字母` 改写掉,闸门再按改写后的文本自检一遍。例外只有不带属性的 `<br>` 和整行的 `<<名字>>` 标记(名字里只许字母数字下划线,带不了属性)。

显示这一侧的边界是 Mermaid 的 strict 配置 + `sanitizeMermaidSvg` 白名单(不许 `foreignObject`、`image`、`a`,属性值里不许带协议或 `//`)+ 只以 `<img>` 显示。

## 解析失败后的自动修复

模型最常见的错不是写了危险的东西,而是语法小错,最多的是标签没加引号却带了括号。`repairMermaidSource` 只在官方解析器报错之后运行,修完重新解析(修过的文本还要再过一遍闸门和模型检查),仍然不行就显示源码。

| 图 | 修什么 |
| --- | --- |
| 流程图 | 节点标签、连线文字包进引号(里面的引号换成 `#quot;`);`A -- 文字 --> B` 换成 `A -->|"文字"| B`;当节点名用的关键字(`end`、`default`、`style`、`class`、`graph` ...)改名并保留显示文字;带空格或符号的子图标题换成 `内部名["标题"]`;语句后面的 `%% 注释` 去掉 |
| 时序图 | 分组缺的 `end` 补上、多的去掉;激活对不上时去掉全部激活标记(消息都在) |
| 状态图 | 带连字符的状态名声明成别名 |
| 类图 | 带 `::` 或连字符的类名包上反引号;成员行里的花括号换成实体码 |
| ER 图 | 关系后面漏了说明的补一个空说明 |

修复只补引号、改名、补 `end`,不会造出新的语句。

流程图 / 类图 / 状态图 / ER 图的标签是用 SVG 文字画的,Mermaid 在这条路径上只把 `#lt;` `#gt;` `#amp;` 还原成字符,`#quot;`、`#35;` 会原样显示成 `&quot;`。`sanitizeMermaidSvg` 在检查通过后把文字节点里剩下的实体码还原(只改文字,不产生标记)。

## 画不出来时

源码照旧显示,下方多一行说明,图框上的 `data-mermaid-error` 记着卡在哪一层和原因。问题反馈过来时往往只有一张照片或截图,从图上要能看出是哪一层。

| 说明 | 卡在哪 |
| --- | --- |
| 第 N 行语法有误 / 图表语法有误 | 官方解析器报错,修也没修好。行号按源码算 |
| 暂不支持这种图表类型 | 五种以外的图 |
| 图表含有不支持的写法 | 闸门或模型检查拒绝 |
| 图表过大 | 超过大小 / 行数上限 |
| 图表组件加载失败(后面带「重新加载」按钮) | mermaid 组件没取到(页面开着的时候前端被换了新版本)。按钮重新加载页面,桌面版里 F5 不管用 |
| 图表绘制失败 | Mermaid 绘制时抛错 |
| 图表绘制结果未通过检查 | 画出来的 SVG 没过白名单 |

## 改完怎么验证

`pnpm test` 里的 Node 测试(`mermaidSource.test.js`、`mermaidRepair.test.js`、`mermaidRenderer.test.js`)只能断言改写后的文本:流程图、状态图、类图的真解析和所有绘制都需要 DOM。动了这条链路,要再跑一次真浏览器验证:

```powershell
$env:ACE_PLAYWRIGHT_MODULE = '<playwright 的 index.mjs>'; node scripts/test-mermaid-rendering.mjs
```

在 `web/` 目录下运行。它用 [web/scripts/mermaid-rendering-cases.mjs](../../web/scripts/mermaid-rendering-cases.mjs) 里的样例检查:常见写法画得出来而且图上的文字对(「出了 SVG」不等于「画对了」,修复改坏标签时只有这一项能发现)、该拒绝的卡在预期的那一层、恶意写法不会让页面向外发请求。遇到新的「没画出来」的反馈,先把那张图(换掉业务名字)加进样例,再改代码。

「不向外发请求」目前只在 Edge(Chromium)上验证过。别的内核对样式里的外部引用处理可能不同,所以样式里的 `url()` 是在绘制之前拦的,不依赖某个浏览器恰好不去取。

2026-11-23 这次改动前后的数字:本机会话记录里收集到的 29 张图,26 → 29 张画出来;按模型常见写法整理的 154 条五种图样例,100 → 147 条(剩下 7 条是有意拒绝或 Mermaid 本身不接受的写法)。
