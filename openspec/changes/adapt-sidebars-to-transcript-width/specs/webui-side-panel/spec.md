## MODIFIED Requirements

### Requirement: Side panel layout in single-session view

Web UI 单会话视图 (`view === 'single'`) SHALL 提供右侧导航面板及详情预览，默认导航宽度 280 CSS px，允许使用用户保存的栏宽。是否正常占位 SHALL 服从 transcript 优先的响应式布局与用户保存的开关；窄窗口展开 SHALL 使用 popup。Grid4 / Grid9 视图下 SHALL NOT 显示该面板。

#### Scenario: 单会话视图渲染右侧面板
- **WHEN** 用户进入单会话视图且已开启右侧面板、空间充足
- **THEN** ChatView 右侧按保存宽度显示导航和已打开的详情

#### Scenario: 切到 4 宫格隐藏面板
- **WHEN** 用户从单会话视图切到 4 宫格视图
- **THEN** SidePanel 从单会话布局移除，grid 视图主区按既定布局填满

#### Scenario: tab 切换零延迟
- **WHEN** 用户点击变更导航
- **THEN** 面板立即展示已加载消息聚合的变更数据

#### Scenario: 切 session 时面板状态保留
- **WHEN** 用户选择同一工作区内另一个 session
- **THEN** 既有工作台状态隔离规则继续保留各自文件树缓存、展开目录和页签，不因响应式隐藏而清空

#### Scenario: cwd 真变时整面板 reset
- **WHEN** 用户切换到另一个 cwd
- **THEN** 系统按对应工作台加载导航和预览，不能显示其他 cwd 的残留
