## ADDED Requirements

### Requirement: 空闲刷新释放资源
系统 SHALL 在侧栏刷新内容相同时避免状态写入，并在每次桌面桥接回包后释放对应 Promise 记录。

#### Scenario: 相同列表持续刷新
- **WHEN** 服务端重复返回相同的工作区与会话数据
- **THEN** 侧栏不因该数据重复写入 React 状态

#### Scenario: 桥接请求结束
- **WHEN** 桥接返回成功、失败或无效 JSON
- **THEN** 对应 Promise 被结束并从待处理表移除

### Requirement: 有界恢复渲染进程
Windows Desktop SHALL 在主渲染进程退出后自动重载，十分钟内最多三次，并在宿主销毁后停止恢复回调。

#### Scenario: 渲染进程内存耗尽
- **WHEN** 主渲染进程因内存耗尽退出且窗口内次数未达上限
- **THEN** 页面自动重载且桥接继续可用

#### Scenario: 连续崩溃
- **WHEN** 十分钟内第四次主渲染进程退出
- **THEN** 保留错误页且不再自动重载

#### Scenario: 关闭宿主
- **WHEN** 已排队恢复但宿主开始析构
- **THEN** 恢复回调不得访问宿主或重载页面
