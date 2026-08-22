## ADDED Requirements

### Requirement: 普通 Web 启用 ACECode 自定义右键菜单
ACECode Web UI SHALL 在普通浏览器直连模式中接管适用区域的 `contextmenu`，并复用与 Desktop 相同的对象识别、菜单构建和动作分发逻辑。

#### Scenario: 普通 Web 的对象目标显示业务动作
- **WHEN** 用户在普通浏览器中右键会话、工作区、文件、变更、消息、工具输出或附件目标
- **THEN** UI SHALL 显示该目标已有的 Web 可执行 ACECode 菜单动作

#### Scenario: 普通 Web 的可编辑目标显示通用动作
- **WHEN** 用户在普通浏览器中右键可编辑文本目标
- **THEN** UI SHALL 显示适用的全选、复制、粘贴和剪切动作

#### Scenario: 控制台终端保留专属菜单
- **WHEN** 用户在 `.ace-console-term` 内右键
- **THEN** 全局 ACECode 菜单 SHALL NOT 拦截该事件，并 SHALL 由控制台终端自身处理

### Requirement: 普通 Web 过滤 native-only 动作
ACECode Web UI MUST 在菜单展示和动作执行两个边界阻止普通浏览器使用依赖 Desktop/native 能力的动作。

#### Scenario: 普通 Web 隐藏文件管理器动作
- **WHEN** 普通浏览器中的目标携带绝对路径、定位路径或资源管理器元数据
- **THEN** 菜单 SHALL NOT 包含 `OPEN_IN_EXPLORER` 或 `LOCATE_FILE`

#### Scenario: 普通 Web 隐藏原生检查动作
- **WHEN** 普通浏览器开启前端调试标志但没有 Desktop 原生能力
- **THEN** 菜单 SHALL NOT 包含 `INSPECT`

#### Scenario: 普通 Web 隐藏依赖原生目录选择器的会话导出
- **WHEN** 普通浏览器用户右键带有稳定标识的会话目标
- **THEN** 菜单 SHALL NOT 包含 `EXPORT_SESSION`

#### Scenario: 绕过展示层的 native 动作不被执行
- **WHEN** 普通 Web 的旧菜单状态或其他调用路径尝试执行 native-only 动作
- **THEN** UI MUST 拒绝该动作且 MUST NOT 调用 Desktop bridge 或文件管理器 REST 回退

### Requirement: Desktop 与 WebApp 保留既有宿主动作
Desktop Shell 和 Edge WebApp 兼容模式 SHALL 在具备现有宿主能力时继续显示并执行既有 native 菜单动作。

#### Scenario: Desktop Shell 保留资源管理器动作
- **WHEN** Desktop Shell 用户右键带有有效本地路径的对象
- **THEN** 菜单 SHALL 保留适用的资源管理器打开或定位动作

#### Scenario: Edge WebApp 保留既有 REST 回退
- **WHEN** Edge WebApp 兼容模式用户执行资源管理器动作
- **THEN** UI SHALL 继续使用现有 daemon REST 回退，不得因普通 Web 过滤而移除该动作

#### Scenario: Desktop 与 WebApp 保留原生目录选择导出
- **WHEN** Desktop Shell 或 Edge WebApp 兼容模式用户右键可导出的会话
- **THEN** 菜单 SHALL 保留既有 `EXPORT_SESSION` 动作

### Requirement: 能力过滤保持菜单结构稳定
菜单构建 SHALL 在应用运行能力过滤后生成确定的动作顺序和分组分隔线。

#### Scenario: native 动作移除后重新计算分隔线
- **WHEN** 普通 Web 过滤一个或多个 native-only 动作
- **THEN** 剩余 Web 动作 SHALL 保持原有相对顺序且 SHALL NOT 出现由已移除动作遗留的空分组或错误分隔线

#### Scenario: Web 动作不被过度过滤
- **WHEN** 某动作通过前端状态、CustomEvent、浏览器能力或现有通用 daemon API 完成
- **THEN** 普通 Web 菜单 SHALL 保留该动作，即使 Desktop 模式存在更优先的原生实现
