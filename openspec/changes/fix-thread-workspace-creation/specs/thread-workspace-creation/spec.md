## Purpose

让用户通过 AI 在当前归属、指定工作区或指定文件夹创建可在界面找到的独立会话，并能从原会话继续发送任务，避免将无工作区的临时执行目录误识别为普通工作区。

## ADDED Requirements

### Requirement: 显式选择创建目标

`create_thread` SHALL 接受可选 `workspaceHash`、`cwd`、`noWorkspace`。省略目标时继承调用会话归属；`workspaceHash` 指向已注册工作区，`cwd` 为已存在的绝对目录，`noWorkspace=true` 创建无工作区会话。系统 MUST 拒绝冲突目标、错误类型、未知工作区、相对路径及不存在或非目录路径，不能静默回退到当前目录。

#### Scenario: 指定其他工作区
- **WHEN** 从无工作区调用创建工具并提供已注册工作区的 hash
- **THEN** 新会话使用目标工作区 cwd、归属及存储位置

#### Scenario: 指定文件夹
- **WHEN** 提供已存在的绝对目录 cwd
- **THEN** 系统规范化该目录并确保目标工作区在界面可见，保留已有显示名称等配置

#### Scenario: 目标参数非法
- **WHEN** 同时提供 cwd 与 workspaceHash、noWorkspace=true 与目录目标，或目标类型/路径非法
- **THEN** 返回明确错误，不创建会话

### Requirement: 正确继承无工作区归属并返回位置

系统 SHALL 为无工作区新会话分配独立缓存目录，持久化 no_workspace，并返回 `threadId`、`cwd`、`workspaceHash` 和 `noWorkspace`；workspaceHash 使用与全局会话查询一致的存储项目 hash。普通会话目标 SHALL 注册为可见工作区，创建结果可通过对应界面列表发现。首条输入投递失败时 MUST 清理实际目标中的新会话文件。

#### Scenario: 无工作区调用默认创建
- **WHEN** 无工作区会话省略目标调用 create_thread
- **THEN** 新会话仍属于无工作区，可通过无工作区列表发现，且不会把调用方临时目录注册成工作区

#### Scenario: 普通工作区调用默认创建
- **WHEN** 普通会话省略目标调用 create_thread
- **THEN** 新会话使用调用方目录，并保留 title/model 与首条 prompt 行为

#### Scenario: 投递失败
- **WHEN** 新会话创建后首条输入无法排队
- **THEN** 销毁新会话并清理目标存储，不删除调用方历史

### Requirement: 跨工作区继续发送任务

`send_message_to_thread` SHALL 全局定位 threadId，并接受可选 workspaceHash 消除歧义。恢复非活跃目标时 MUST 使用目标自身 cwd 和 noWorkspace 属性。不能将请求发送给其他工作区中同 ID 的活跃会话；自发消息仍被拒绝。

#### Scenario: 新建后继续发送
- **WHEN** 原会话向刚创建的其他工作区或独立无工作区会话发送 prompt
- **THEN** 输入进入目标会话队列，目标归属保持不变

#### Scenario: 同 ID 冲突
- **WHEN** 目标 ID 对应多个存储项目且未指定 workspaceHash，或运行时已加载其他项目的同 ID
- **THEN** 返回明确错误，不把消息投递给错误会话
