<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

动机见 proposal.md。globals.css 存在 5px 普通条、10px transcript、10px 代码预览以及自绘标签条；PPT 使用独立 iframe。xterm 初始化时缓存轨道宽度，因此悬停时改变真实轨道宽度会破坏布局与列数。

## Goals / Non-Goals

统一应用自有滚动表面的外观和命中范围，不重写浏览器滚动、终端输入或标签拖拽算法。不控制浏览器 PDF 插件和用户网页内容。

## Decisions

- 新建共享 `styles/scrollbars.css`，由 globals.css 引用，PPT 组件通过 raw 文本将同一规则传给 iframe 文档生成器；避免两份样式漂移。
- 细条轨道固定 12px，透明边框将滑块视觉宽度保持为 4px，悬停或拖动时仅加深颜色。透明边框属于原生滑块命中区域，容器宽度不因 hover 改变；自绘标签滚动条同样保留 12px 命中区域和 4px 滑块厚度。
- 默认隐藏，容器 hover 或 focus-within 时显现；`.ace-scrollbar-always` 或 `data-scrollbar-visibility="always"` 可明确选择常显。主 transcript 始终使用 10px 粗轨道，保留已有流式锚定行为。
- 清除会绕开 Chromium WebKit 自定义规则的局部 `scrollbar-color` 和 `scrollbar-width: thin`；标准属性仅用于无 WebKit 滚动条的浏览器降级。
- 标签条沿用既有自绘拖拽及点击逻辑，尺寸和颜色引用共享 token。下层编辑预览与紧凑标签导航的结构性隐藏继续保留。
- 无 hover 的触摸设备显示细条以保持可发现性；自绘动画遵守 reduced-motion。

## Risks / Trade-offs

- 固定较宽轨道会占用更多初始空间，但 hover 不重新布局。检查输入框、窄屏、代码预览、终端与电子表格。
- 原生 hover 伪状态不能仅用 getComputedStyle 证明；浏览器验证须开启可见滚动条并通过截图和透明边缘拖动验证。
- Firefox 等标准属性降级遵循平台原生交互，无法保证与 Chromium 完全相同的滑块悬停视觉宽度。
