<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

App 渲染 TopBar，ChatView 持有会话、轨迹和侧边聊天状态。DesktopContextMenu 已统一处理会话、项目和文件菜单。顶栏当前高度 41 px，按钮 24 px，macOS 非全屏左侧预留 80 px。

## Goals / Non-Goals

将会话标题、工作区和操作显示在顶栏同一行，保留原操作所有者、图标语言、原生拖动与三灯行为。首页不显示会话标题；独立 ChatView 保留自身标题的回退。菜单保留权限、确认框与当前会话上下文。

## Decisions

App 将 TopBar 的标题和操作 DOM 插槽显式传给 ChatView，SessionTitleBar 使用 React portal；状态与回调继续归 ChatView 所有，卸载或回首页时自然清理。

竖三点是会话操作的最后一个按钮，紧邻控制台。侧边聊天和查找通过共享菜单的显式打开事件提供回调，现有会话操作从原上下文目标生成，不复制归档等实现。菜单图标复用原有单色圆润 SVG。

右侧功能组和窗口控制是两个 flex 子组，间距精确为 40 px。macOS 不产生空的窗口按钮组。标题允许截断，窄屏工作区徽标可隐藏。

菜单分隔线作为按钮的相邻元素绘制，颜色来自 border token，不继承危险操作颜色或按钮圆角。菜单按实际尺寸限制视口并支持键盘选择。

顶部功能按钮与窗口按钮使用同一层级的 CSS hover/focus 底块，避免透明背景的无 layer 规则压过 Tailwind hover。会话操作中移除额外面板恢复按钮，面板开关保留在顶栏最右侧。

共享菜单在 window 的 pointerdown 捕获阶段判断外部按下，同步关闭后再允许后续原生拖动或按钮事件继续；不依赖可能被原生窗口拖动吞掉的 click。菜单内部按下不关闭，原有 click 关闭作为键盘/程序激活的补充。

## Risks / Trade-offs

Portal 不改变 React 事件归属，需要验证拖动、点击和菜单焦点不会互相干扰。原生 macOS 无本机实测条件，仅验证网页布局及桥接模拟，不宣称完成原生验收。

## Validation

验证标题/按钮同一行、40 px 间距、菜单查找/聊天/原操作、只读会话、切换与卸载、直线分隔、390 px 中英文和 macOS 全屏避让。运行 pnpm test、pnpm build、图标再生成检查、严格 OpenSpec 验证和 diff 检查。
