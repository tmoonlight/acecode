## Purpose

定义 ACECode 进程(TUI、daemon、headless)退出时的关停顺序,以及对可放弃后台工作的有界等待:退出过程不访问已释放的资源,不无限挂起,运行中的工作被有序中止。

## ADDED Requirements

### Requirement: 关停时先停会话再停共享运行时
TUI、daemon、headless 进程退出时,系统 MUST 按以下顺序关停:
1. 停止接收新的输入:HTTP / WebSocket 入口、TUI 输入、IM 通道;
2. 停止会产生新回合的后台组件:循环调度器、任务建议、远程控制绑定;
3. 停止全部会话:中止运行中的回合并等待其工作线程结束,包括子代理会话;
4. 关闭 MCP 服务器;
5. 关闭 LSP 服务器。

任何会话的回合 MUST NOT 在 MCP 或 LSP 已关闭之后继续执行工具调用。

#### Scenario: daemon 退出时有调用 MCP 工具的回合在运行
- **WHEN** 某会话的回合正在执行一个耗时的 MCP 工具调用,此时 Desktop 关闭并停止 daemon
- **THEN** 该回合先被中止,工具结果记为已中止,不再是「MCP 已关闭」这类错误
- **AND** 回合结束之后才关闭 MCP 与 LSP,daemon 正常退出,不出现进程崩溃或 fail-fast

#### Scenario: TUI 退出时有子代理在运行
- **WHEN** TUI 主会话派出的子代理正在运行,用户执行 `/exit`
- **THEN** 子代理会话在 MCP 与 LSP 关闭之前被停止
- **AND** TUI 正常退出并打印 resume 提示

#### Scenario: headless 退出顺序保持
- **WHEN** `acecode -p` 的回合结束,进程退出
- **THEN** 会话先于 MCP 与 LSP 结束,退出码语义不变

### Requirement: 退出时对可放弃的后台工作有界等待
进程退出时,系统 MUST 对可放弃的后台工作最多等待 2 秒,超时后继续退出。可放弃的后台工作包括:models.dev 注册表刷新、网页搜索区域探测、进行中的 MCP 工具调用与连接尝试、进行中的生图请求。该规则对 TUI、daemon、headless 三个入口都生效。

超时后才到达的结果 MUST 被丢弃,且处理过程 MUST NOT 访问已释放的资源。

#### Scenario: 退出时有卡住的 MCP 调用
- **WHEN** 一个 MCP 工具调用已被中止,但底层请求仍在等待服务器响应,此时用户退出
- **THEN** 进程最多额外等待约 2 秒后完成退出
- **AND** 退出之后该请求的迟到响应不会引发崩溃

#### Scenario: 启动后立即退出
- **WHEN** 进程启动后立刻退出,models.dev 刷新与区域探测仍在后台运行
- **THEN** 进程在 2 秒内完成退出,静态析构阶段不崩溃

#### Scenario: 没有后台工作
- **WHEN** 退出时没有任何进行中的可放弃后台工作
- **THEN** 退出不增加任何额外等待
