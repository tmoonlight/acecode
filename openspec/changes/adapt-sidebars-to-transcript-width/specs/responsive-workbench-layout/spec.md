## Purpose

保证 ACECode 单会话工作台在窗口缩放时通过自动折叠左栏为 transcript 让出空间，同时保持右栏手动开关、左栏临时浮层及用户保存的布局偏好。

## ADDED Requirements

### Requirement: Transcript-first responsive collapse
系统 SHALL 根据 transcript 可用宽度决定左栏是否占位。所有字号折叠阈值 SHALL 统一为 350 CSS px；恢复展开 SHALL 额外保留 64 px。实际开启的右侧导航、预览及子对话占宽 MUST 参与判断；手动关闭的右侧内容 MUST NOT 预留宽度。空间不足 SHALL 仅自动收起左栏，SHALL NOT 自动收起右栏或将其转为 popup。左栏收起后仍不足阅读下限时 SHALL 保持右栏手动选择，由 transcript 使用剩余空间。显式最大化预览和演示模式 SHALL 保留原行为。

#### Scenario: 连续缩小再拉大
- **WHEN** 用户从足够宽的窗口逐渐缩小并重新拉宽
- **THEN** 只有左栏按空间不足及恢复余量切换，临界宽度的小幅抖动不反复切换；右栏保持用户手动选择的显示状态

#### Scenario: 打开预览造成拥挤
- **WHEN** 窗口宽度不变但预览或子对话占用更多空间
- **THEN** 系统重新判断 transcript 的空间并仅自动折叠左栏，右侧内容保持显示

#### Scenario: 统一 350 px 折叠边界
- **WHEN** 不同字号下，正常占位的辅助栏使 transcript 可用宽度逐渐缩至 350 CSS px，再继续变窄
- **THEN** 恰好 350 CSS px 时左栏保持占位，低于该值时左栏自动折叠；已折叠左栏需满足额外 64 px 余量才能恢复

#### Scenario: 原生窗口允许拖动到单列布局
- **WHEN** 用户已手动关闭右栏，并拖动 Desktop 窗口边缘缩小宽度
- **THEN** 原生窗口 SHALL 允许缩到 480 逻辑像素，能够触发左栏自动折叠及单列 transcript，不能以旧多栏布局的最小宽度阻止缩小

### Requirement: Preserve persistent layout intent
系统 SHALL 独立保存用户左右栏开关及栏宽；左栏自动折叠、浮层开关和恢复 SHALL NOT 修改这些偏好。宽度恢复 SHALL 仅恢复之前开启的左栏。右栏手动开关 SHALL 始终更新既有偏好。刷新、重启和切工作台 SHALL 使用既有持久化作用域中的偏好，不能继承其他工作台的临时 popup。

#### Scenario: 原本关闭的栏临时查看
- **WHEN** 原本关闭的左栏在窄窗口中通过 popup 打开，随后窗口拉宽或刷新
- **THEN** 该栏仍然关闭，已保存开关与栏宽不变

#### Scenario: 原本开启的栏自动恢复
- **WHEN** 原本开启的左栏因空间不足自动收起，随后恢复门槛满足
- **THEN** 该栏恢复正常占位，预览页签与草稿仍然保留

### Requirement: Temporary sidebar popup
在空间不足以正常展开左栏时，左侧 expand SHALL 打开 popup，不改变 transcript 的布局宽度。popup SHALL 限于可用视口；栏内操作和其菜单 SHALL 保持可用；点击栏外、再次点击左侧按钮或 Escape SHALL 收起。内层菜单或对话框处理 Escape 时 SHALL 优先关闭内层。右侧 expand SHALL 始终正常展开右栏而非触发 popup。

#### Scenario: 点击内部与外部
- **WHEN** 用户在窄窗口打开左栏 popup 并点击其中的菜单、滚动或选择内容
- **THEN** 栏正常响应且不会因栏内点击被收起；点击栏外时 popup 收起，持久化偏好不变

#### Scenario: 右栏操作与键盘关闭
- **WHEN** 左栏 popup 打开时用户切换右侧开关或按 Escape
- **THEN** 切换右侧开关时关闭左栏 popup 并执行手动开关；Escape 关闭左栏 popup 并将键盘焦点还给左侧展开按钮

#### Scenario: 窄窗口中的右侧详情和文件列表
- **WHEN** 用户缩小窗口或在窄窗口手动展开详情和文件列表
- **THEN** 右侧内容保持正常占位布局和手动开关语义，不自动隐藏或进入浮层

### Requirement: Resizable sidebar popups
左栏 popup SHALL 支持拖动右边缘调整宽度，并支持手柄上的键盘方向键。调整 SHALL 保持浮动，不改变 transcript 几何和正常占位栏宽或开关；手动浮层宽度 SHALL 独立持久化。浮层 SHALL 保留可点击的外部区域并遵守当前视口边界；窗口临时变窄 SHALL NOT 覆盖保存的浮层宽度。拖动取消、关闭、切会话或失焦后 SHALL 清理拖动状态。

#### Scenario: 拖宽左栏浮层
- **WHEN** 用户向右拖动左栏浮层的右边缘
- **THEN** 浮层变宽，左边缘保持贴边，transcript 宽度不变；向左拖动则变窄，过程中不会自动收起或恢复占位

#### Scenario: 保存浮层宽度并适配视口
- **WHEN** 用户调整左栏浮层宽度后关闭、重开或刷新，再缩小和恢复窗口
- **THEN** 浮层恢复用户选择的宽度，空间不足时仅临时夹取到视口范围；正常栏宽与开关不变

#### Scenario: 跨边界拖动和取消
- **WHEN** 拖动指针移出浮层，随后松开、取消或关闭浮层
- **THEN** 拖动持续到终止事件，终止后不残留拖动光标、监听或意外的宽度更新
