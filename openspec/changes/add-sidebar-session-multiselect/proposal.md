# Proposal

## Why

左侧会话展开后的整块背景不利于识别树形层级，整理多条会话需要重复操作。按 2026-10-21 用户提供的截图，恢复缩进树线并支持范围选择与批量整理。

## What Changes

- 父子节点以缩进和细树线区分，移除展开分组背景，保留行级选中和悬停反馈。此决定覆盖 `collapse-scheduled-sessions` 设计决策 7 的无缩进及整块背景约定。
- 单击建立锚点，Shift 连选当前可见会话，Ctrl/Cmd 增减选择；隐藏节点和分组标题不加入范围。
- 多选拖动显示最多三条叠放预览和总数，整体排序或移入/移出定时分组。
- 复用现有右键菜单与操作实现，支持批量置顶/取消置顶、标记未读/已读、归档，准确反馈部分失败。

## Capabilities

### New Capabilities

- `sidebar-session-selection`: 会话树视觉、多选、批量菜单与拖动语义。

### Modified Capabilities

无。已有 `sidebar-session-collapse` 描述根列表分页，不改变该行为。

## Impact

涉及 Sidebar、侧栏 CSS、选择纯函数、指针拖动、现有菜单委托，以及 session-sidebar 移动请求对 sources 数组的兼容扩展。复用现有置顶、状态、归档接口，不引入依赖。保持两层树及工作区数据边界，普通树批量拖动限定同一范围，置顶区域继续支持跨范围排序。
