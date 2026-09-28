<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

见 proposal.md。`SessionContentLoading` 使用绝对定位；恢复遮罩当前位于带 `position: relative` 的输入区内，记录遮罩位于会话主列内。9 月 17 日的旧设计为保留历史可读性而仅遮住输入区，这次按用户反馈统一加载位置。

## Goals / Non-Goals

**Goals:** 一个定位容器负责会话内容的加载反馈，两个异步阶段共同决定遮罩状态。

**Non-Goals:** 不调整恢复请求、并发池、订阅状态机、侧栏导航或现有加载卡片视觉样式。

## Decisions

- 将 `ChatView` 中两个加载组件合为会话主列的一个直接子节点。继续复用已有 `absolute inset-0` 定位，不以固定坐标或全窗口遮罩修补问题。
- 在现有 `sessionContentLoading.js` 中归并阶段：错误优先，其次记录读取，再次运行环境恢复，全部完成后不显示。只读外部会话不参与运行环境恢复判定。
- 保留 `sessionRuntimeUnavailable` 的发送和队列保护；不清空已加载历史。`App` 在 `resumePending` 时仍抑制外层侧栏加载遮罩。
- 结构回归检查实际 JSX 祖先和唯一挂载，避免仅检查组件名称而遗漏定位容器；浏览器验证窄屏、侧面板和输入区高度变化。

## Risks / Trade-offs

- 恢复期间会话主列统一被遮罩覆盖，历史数据仍保留，但需恢复完成后阅读和操作；侧栏仍可用于切换。
- 并行的工作台改动已修改 `ChatView.jsx`，只在导入和加载组件所在局部编辑，并核对其余已有改动未丢失。
- 源码和浏览器验证不等同于已安装 Desktop 客户端验证；本次不打包或发布。
