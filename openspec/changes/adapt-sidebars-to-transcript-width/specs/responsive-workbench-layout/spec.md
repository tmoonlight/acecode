## Purpose

保证 ACECode 单会话工作台在窗口缩放时优先显示可阅读的 transcript，同时保留左右辅助栏的临时访问能力、用户保存的开关选择和栏宽，避免窗口缩放意外改变用户长期布局。

## ADDED Requirements

### Requirement: Transcript-first responsive collapse
系统 SHALL 根据 transcript 可用宽度决定辅助栏是否占位。所有字号阅读下限 SHALL 统一为 350 CSS px；恢复展开 SHALL 额外保留 64 px。右侧导航与预览及打开的子对话占宽 MUST 参与判断。空间不足 SHALL 先收右侧工作区，再收左栏；整个窗口不足阅读下限时 transcript SHALL 使用可用空间而不是撑出视口。显式最大化预览和演示模式 SHALL 保留原行为。

#### Scenario: 连续缩小再拉大
- **WHEN** 用户从足够宽的窗口逐渐缩小并重新拉宽
- **THEN** 右栏先收起、左栏后收起，恢复按左栏优先且 transcript 仍可读的条件进行，临界宽度的小幅抖动不反复切换

#### Scenario: 打开预览造成拥挤
- **WHEN** 窗口宽度不变但预览或子对话占用更多空间
- **THEN** 系统重新判断 transcript 的空间并按相同顺序释放辅助栏占宽

#### Scenario: 统一 350 px 折叠边界
- **WHEN** 不同字号下，正常占位的辅助栏使 transcript 可用宽度逐渐缩至 350 CSS px，再继续变窄
- **THEN** 恰好 350 CSS px 时保持占位，低于该值时按右栏优先的顺序折叠；已折叠栏需满足额外 64 px 余量才能恢复

#### Scenario: 原生窗口允许拖动到单列布局
- **WHEN** 用户拖动 Desktop 窗口边缘缩小宽度
- **THEN** 原生窗口 SHALL 允许缩到 480 逻辑像素，能够触发左右栏自动折叠及单列 transcript，不能以旧多栏布局的最小宽度阻止缩小

### Requirement: Preserve persistent layout intent
系统 SHALL 独立保存用户左右栏开关及栏宽；自动折叠、浮层开关和恢复 SHALL NOT 修改这些偏好。宽度恢复 SHALL 仅恢复之前开启的栏。刷新、重启和切工作台 SHALL 使用既有持久化作用域中的偏好，不能继承其他工作台的临时 popup。

#### Scenario: 原本关闭的栏临时查看
- **WHEN** 原本关闭的栏在窄窗口中通过 popup 打开，随后窗口拉宽或刷新
- **THEN** 该栏仍然关闭，已保存开关与栏宽不变

#### Scenario: 原本开启的栏自动恢复
- **WHEN** 原本开启的栏因空间不足自动收起，随后恢复门槛满足
- **THEN** 该栏恢复正常占位，预览页签与草稿仍然保留

### Requirement: Temporary sidebar popup
在空间不足以正常展开时，expand SHALL 打开对应侧 popup，不改变 transcript 的布局宽度。popup SHALL 限于可用视口，左右互斥；栏内操作和其菜单 SHALL 保持可用；点击栏外、再次点击对应按钮或 Escape SHALL 收起。内层菜单或对话框处理 Escape 时 SHALL 优先关闭内层。

#### Scenario: 点击内部与外部
- **WHEN** 用户在窄窗口打开一个边栏并点击其中的菜单、滚动或选择内容
- **THEN** 栏正常响应且不会因栏内点击被收起；点击栏外时 popup 收起，持久化偏好不变

#### Scenario: 切换两侧与键盘关闭
- **WHEN** 一个 popup 打开时用户打开另一侧或按 Escape
- **THEN** 打开另一侧时关闭当前侧；Escape 关闭当前 popup 并将键盘焦点还给相应展开按钮

#### Scenario: 窄右栏中的详情和文件列表
- **WHEN** 右侧 popup 不能并排容纳详情和导航
- **THEN** 用户仍可在浮层内查看详情和打开导航选择文件，浮层不超出窗口

### Requirement: Resizable sidebar popups
左右 popup SHALL 支持拖动靠 transcript 一侧的边缘调整宽度，并支持手柄上的键盘方向键。调整 SHALL 保持浮动，不改变 transcript 几何和正常占位栏宽或开关；手动浮层宽度 SHALL 独立持久化。浮层 SHALL 保留可点击的外部区域并遵守当前视口边界；窗口临时变窄 SHALL NOT 覆盖保存的浮层宽度。拖动取消、关闭、切会话或失焦后 SHALL 清理拖动状态。

#### Scenario: 拖宽详情浮层
- **WHEN** 用户向左拖动右侧详情浮层的左边缘
- **THEN** 浮层变宽，右边缘保持贴边，transcript 宽度和草稿不变；向右拖动则变窄，过程中不会自动收起或恢复占位

#### Scenario: 保存浮层宽度并适配视口
- **WHEN** 用户调整左栏或右栏浮层宽度后关闭、重开或刷新，再缩小和恢复窗口
- **THEN** 浮层恢复用户选择的宽度，空间不足时仅临时夹取到视口范围；正常栏宽与开关不变

#### Scenario: 跨边界拖动和取消
- **WHEN** 拖动指针移出浮层，随后松开、取消或关闭浮层
- **THEN** 拖动持续到终止事件，终止后不残留拖动光标、监听或意外的宽度更新
