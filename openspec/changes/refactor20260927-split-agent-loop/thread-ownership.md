# 一期 AgentLoop 线程归属

本文记录已拆出的状态与加锁方向,最终验收结果另记。当前所有用例均等待一期整体实现后的 Windows 验证。

| 状态/入口 | 可进入线程 | 同步与调用边界 |
| --- | --- | --- |
| AgentTaskQueue 入队/控制回执 | worker、TUI、HTTP/API、跨会话交接 | queue 锁保护两个 deque、任务状态和回执顺序;扫描不复制队列。 |
| ActiveTurnGate steer/interject/interrupt | UI/API、worker 结束边界 | gate → queue → AbortSignal;不得从 queue 回取 gate。interject 在 gate 内先完成问题响应,再记录已接受输入。 |
| TaskHandoff | 当前会话控制调用 | source.queue → target.queue;释放两个队列锁后再发送转录/Goal 事件。 |
| ConversationHistory | worker;空闲恢复/控制调用 | 只有该类改写消息数组;空闲入口要求 !busy 且当前线程是 worker 或持有队列门,过渡期违例只告警。 |
| TrajectoryRecorder | 事件投递线程 | 独占 EventDispatcher 单槽 observer;销毁时卸载并通过 LifetimeToken 等待在途回调,不强持有宿主。 |
| WorkspaceBoundary | worker、并行只读工具 | cwd/文件夹/边界配置按值取快照;state 锁外访问 SessionManager、文件系统或其它协作对象。 |
| SessionExecSecurity | worker、并行工具、空闲配置控制 | 规则、审计接收器与拒绝反馈在 state 叶子锁下取值;审计调用与文件 IO 在锁外;SandboxRuntime 自有锁。 |
| SafeEditGuard | 写工具/后续 shell 检查 | 叶子锁只保护失败路径及时间;检查文件编码在锁外。十分钟有效期与显式绕过条件保持原样。 |
| GoalRuntime account_usage/emit_updated/emit_cleared | worker、并行只读工具经 ToolContext 回调 | cursor 叶子锁只保护 goal/thread ID、时间游标及通知去重;不跨 store、emit、回调或 queue.with_locked。 |
| GoalRuntime notify_objective_updated | TUI、daemon 控制线程 | 只写 atomic 待处理标志;worker 在下次请求前消费。 |
| AgentHookBridge apply/drain | worker、并行工具回调 | context 锁内追加或 swap;格式化、发送消息、运行 Hook 全部在锁外。 |
| AgentHookBridge Stop 状态 | worker | stop_active 跨回合保留;清理 request context 不重置 Stop 状态。 |
| PermissionHookSession | 单次同步工具执行作用域 | 请求输入按值固定,解决标志在回调前置位;正常离开补 implicit,异常展开不补新 Hook 事件。不得复制或被异步回调保留。 |

| SideQuestionService | API、每请求独立线程 | context 叶子锁内按值复制;provider/callback 在锁外,线程捕获自有共享状态;停机先抑制回调,再回收并等待线程。 |
| ActivityNarrator | worker、并行工具、配置线程 | 叶子锁保护阶段/批次/去重状态;配置与状态一次锁内读取,回调可重入查询,不持锁发送。 |
| AgentProgressEmitter | worker、并行工具 | shared_ptr 保证发射器状态寿命;750ms 节流锁外调用 narrator 与 EventDispatcher,时钟按回合取值。 |
| RetryProgressReporter | provider 回调、压缩、PA 重试 | 无可变状态;先状态回调再进度事件,三路径共享载荷出口。 |

A-10 至 A-14 继续在本表追加旁路问答、进度、请求、工具批次与最终装配的线程归属。Windows 本轮不声称执行 Linux TSan。
