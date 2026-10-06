## ADDED Requirements

### Requirement: 忙碌状态可读

办公室 SHALL 为忙碌的 agent 显示常驻状态气泡，内容包括动作和细节；正在输出正文时 SHALL 显示最新输出的文字并随输出更新，调用工具时 SHALL 显示动作、共源图标与调用预览。默认工作模式下没有正文进度事件时，系统 SHALL 由正文流推断输出状态。

#### Scenario: 默认模式下输出正文
- **WHEN** agent 在「用于编程」模式下开始输出正文
- **THEN** 办公室显示该 agent 在打字，气泡为「撰写回复」及最新几个字，并在两次快照之间持续更新

#### Scenario: 调用工具
- **WHEN** agent 开始调用读取文件工具
- **THEN** 气泡显示读取图标、「读取」和文件路径的末尾部分

### Requirement: 交接可见

成员加入、完成以及 agent 间消息 SHALL 以可见的交接动画表达：新成员 SHALL 从门口进入并在派活的 agent 处接收任务后入座；成功完成的成员 SHALL 先向派活的 agent 递交回报再离开；agent 间消息 SHALL 以带类型标签的信封在两者之间移动。减少动态效果偏好下 SHALL 省略走路但保留交接信封。

#### Scenario: 星状派活与回报
- **WHEN** 主 agent 派出子 agent，子 agent 随后成功完成
- **THEN** 子 agent 走到主 agent 桌前领取「任务」信封后入座，完成后回到主 agent 桌前递交「回报」信封并离开，主 agent 显示收到结果

### Requirement: macOS 桌面办公室

macOS 桌面端 SHALL 提供与 Windows 相同页面与桥协议的透明置顶办公室窗口，不抢主窗口焦点，透明区域点击穿透，支持拖动、缩放、大小记忆、右键菜单与隐藏。

#### Scenario: 透明区域点击
- **WHEN** 用户点击办公室窗口中房间、控制条和浮层之外的透明区域
- **THEN** 点击落到后面的窗口
