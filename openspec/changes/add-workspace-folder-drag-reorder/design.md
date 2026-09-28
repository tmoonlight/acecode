<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

动机见 proposal.md。`Sidebar.jsx` 每轮刷新以服务端数组覆盖工作区；现有 `workspaceDragRef` 仅负责工作区内会话。`WorkspaceRegistry::list()` 当前来自无序映射，服务端和 Desktop bridge 共用它。`usePreference` 仅依赖 origin 内 localStorage，而桌面 daemon 使用动态端口，不能满足重启恢复。

## Goals / Non-Goals

**Goals:** 文件夹排序独立于会话排序；共享注册表成为持久化顺序的唯一来源；拖动事务拥有完整取消、点击抑制和竞态处理。

**Non-Goals:** 不修改会话归属、置顶顺序、工作区激活和点击展开策略；不迁移其他 UI 偏好。

## Decisions

1. 在 projects 根目录单独原子保存工作区 hash 顺序，注册表在锁内验证可见工作区完整排列后提交。保留隐藏 hash 的原有位置，新 hash 追加；列表和 Desktop bridge 统一读取该顺序。避免逐个改 workspace.json 引入部分写入，也避免 localStorage 因端口变化失效。
2. 新增 `PUT /api/workspaces/order`，请求 `{hashes: string[]}`，成功返回确认的 `{hashes}`。格式错误返回 400，非当前可见集合排列返回 409，保存失败返回 500。前端发送期间拒绝重复拖动，失败回滚并刷新。
3. 拖动逻辑放入专用 hook；以标题行为起点、整个分组边界为目标，Pointer Events 跨过小位移阈值才启动，排除按钮、输入和非主指针。独立引用与现有会话拖动互斥。
4. 浮层使用 portal、fixed 和 translate3d，复用文件夹图标、名称、字体和表面/阴影 token，保留抓取点；源分组淡化占位。插入线与端点放在整个目标分组边界。即时跟随指针，不增加过渡动画。
5. 使用 requestAnimationFrame 处理边缘滚动与动态几何。Escape、blur、pointercancel、lostpointercapture、卸载、列表身份顺序变化或侧栏收起统一清理；拦截拖动后点击，普通点击继续展开收起。
6. 前端排序纯函数保留对象身份；乐观提交用修订号标记，避免旧轮询响应覆盖新顺序。键盘 Alt+方向键复用同一提交路径。

## Risks / Trade-offs

- [风险] 后端和 Desktop 各自持有注册表快照 → 扫描时读取同一原子排序文件。
- [风险] 工作区在拖动期间增删 → 前端取消变化中的拖动，后端校验排列并在冲突后刷新。
- [风险] 原生菜单或任务按钮触发拖动 → 排除交互控件，保留原行为并在浏览器中验证。
- [风险] 当前工作区有无关未提交修改 → 精确增量编辑并对比修改前快照。

## Migration Plan

首次没有排序文件时保留现有列表；首次成功排序才写入。损坏文件按无排序处理，不影响工作区可见性。回滚本次代码后工作区元数据和会话仍完整。
